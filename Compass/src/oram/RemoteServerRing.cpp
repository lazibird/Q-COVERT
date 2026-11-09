#include "RemoteServerRing.h"
// #include "utils_uring.cpp"
#include <iostream>
#include <string>
#include <sstream>
#include <cassert>
#include <omp.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <chrono>
#include <omp.h>
#include <cstring> 
#include <iomanip> // For std::hex, std::setw, std::setfill

#include "evp.h"

#define NUM_THREADS 4
#define SHA256_DIGEST_LENGTH 32

enum RequestType {
    Read,
    Write,
    ReadBatch,
	WriteBatch,
	WriteBatch_R,
	ReadBatchBlock,
	ReadBatchBlock_R,
	ReadBatchBlockXor,
	WriteBatchBlock,
	Init,
	End // End the remote server
};

size_t get_sibling3(size_t me){
	if(me % 2 == 1){
		// odd
		return me + 1;
	} else if (me == 0){
		return 0;
	} else{
		return me - 1;
	}
}

inline double interval(std::chrono::_V2::system_clock::time_point start){
    auto end = std::chrono::high_resolution_clock::now();
    auto interval = (end - start)/1e+9;
    return interval.count();
}

RemoteServerRing::RemoteServerRing(NetIO* io, size_t capacity, size_t bucket_size, bool in_memory, bool integrity, RingOramConfig oram_config)
    : io(io), capacity(capacity), bucket_size(bucket_size), in_memory(in_memory), integrity(integrity), oram_config(oram_config){

	assert(in_memory);
	// assert(!integrity);

	block_size = SBucket::getCipherSize();
	data = new unsigned char [capacity * bucket_size * block_size];

	// for(int i = 0; i < capacity*bucket_size; i++){
	// 	SBucket* sbkt = new SBucket(integrity);
	// 	buckets.push_back(sbkt);
	// }

	enable_tree_hash = false;

    cout << "> Remote storage server config: " << endl;
    cout << " -> in memory: " << in_memory << endl;
    cout << " -> integrity: " << integrity << endl;
	cout << " -> # buckets: " << capacity << endl;
	cout << " -> # Z: " << bucket_size << endl;
	cout << " -> block size: " << block_size << endl;
	cout << " -> total: " << capacity * bucket_size * block_size << endl;
    // cout << endl;
}

void RemoteServerRing::RunServer(){
    RunServerInMemory();
}

// void RemoteServerRing::sync_root(){
// 	io->send_data(root.data(), SHA256_DIGEST_LENGTH*sizeof(uint8_t));
// }

void RemoteServerRing::send_hash(std::vector<int> &position, std::vector<int> &offset){

	// cout << "send_hash..." << endl;
	
	size_t num_hashes = position.size() * (per_bucket_tree_height - 1);
	uint8_t* inner_mt_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];

	// send hashes for inner mt for each bucket
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < position.size(); i++){
		size_t pos = position[i];
		size_t cur_offset = offset[i] + (per_bucket_hashes + 1) / 2 - 1;
		for(int cur_height = per_bucket_tree_height; cur_height > 1; cur_height --){
			size_t sib_offset = get_sibling3(cur_offset);
			uint8_t* src = per_bucket_hash + pos * per_bucket_hashes * SHA256_DIGEST_LENGTH + sib_offset * SHA256_DIGEST_LENGTH;
			uint8_t* dst = inner_mt_payload + i * (per_bucket_tree_height - 1) * SHA256_DIGEST_LENGTH + (per_bucket_tree_height - cur_height) * SHA256_DIGEST_LENGTH;
			memcpy(dst, src, SHA256_DIGEST_LENGTH);

			// if(i == 0){
			// 	cout << "sib_hash for height - " << cur_height << " cur_offset: " << cur_offset << ": " ;
			// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 		cout << (int)(dst[j]) << " ";
			// 	}
			// 	cout << endl;
			// }
			cur_offset = (cur_offset - 1) / 2;
		}
	}

	io->send_data(inner_mt_payload, num_hashes*SHA256_DIGEST_LENGTH);

	// send hashes for outer mt 
	uint8_t* outer_mt_payload = new uint8_t[position.size() *SHA256_DIGEST_LENGTH];
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < position.size(); i++){
		size_t pos = position[i];
		size_t sib_pos = get_sibling3(pos);
		memcpy(
			outer_mt_payload + i*SHA256_DIGEST_LENGTH,
			tree_hash + sib_pos*SHA256_DIGEST_LENGTH,
			SHA256_DIGEST_LENGTH
		);

		uint8_t* pos_hash = tree_hash + pos*SHA256_DIGEST_LENGTH;

	}
	io->send_data(outer_mt_payload, position.size()*SHA256_DIGEST_LENGTH);

	// cout << "send_hash done..." << endl;
	// assert(0);

	delete[] outer_mt_payload;
	delete[] inner_mt_payload;
}

void RemoteServerRing::send_hash_reshuffle(std::vector<int> &position, std::vector<int> &offset){
	
	// cout << "send hash reshuffle" << endl;

	size_t num_buckets = position.size() / oram_config.real_bucket_size;
	std::vector<int> bucket_positions;

	for(size_t i = 0 ; i < num_buckets; i++){
		bucket_positions.push_back(position[i*oram_config.real_bucket_size]);
	}

	size_t num_hashes = num_buckets * oram_config.bucket_size;
	uint8_t* inner_leaves_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];

	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0 ; i < num_buckets; i++){
		int bucket_pos = bucket_positions[i];
		uint8_t* src = per_bucket_hash + (bucket_pos * per_bucket_hashes + first_leaf_off)* SHA256_DIGEST_LENGTH;
		uint8_t* dst = inner_leaves_payload + i * oram_config.bucket_size* SHA256_DIGEST_LENGTH;
		memcpy(dst, src, oram_config.bucket_size* SHA256_DIGEST_LENGTH);
	}

	io->send_data(inner_leaves_payload, num_hashes*SHA256_DIGEST_LENGTH);

	vector<int> inner_hash_pos;
	vector<int> outer_hash_pos;
	for(int i = 0; i < num_buckets; i++){
		int pos = bucket_positions[i];
		bucket_positions.push_back(pos);

		if(pos >= capacity / 2){
			// leaf bucket
		} else{
			int l = 2*pos + 1;
			int r = l + 1;
			outer_hash_pos.push_back(l);
			outer_hash_pos.push_back(r);
		}


		int pos_iter = pos;
		// loop ancestors
		while(pos_iter >= (1 << oram_config.cached_levels) - 1){
			outer_hash_pos.push_back(get_sibling3(pos_iter));
			inner_hash_pos.push_back(pos_iter);

			pos_iter = (pos_iter - 1) / 2;
		}
	}

	// std::cout << "inner hash pos: ";
	// for (const auto& element : inner_hash_pos) {
	// 	std::cout << element << ' ';
	// }
	// std::cout << '\n';

	// std::cout << "outer hash pos: ";
	// for (const auto& element : outer_hash_pos) {
	// 	std::cout << element << ' ';
	// }
	// std::cout << '\n';

	// assert(0);

	size_t inner_hash_size = inner_hash_pos.size();
	size_t outer_hash_size = outer_hash_pos.size();

	uint8_t* inner_hash_payload = new uint8_t[inner_hash_size*SHA256_DIGEST_LENGTH];
	uint8_t* outer_hash_payload = new uint8_t[outer_hash_size*SHA256_DIGEST_LENGTH];

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < outer_hash_size; i++){
		int pos = outer_hash_pos[i];
		uint8_t* src = tree_hash + pos*SHA256_DIGEST_LENGTH;
		uint8_t* dst = outer_hash_payload + i*SHA256_DIGEST_LENGTH;
		memcpy(dst, src, SHA256_DIGEST_LENGTH);
	}

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < inner_hash_size; i++){
		int pos = inner_hash_pos[i];
		uint8_t* src = per_bucket_hash + pos*per_bucket_hashes*SHA256_DIGEST_LENGTH;
		uint8_t* dst = inner_hash_payload + i*SHA256_DIGEST_LENGTH;
		memcpy(dst, src, SHA256_DIGEST_LENGTH);
	}

	io->send_data(inner_hash_payload, inner_hash_size*SHA256_DIGEST_LENGTH);
	io->send_data(outer_hash_payload, outer_hash_size*SHA256_DIGEST_LENGTH);

	delete[] inner_leaves_payload;
	delete[] outer_hash_payload;
}


void RemoteServerRing::send_hash_bucket(std::vector<int> &position, std::vector<int> &offset){


	size_t num_buckets = position.size() / oram_config.real_bucket_size;
	std::vector<int> bucket_positions;

	for(size_t i = 0 ; i < num_buckets; i++){
		bucket_positions.push_back(position[i*oram_config.real_bucket_size]);
	}

	// cout << "send_hash bucket with bucket_size: " << num_buckets << endl;	

	size_t num_hashes = num_buckets * oram_config.bucket_size;
	uint8_t* inner_leaves_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];

	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0 ; i < num_buckets; i++){
		int bucket_pos = bucket_positions[i];
		uint8_t* src = per_bucket_hash + (bucket_pos * per_bucket_hashes + first_leaf_off)* SHA256_DIGEST_LENGTH;
		uint8_t* dst = inner_leaves_payload + i * oram_config.bucket_size* SHA256_DIGEST_LENGTH;
		memcpy(dst, src, oram_config.bucket_size* SHA256_DIGEST_LENGTH);
	}

	io->send_data(inner_leaves_payload, num_hashes*SHA256_DIGEST_LENGTH);

	// cout << "send_hash bucket for outer mt" << endl;	

	// send hashes for outer mt 
	uint8_t* outer_mt_payload = new uint8_t[num_buckets *SHA256_DIGEST_LENGTH];

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < bucket_positions.size(); i++){
		size_t pos = bucket_positions[i];
		size_t sib_pos = get_sibling3(pos);
		memcpy(
			outer_mt_payload + i*SHA256_DIGEST_LENGTH,
			tree_hash + sib_pos*SHA256_DIGEST_LENGTH,
			SHA256_DIGEST_LENGTH
		);

		// uint8_t* pos_hash = tree_hash + pos*SHA256_DIGEST_LENGTH;

		// if(i < 5){
		// 	cout << "pos: " << pos << ": ";
		// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
		// 		cout << (int)(pos_hash[j]) << " ";
		// 	}
		// 	cout << endl;

		// 	uint8_t* sib_hash = tree_hash + sib_pos*SHA256_DIGEST_LENGTH;
		// 	cout << "sib: " << sib_pos << ": ";
		// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
		// 		cout << (int)(sib_hash[j]) << " ";
		// 	}
		// 	cout << endl;

		// 	uint8_t* bucket_hash = per_bucket_hash + pos * per_bucket_hashes * SHA256_DIGEST_LENGTH;
		// 	cout << "bucket hash: " << pos << ": ";
		// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
		// 		cout << (int)(bucket_hash[j]) << " ";
		// 	}
		// 	cout << endl;
		// }

	}

	io->send_data(outer_mt_payload, num_buckets*SHA256_DIGEST_LENGTH);


	// for(size_t i = 0 ; i < num_buckets; i++){
	// 	uint8_t* bucket_hash = per_bucket_hash + (i * per_bucket_hashes)* SHA224_DIGEST_LENGTH;
	// 	cout << "reconstructed hash: " << position[i*oram_config.real_bucket_size] << ": ";
	// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
	// 		cout << (int)(bucket_hash[j]) << " ";
	// 	}
	// 	cout << endl;
	// }

	// assert(0);

	// cout << "send_hash bucket done..." << endl;
	// assert(0);

	delete[] outer_mt_payload;
	delete[] inner_leaves_payload;

	// assert(0);
}

void RemoteServerRing::update_hash(std::vector<int> &position, uint8_t* payload){
	size_t num_buckets = position.size();
	size_t num_blocks = num_buckets * oram_config.bucket_size;

	// hash for each block
	uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * num_blocks];

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < num_blocks; i++){
		sha256_wrapper(payload + i*SBucket::getCipherSize(), SBucket::getCipherSize(), per_block_hash + i*SHA256_DIGEST_LENGTH);

		// cout << "bid: " << i << ": ";
		// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
		// 	cout << (int)((per_block_hash + i*SHA256_DIGEST_LENGTH)[j]) << " ";
		// }
		// cout << endl;
	}

	// assert(0);

	// Merkle Trees for each bucket
	// size_t inner_mt_size = num_buckets * per_bucket_hashes * SHA256_DIGEST_LENGTH;
	// uint8_t* inner_mt = new uint8_t[inner_mt_size];
	// memset(inner_mt, 0, inner_mt_size);

	// copy leaf hashes
	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < num_buckets; i++){
		size_t pos = position[i];
		uint8_t* src = per_block_hash + i * oram_config.bucket_size * SHA256_DIGEST_LENGTH;
		uint8_t* dst = per_bucket_hash + (pos * per_bucket_hashes +  first_leaf_off) * SHA256_DIGEST_LENGTH;
		memcpy(dst, src, oram_config.bucket_size * SHA256_DIGEST_LENGTH);
	}

	// reconstruct inner mt
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0 ; i < num_buckets; i++){
		size_t pos = position[i];
		for(size_t j = first_leaf_off; j >= 1; j--){
			size_t id = j-1;
			size_t lid = 2*j-1;
			size_t rid = 2*j;

			assert(lid <=  per_bucket_hashes - 1);

			uint8_t* dst = per_bucket_hash + (pos * per_bucket_hashes + id)* SHA256_DIGEST_LENGTH;
			uint8_t* lsrc = per_bucket_hash + (pos * per_bucket_hashes + lid)* SHA256_DIGEST_LENGTH;
			uint8_t* rsrc = per_bucket_hash + (pos * per_bucket_hashes + rid)* SHA256_DIGEST_LENGTH;

			sha256_twin_input_wrapper(
				lsrc, SHA256_DIGEST_LENGTH,
				rsrc, SHA256_DIGEST_LENGTH,
				dst
			);
		}
	}


	// reconstruct outer mt
	for(size_t i = 0; i < num_buckets; i++){
		size_t bucket_pos = position[i];

		uint8_t* bucket_hash = per_bucket_hash + bucket_pos * per_bucket_hashes * SHA256_DIGEST_LENGTH;
		uint8_t* tree_bucket_hash = tree_hash + bucket_pos * SHA256_DIGEST_LENGTH;

		if(bucket_pos >= capacity / 2){
			// leaf bucket
			memcpy(tree_bucket_hash, bucket_hash, SHA256_DIGEST_LENGTH);

			// cout << "leaf: " << bucket_pos << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(tree_bucket_hash[j]) << " ";
			// }
			// cout << endl;
			// cout << endl;

		} else{
			size_t l = 2*bucket_pos + 1;
			size_t r = l + 1;
			// auto lit = hash_map.find(l);
			// auto rit = hash_map.find(r);

			// if(lit == hash_map.end() || rit == hash_map.end()){
			// 	// This shouldn't happen
			// 	assert(0);
			// }

			uint8_t* lhash = tree_hash + l * SHA256_DIGEST_LENGTH;
			uint8_t* rhash = tree_hash + r * SHA256_DIGEST_LENGTH;

			sha256_trib_input_wrapper(
				bucket_hash, SHA256_DIGEST_LENGTH,
				lhash, SHA256_DIGEST_LENGTH,
				rhash, SHA256_DIGEST_LENGTH,
				tree_bucket_hash
			);

			// cout << "update bucket_hash: " << bucket_pos << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(bucket_hash[j]) << " ";
			// }
			// cout << endl;

			// cout << "update lit: " << l << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(lhash[j]) << " ";
			// }
			// cout << endl;

			// cout << "update rit: " << r << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(rhash[j]) << " ";
			// }
			// cout << endl;

			// cout << "update reconstructed: " << bucket_pos << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(tree_bucket_hash[j]) << " ";
			// }
			// cout << endl;
			// cout << endl;
		}

		// hash_map[bucket_pos] = vector<uint8_t>(tree_bucket_hash, tree_bucket_hash+SHA256_DIGEST_LENGTH);
		// delete[] tree_bucket_hash;
	}

	delete[] per_block_hash;
	// delete[] inner_mt;

	// for(size_t i = (1 << oram_config.cached_levels) - 1; i < (1 << (oram_config.cached_levels + 1)) - 1; i++){
	// 	uint8_t* hash = tree_hash + i * SHA256_DIGEST_LENGTH;

	// 	cout << "root_hash: " << i << ": ";
	// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
	// 		cout << (int)(hash[j]) << " ";
	// 	}
	// 	cout << endl;

	// }

	// assert(0);

}

void RemoteServerRing::update_hash_reshuffle(std::vector<int> &position, uint8_t* payload){

	// cout << "update_mt_reshuffle" << endl;
	// assert(0);

	size_t num_buckets = position.size();
	size_t num_blocks = num_buckets * oram_config.bucket_size;

	// hash for each block
	uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * num_blocks];

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < num_blocks; i++){
		sha256_wrapper(payload + i*SBucket::getCipherSize(), SBucket::getCipherSize(), per_block_hash + i*SHA256_DIGEST_LENGTH);

		// cout << "bid: " << i << ": ";
		// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
		// 	cout << (int)((per_block_hash + i*SHA256_DIGEST_LENGTH)[j]) << " ";
		// }
		// cout << endl;
	}

	// assert(0);

	// Merkle Trees for each bucket
	// size_t inner_mt_size = num_buckets * per_bucket_hashes * SHA256_DIGEST_LENGTH;
	// uint8_t* inner_mt = new uint8_t[inner_mt_size];
	// memset(inner_mt, 0, inner_mt_size);

	// copy leaf hashes
	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;
	for(size_t i = 0; i < num_buckets; i++){
		size_t pos = position[i];
		uint8_t* src = per_block_hash + i * oram_config.bucket_size * SHA256_DIGEST_LENGTH;
		uint8_t* dst = per_bucket_hash + (pos * per_bucket_hashes +  first_leaf_off) * SHA256_DIGEST_LENGTH;
		memcpy(dst, src, oram_config.bucket_size * SHA256_DIGEST_LENGTH);
	}

	// reconstruct inner mt
	for(size_t i = 0 ; i < num_buckets; i++){
		size_t pos = position[i];
		for(size_t j = first_leaf_off; j >= 1; j--){
			size_t id = j-1;
			size_t lid = 2*j-1;
			size_t rid = 2*j;

			assert(lid <=  per_bucket_hashes - 1);

			uint8_t* dst = per_bucket_hash + (pos * per_bucket_hashes + id)* SHA256_DIGEST_LENGTH;
			uint8_t* lsrc = per_bucket_hash + (pos * per_bucket_hashes + lid)* SHA256_DIGEST_LENGTH;
			uint8_t* rsrc = per_bucket_hash + (pos * per_bucket_hashes + rid)* SHA256_DIGEST_LENGTH;

			sha256_twin_input_wrapper(
				lsrc, SHA256_DIGEST_LENGTH,
				rsrc, SHA256_DIGEST_LENGTH,
				dst
			);
		}
	}

	for(size_t i = 0; i < num_buckets; i++){
		int bucket_pos = position[i];

		int pos_iter = bucket_pos;
		// loop ancestors
		while(pos_iter >= (1 << oram_config.cached_levels) - 1){
			uint8_t* bucket_hash = per_bucket_hash + pos_iter * per_bucket_hashes * SHA256_DIGEST_LENGTH;
			uint8_t* tree_bucket_hash = tree_hash + pos_iter * SHA256_DIGEST_LENGTH;

			if(pos_iter >= capacity / 2){
				memcpy(tree_bucket_hash, bucket_hash, SHA256_DIGEST_LENGTH);
			} else{
				size_t l = 2*pos_iter + 1;
				size_t r = l + 1;

				uint8_t* lhash = tree_hash + l * SHA256_DIGEST_LENGTH;
				uint8_t* rhash = tree_hash + r * SHA256_DIGEST_LENGTH;

				sha256_trib_input_wrapper(
					bucket_hash, SHA256_DIGEST_LENGTH,
					lhash, SHA256_DIGEST_LENGTH,
					rhash, SHA256_DIGEST_LENGTH,
					tree_bucket_hash
				);

			}
			pos_iter = (pos_iter - 1) / 2;
		
		}
	}

	// for(size_t i = (1 << oram_config.cached_levels) - 1; i < (1 << (oram_config.cached_levels + 1)) - 1; i++){
	// 	uint8_t* hash = tree_hash + i * SHA256_DIGEST_LENGTH;

	// 	cout << "root_hash: " << i << ": ";
	// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
	// 		cout << (int)(hash[j]) << " ";
	// 	}
	// 	cout << endl;

	// }

	// assert(0);
}

void RemoteServerRing::RunServerInMemory(){
	// cout << "Remote storage server running ..." << endl;
    
	while(1) {
		int rt;
		io->recv_data(&rt, sizeof(int));
		switch (rt){
			case Read:{
                // Legacy branch, disabled
				assert(0);
				break;
			}
			case Write:{
                // Legacy branch, disabled
				assert(0);
				break;
			}
			case ReadBatchBlock_R:
			case ReadBatchBlock: {
				size_t num_blocks;
				io->recv_data(&num_blocks, sizeof(size_t));

				// auto t_fetch = std::chrono::high_resolution_clock::now();
				
				std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks);
				io->recv_data(offset.data(), sizeof(int)*num_blocks);

				size_t len = num_blocks * (block_size);
				// cout << "ReadBucketBatch allocate payload for size: " << len << endl;
				unsigned char* payload = new unsigned char[len];
				// cout << "ReadBucketBatch allocate done" << endl;

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_blocks; bucket_id++){
					size_t bucket_pos = position[bucket_id]*bucket_size + offset[bucket_id]; 
					size_t bucket_offset = bucket_id * block_size;
					// this->buckets[bucket_pos]->data_to_ptr(payload + bucket_offset);
					unsigned char* tmp_data = data + bucket_pos*block_size;
					mempcpy(payload + bucket_offset, tmp_data, block_size);
				} 

				// cout << "ReadBucketBatch write to payload done" << endl;

				io->send_data(payload, sizeof(unsigned char)*len);


				if(integrity){
					if(rt == ReadBatchBlock){
						// for bucket read
						send_hash_bucket(position, offset);
					} else{
						// for reshuffle
						send_hash_reshuffle(position, offset);
					}
					// send_hash(position, offset);
				}

				// cout << "ReadBucketBatch send to client done" << endl;

				delete[] payload;

				break;
			}
			
			case ReadBatchBlockXor:{
				// cout << "ReadBatchBlockXor" << endl;
				size_t num_blocks;
				size_t num_real_blocks;
				io->recv_data(&num_blocks, sizeof(size_t));
				io->recv_data(&num_real_blocks, sizeof(size_t));
				
				std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks);
				io->recv_data(offset.data(), sizeof(int)*num_blocks);

				size_t path_len = num_blocks / num_real_blocks;

				// no need for ivs
				size_t len = num_real_blocks * (block_size - 16);

				unsigned char* payload = new unsigned char[len];
				unsigned char* ivs = new unsigned char[num_blocks*16];
				std::memset(payload, 0, len); 

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t block_id = 0; block_id < num_real_blocks; block_id++){
					size_t bucket_offset = block_id * (block_size - 16);
					for(int i = 0; i < path_len; i++){
						size_t bucket_id = block_id * path_len + i;
						size_t bucket_pos = position[bucket_id]*bucket_size + offset[bucket_id]; 
						// this->buckets[bucket_pos]->data_xor_to_ptr(payload + bucket_offset);

						unsigned char* ptr = payload + bucket_offset;
						unsigned char* tmp_data = data + bucket_pos*block_size;
						
						// first 16 goes to iv
						memcpy(ivs + bucket_id*16, tmp_data, 16);

						// 16 - block_size goes to xor
						for(size_t j = 16; j < block_size; j++){
							ptr[j - 16] = tmp_data[j] ^ ptr[j - 16]; 
						}
					}
					
				}

				io->send_data(payload, sizeof(unsigned char)*len);
				io->send_data(ivs, sizeof(unsigned char)*num_blocks*16);

				if(integrity){
					send_hash(position, offset);
				}

				// cout << "ReadBatchBlockXor send to client done" << endl;

				delete[] payload;

				break;
			}
			case WriteBatchBlock:{
				assert(0);
				break;
			}
			case ReadBatch:{

				assert(0);

				size_t num_buckets;
				io->recv_data(&num_buckets, sizeof(size_t));

				// auto t_fetch = std::chrono::high_resolution_clock::now();
				
				std::vector<int> position(num_buckets);
				io->recv_data(position.data(), sizeof(int)*num_buckets);

				size_t len = num_buckets * bucket_size * (block_size);
				// cout << "ReadBucketBatch allocate payload for size: " << len << endl;
				unsigned char* payload = new unsigned char[len];
				// cout << "ReadBucketBatch allocate done" << endl;

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
					for(size_t block_id = 0; block_id < bucket_size; block_id++){
						size_t block_pos = position[bucket_id]*bucket_size + block_id;
						size_t block_offset = (bucket_id*bucket_size + block_id) * block_size;
						// this->buckets[block_pos]->data_to_ptr(payload + block_offset);
						unsigned char* tmp_data = data + block_pos*block_size;
						mempcpy(payload + block_offset, tmp_data, block_size);
					}
				} 

				io->send_data(payload, sizeof(unsigned char)*len);

				delete[] payload;
				break;
			}
			case WriteBatch_R:
			case WriteBatch:{

				size_t num_buckets;
				io->recv_data(&num_buckets, sizeof(size_t));

				std::vector<int> position(num_buckets);
				io->recv_data(position.data(), sizeof(int)*num_buckets);

				size_t len = num_buckets * bucket_size * (block_size);

				unsigned char* payload = new unsigned char[len];
				// cout << "WriteBatch allocate done" << endl;

				io->recv_data(payload, sizeof(unsigned char)*len);


				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
					for(size_t block_id = 0; block_id < bucket_size; block_id++){
						size_t block_pos = position[bucket_id]*bucket_size + block_id;
						size_t block_offset = (bucket_id*bucket_size + block_id) * block_size;
						// this->buckets[block_pos]->data_from_ptr(payload + block_offset);
						unsigned char* tmp_data = data + block_pos*block_size;
						mempcpy(tmp_data, payload + block_offset, block_size);
					}
				} 

				if(integrity){
					if(rt == WriteBatch){
						update_hash(position, payload);
					} else{
						update_hash_reshuffle(position, payload);
					}
					// size_t hash_payload_size = position.size() * per_bucket_hashes * SHA256_DIGEST_LENGTH;
					// uint8_t* hash_payload = new uint8_t[hash_payload_size];
					// io->recv_data(hash_payload, hash_payload_size);
					// #pragma omp parallel for num_threads(NUM_THREADS)
					// for(size_t i = 0 ; i < position.size(); i++){
					// 	memcpy(
					// 		per_bucket_hash + position[i] * per_bucket_hashes * SHA256_DIGEST_LENGTH,
					// 		hash_payload + i * per_bucket_hashes * SHA256_DIGEST_LENGTH,
					// 		per_bucket_hashes * SHA256_DIGEST_LENGTH
					// 	);
					// }
					// delete[] hash_payload;
				}
				
				delete[] payload;
				break;
			}
			
			case Init:{
				cout << "Remote storage server Initializing ..." ;
				assert(0);
				cout << " done!" << endl;
				break;
			}
			case End:{
				cout << "Remote storage server closing ..." << endl;
				return;
			}
			default:{
				assert(0);
			}
			
		}
	}
}

void RemoteServerRing::load_hash(const char* fname){
	per_bucket_tree_height = ceil(log10(bucket_size) / log10(2)) + 1;
	per_bucket_hashes = pow(2, per_bucket_tree_height) - 1;
	size_t per_bucket_hash_size = SHA256_DIGEST_LENGTH * per_bucket_hashes * capacity;

	per_bucket_hash = new uint8_t[per_bucket_hash_size];

	int fd = open(fname, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }

	ssize_t target_size = per_bucket_hash_size;
	ssize_t total_read = 0;
	ssize_t bytes_read;
	while (total_read < target_size) {
		bytes_read = read(fd, per_bucket_hash + total_read, target_size - total_read);
		if (bytes_read < 0) {
			// Handle error
			cout << "error"  << endl;
			perror("read failed");
			break;
		}
		if (bytes_read == 0) {
			// EOF reached
			cout << "eof"  << endl;
			break;
		}
		total_read += bytes_read;
	}

	enable_tree_hash = true;
	if(enable_tree_hash){
		tree_hash = new uint8_t[capacity * SHA256_DIGEST_LENGTH];
		target_size = capacity * SHA256_DIGEST_LENGTH;
		total_read = 0;

		while (total_read < target_size) {
			bytes_read = read(fd, tree_hash + total_read, target_size - total_read);
			if (bytes_read < 0) {
				// Handle error
				cout << "error"  << endl;
				perror("read failed");
				break;
			}
			if (bytes_read == 0) {
				// EOF reached
				cout << "eof"  << endl;
				break;
			}
			total_read += bytes_read;
		}

	}

	cout << "total_read: " << total_read << endl;

	assert(total_read == target_size);

	return;
}

void RemoteServerRing::sync_hash_2(){
	// Send the hashes of uncached_roots

	cout << "sync hash start ..." << endl;

	size_t oram_cached_levels = oram_config.cached_levels;

	size_t roots_length = (1 << oram_cached_levels)*SHA256_DIGEST_LENGTH;
	uint8_t* payload = new uint8_t[roots_length];

	// id of first hash
	size_t bucket_id = (1 << oram_cached_levels) - 1;

	for(size_t hid = 0; hid < (1 << oram_cached_levels); hid++){
		// cout << "sync bucket id: " << bucket_id << endl;
		memcpy(
			payload + hid*SHA256_DIGEST_LENGTH,
			tree_hash + bucket_id*SHA256_DIGEST_LENGTH,
			SHA256_DIGEST_LENGTH
		);
		bucket_id++;
	}

	io->send_data(payload, roots_length);

	cout << "sync hash start done" << endl;

	delete[] payload;
}

void RemoteServerRing::sync_hash(){
	// Send the root hash of every bucket back to the client

	cout << "sync hash start ..." << endl;

	uint8_t* payload = new uint8_t[capacity*SHA256_DIGEST_LENGTH];
	for(size_t bucket_id = 0; bucket_id < capacity; bucket_id++){
		memcpy(
			payload + bucket_id*SHA256_DIGEST_LENGTH,
			per_bucket_hash + bucket_id*per_bucket_hashes*SHA256_DIGEST_LENGTH,
			SHA256_DIGEST_LENGTH
		);
	}

	io->send_data(payload, capacity*SHA256_DIGEST_LENGTH);

	cout << "sync hash start done" << endl;

	delete[] payload;
}

void RemoteServerRing::load(const char* fname){
	int fd = open(fname, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }

    // File check
    
	// Read capacity
	int c;
	read(fd, &c, sizeof(int));
	if(c != capacity){
		assert(0);
	}
	// Read SBucket size
	int sb_size;
	read(fd, &sb_size,sizeof(int));
	if(sb_size != block_size){
		assert(0);
	}

	bool i;
	read(fd, &i, sizeof(bool));
	// if(i != integrity){
	// 	assert(0);
	// }

	// size_t mmap_size = capacity * bucket_size * block_size + sizeof(int)*2 + sizeof(bool);

	// // mmap optimization
	// // Memory-map the file with read-write permissions
    // unsigned char* mmap_data = static_cast<unsigned char*>(mmap(nullptr, mmap_size, PROT_READ, MAP_PRIVATE, fd, 0));
    // if (mmap_data == MAP_FAILED) {
    //     perror("Error mapping file");
    //     close(fd);
    //     assert(0);
    // }

	// if (madvise(mmap_data, mmap_size, POSIX_MADV_SEQUENTIAL) == -1) {
    //     perror("Error advising kernel");
    //     munmap(mmap_data, mmap_size);
    //     close(fd);
    //     assert(0);
    // }
	
	// unsigned char* mmap_payload = mmap_data + sizeof(int)*2 + sizeof(bool);

	// #pragma omp parallel num_threads(NUM_THREADS)
	// for(size_t i = 0; i < capacity; i++){
	// 	memcpy(data + i * bucket_size * block_size, mmap_payload + i * bucket_size * block_size, bucket_size * block_size);
	// }

	// munmap(mmap_data, mmap_size);


    if(in_memory){
		// read(fd, data, capacity * bucket_size * block_size);

		ssize_t target_size = capacity * bucket_size * block_size;
		ssize_t total_read = 0;
		ssize_t bytes_read;
		while (total_read < target_size) {
			bytes_read = read(fd, data + total_read, target_size - total_read);
			if (bytes_read < 0) {
				// Handle error
				cout << "error"  << endl;
				perror("read failed");
				break;
			}
			if (bytes_read == 0) {
				// EOF reached
				cout << "eof"  << endl;
				break;
			}
			total_read += bytes_read;
		}

		cout << "total_read: " << total_read << endl;

		assert(total_read == target_size);

        // for (size_t i = 0; i < capacity*bucket_size; i++){
        //     read(fd, this->buckets[i]->data, SBucket::getCipherSize());
        //     // if(integrity){
		// 	// 	read(fd, this->buckets[i]->hash, 32*sizeof(uint8_t));
		// 	// }
        // }

		// if(integrity){
		// 	for(int i = 0; i < SHA256_DIGEST_LENGTH; i++){
		// 		root[i] = this->buckets[0]->hash[i];
		// 	}
		// }

    } else {
		assert(0);
        // buckets_fname = (char *)malloc(strlen(fname) + 1);
	    // strcpy(buckets_fname, fname);
    }
    close(fd);
}
