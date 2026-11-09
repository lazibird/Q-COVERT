#include "RemotePath.h"
#include "evp.h"
#include <fcntl.h>

#include <cassert>

#define NUM_THREADS 4

enum RequestType {
    Read,
    Write,
    ReadBatch,
	WriteBatch,
	Init,
	End // End the remote server
};

int get_sibling_2(int me){
	if(me % 2 == 1){
		// odd
		return me + 1;
	} else if (me == 0){
		return 0;
	} else{
		return me - 1;
	}
}


RemotePath::RemotePath(NetIO* io, OramConfig oram_config, bool is_server, bool in_memory, bool integrity) 
    : io(io), is_server(is_server), in_memory(in_memory), integrity(integrity), oram_config(oram_config) {
	
    ptx_block_size = oram_config.block_size;
    ctx_block_size = SBucket::getCipherSize();
    capacity = oram_config.num_buckets;
    bucket_size = oram_config.bucket_size;

	root.resize(SHA256_DIGEST_LENGTH);

	if(is_server){
		for(int i = 0; i < capacity; i++){
			SBucket* sbkt = new SBucket(integrity);
			buckets.push_back(sbkt);
		}
	}
}

void RemotePath::load_server_state(const char* fname){
    int fd = open(fname, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }

    // File check
    
	// Write capacity
	int c;
	read(fd, &c, sizeof(int));
	if(c != capacity){
		assert(0);
	}
	// Write SBucket size
	int sb_size;
	read(fd, &sb_size,sizeof(int));
	if(sb_size != SBucket::getCipherSize()){
		cout << "sb_size: " << sb_size << endl; 
		cout << "get_cipher: " << SBucket::getCipherSize() << endl; 
		assert(0);
	}

	bool i;
	read(fd, &i, sizeof(bool));
	if(i != integrity){
		assert(0);
	}

    if(in_memory){
        for (size_t i = 0; i < capacity; i++){
            read(fd, this->buckets[i]->data, SBucket::getCipherSize());
            if(integrity){
				read(fd, this->buckets[i]->hash, 32*sizeof(uint8_t));
			}
        }

		if(integrity){
			for(int i = 0; i < SHA256_DIGEST_LENGTH; i++){
				root[i] = this->buckets[0]->hash[i];
			}
		}

    } else {
        assert(0);
        // buckets_fname = (char *)malloc(strlen(fname) + 1);
	    // strcpy(buckets_fname, fname);
    }
    close(fd);
}

void RemotePath::run_server(){
    if(in_memory){
        run_server_memory();
    } else{
        assert(0);
    }
}

void RemotePath::close_server(){
	int rt = End;
	io->send_data(&rt, sizeof(int));
}

void RemotePath::run_server_memory(){
	cout << "Remote storage server running ..." << endl;
    
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
			case ReadBatch:{

				size_t num_buckets;
				io->recv_data(&num_buckets, sizeof(size_t));

				// auto t_fetch = std::chrono::high_resolution_clock::now();
				
				std::vector<int> position(num_buckets);
				io->recv_data(position.data(), sizeof(int)*num_buckets);

				size_t len = num_buckets * (SBucket::getCipherSize());
				// cout << "ReadBucketBatch allocate payload for size: " << len << endl;
				unsigned char* payload = new unsigned char[len];
				// cout << "ReadBucketBatch allocate done" << endl;

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
					size_t bucket_pos = position[bucket_id]; 
					size_t bucket_offset = bucket_id * SBucket::getCipherSize();
					this->buckets[bucket_pos]->data_to_ptr(payload + bucket_offset);
				} 

				// cout << "ReadBucketBatch write to payload done" << endl;

				io->send_data(payload, sizeof(unsigned char)*len);

				// cout << "ReadBucketBatch send to client done" << endl;

				if(integrity){
					int hash_len = num_buckets * SHA256_DIGEST_LENGTH;
					unsigned char* hash_payload = new unsigned char[hash_len];

					#pragma omp parallel for num_threads(NUM_THREADS)
					for(int bucket_id = 0; bucket_id < num_buckets; bucket_id++){
						int bucket_pos = position[bucket_id]; 
						int bucket_offset = bucket_id * SHA256_DIGEST_LENGTH;
						this->buckets[get_sibling_2(bucket_pos)]->hash_to_ptr(hash_payload + bucket_offset);
					} 

					io->send_data(hash_payload, sizeof(unsigned char)*hash_len);
					delete[] hash_payload;
				}

				delete[] payload;
				break;
			}
			case WriteBatch:{


				size_t num_buckets;
				io->recv_data(&num_buckets, sizeof(size_t));

				// auto t_fetch = std::chrono::high_resolution_clock::now();

				std::vector<int> position(num_buckets);
				io->recv_data(position.data(), sizeof(int)*num_buckets);

				size_t len = num_buckets * (SBucket::getCipherSize());
				// cout << "WriteBatch allocate payload for size: " << len << endl;

				unsigned char* payload = new unsigned char[len];
				// cout << "WriteBatch allocate done" << endl;

				io->recv_data(payload, sizeof(unsigned char)*len);

				// auto t_write = interval(t_fetch);

				// cout << "WriteBatch allocate done" << endl;

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
					size_t bucket_pos = position[bucket_id]; 
					size_t bucket_offset = bucket_id * SBucket::getCipherSize();
					this->buckets[bucket_pos]->data_from_ptr(payload + bucket_offset);
				}

				if(integrity){
					int hash_len = num_buckets * SHA256_DIGEST_LENGTH;
					unsigned char* hash_payload = new unsigned char[hash_len];
					io->recv_data(hash_payload, sizeof(unsigned char)*hash_len);
					
					#pragma omp parallel for num_threads(NUM_THREADS)
					for(int bucket_id = num_buckets -1; bucket_id >= 0; bucket_id--){
						int bucket_pos = position[bucket_id]; 
						int bucket_offset = bucket_id * SHA256_DIGEST_LENGTH;
						this->buckets[bucket_pos]->hash_from_ptr(hash_payload + bucket_offset);
					}
					delete[] hash_payload;
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



void RemotePath::ReadBucketBatchAsBlock(const std::vector<int>& positions, std::vector<Block*>& blocks){

	// cout << "ReadBucketBatch: " << positions.size() << " " << SBucket::getCipherSize() << endl;
	
	int rt = ReadBatch;
	io->send_data(&rt, sizeof(int));

	size_t num_buckets = positions.size();
	io->send_data(&num_buckets, sizeof(size_t));
	io->send_data(positions.data(), sizeof(int)*num_buckets);
	// cout << "ReadBucketBatch num_buckets: " << num_buckets << endl;

	size_t len = num_buckets * (SBucket::getCipherSize());
	// cout << "ReadBucketBatch allocate payload for size: " << len << endl;
	unsigned char* payload = new unsigned char[len];
	// cout << "ReadBucketBatch allocate done" << endl;

	io->recv_data(payload, len);

	// cout << "ReadBucketBatch from server" << endl;

	if(integrity){
		int hash_len = num_buckets * SHA256_DIGEST_LENGTH;
		unsigned char* hash_payload = new unsigned char[hash_len];
		io->recv_data(hash_payload, hash_len);

		// Assume that position vector is sorted by layers. Layers close to bottom first
		for(int bucket_id = 0; bucket_id < num_buckets; bucket_id++){
			int pos = positions[bucket_id];

			// Insert sibling's hash
			int sib = get_sibling_2(pos);
			unsigned char* tmp = hash_payload + bucket_id*SHA256_DIGEST_LENGTH;
			// hash_map.insert(make_pair(sib, vector<uint8_t>(tmp, tmp + SHA256_DIGEST_LENGTH)));
			// cout << "Processing pos: " << pos << " and sib: " << sib << endl;

			if(!verify_and_insert(sib, tmp)){
				cout << "Hash verification fails at sib: " << sib << endl;
				assert(0);
			}

			// Compute and insert current hash
			unsigned char* ctx_payload = payload + bucket_id * SBucket::getCipherSize();
			uint8_t* hash = new uint8_t[SHA256_DIGEST_LENGTH];
			if( pos >= capacity / 2){
				// leaf
				sha256_wrapper(ctx_payload, SBucket::getCipherSize(), hash);
			} else{
				// non-leaf
				int l = 2*pos + 1;
				int r = l + 1;
				auto lit = hash_map.find(l);
				auto rit = hash_map.find(r);

				if(lit == hash_map.end() || rit == hash_map.end()){
					// This shouldn't happen
					assert(0);
				}
				sha256_trib_input_wrapper(
					ctx_payload, SBucket::getCipherSize(),
					lit->second.data(), SHA256_DIGEST_LENGTH,
					rit->second.data(), SHA256_DIGEST_LENGTH,
					hash
				);
			}
			if(!verify_and_insert(pos, hash)){
				cout << "Hash verification fails at pos: " << pos << endl;
				assert(0);
			}
			delete[] hash;
		}
		delete[] hash_payload;
	}

	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);
	blocks.resize(num_buckets * Bucket::getMaxSize());
	
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
		// printf("numThreads = %d\n", omp_get_num_threads()); 
		unsigned char* ctx_payload = payload + bucket_id * SBucket::getCipherSize();
		unsigned char* ptx_payload = new unsigned char[SBucket::getCipherSize()];
		size_t ptx_len = decrypt_wrapper(ctx_payload, SBucket::getCipherSize(), ptx_payload);
		// assert(ptx_len == per_block_size*Bucket::getMaxSize());
		unsigned char* tmp = ptx_payload;
		for(size_t block_id= 0; block_id < Bucket::getMaxSize(); block_id++){
			Block* b = new Block(ptx_block_size);
			b->from_ptr(tmp);
			tmp += per_block_size;
			blocks[block_id + bucket_id * Bucket::getMaxSize()] = b;
		}
		// bkts.push_back(std::move(init_bkt));
		delete[] ptx_payload;
	}

	// cout << "ReadBucketBatch done" << endl;

	delete[] payload;
}

void RemotePath::WriteBucketBatchMapAsBlock(const std::map<int, vector<Block*>>& bucket_to_write){

	int rt = WriteBatch;
	io->send_data(&rt, sizeof(int));

	size_t num_buckets = bucket_to_write.size();
	io->send_data(&num_buckets, sizeof(size_t));

	size_t len = num_buckets * (SBucket::getCipherSize());
	unsigned char* payload = new unsigned char[len];
	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);

	std::vector<int> positions;
	size_t bucket_id = 0;
	vector<vector<Block*>> bkts;
	for (const auto& pair : bucket_to_write) {
		positions.push_back(pair.first);
		bkts.push_back(pair.second);
	}

	#pragma omp parallel for num_threads(NUM_THREADS)
	for (int bucket_id = 0; bucket_id < positions.size(); bucket_id++) {
		unsigned char* ptx_payload = new unsigned char[SBucket::getCipherSize()];
		unsigned char* tmp = ptx_payload;
		for(Block* b : bkts[bucket_id]){
			b->to_ptr(tmp);
			tmp += per_block_size;
			delete b;
		}
		unsigned char* ctx_payload = payload + bucket_id * SBucket::getCipherSize();
		size_t ctx_len = encrypt_wrapper(ptx_payload, per_block_size*Bucket::getMaxSize(), ctx_payload);
		// assert(ctx_len == SBucket::getMaxSize());
		delete[] ptx_payload;
	}

	io->send_data(positions.data(), positions.size()*sizeof(int));
	io->send_data(payload, len);

	if(integrity){
		int hash_len = num_buckets * SHA256_DIGEST_LENGTH;
		unsigned char* hash_payload = new unsigned char[hash_len];

		// Compute new hashes
		for(int bucket_id = num_buckets - 1; bucket_id >= 0; bucket_id--){
			int pos = positions[bucket_id];

			uint8_t* hash = hash_payload + bucket_id * SHA256_DIGEST_LENGTH;
			unsigned char* ctx_payload = payload + bucket_id * SBucket::getCipherSize();
			if( pos >= capacity / 2){
				// leaf
				sha256_wrapper(ctx_payload, SBucket::getCipherSize(), hash);
			} else{
				// non-leaf
				int l = 2*pos + 1;
				int r = l + 1;
				auto lit = hash_map.find(l);
				auto rit = hash_map.find(r);

				if(lit == hash_map.end() || rit == hash_map.end()){
					// This shouldn't happen
					assert(0);
				}
				sha256_trib_input_wrapper(
					ctx_payload, SBucket::getCipherSize(),
					lit->second.data(), SHA256_DIGEST_LENGTH,
					rit->second.data(), SHA256_DIGEST_LENGTH,
					hash
				);
			}
			hash_map[pos] = vector<uint8_t>(hash, hash+SHA256_DIGEST_LENGTH);
		}

		io->send_data(hash_payload, hash_len);
		delete[] hash_payload;

		// Reset the hash map
		memcpy(root.data(), hash_map[0].data(), SHA256_DIGEST_LENGTH);
		hash_map.clear();
		hash_map.insert(make_pair(0, root));
	}

	delete[] payload;
}

// ----------------------------- hash ---------------------------------- //

void RemotePath::sync_root(){
	if(is_server){
		io->send_data(root.data(), SHA256_DIGEST_LENGTH*sizeof(uint8_t));
	} else{
		io->recv_data(root.data(), SHA256_DIGEST_LENGTH*sizeof(uint8_t));
		hash_map.insert(make_pair(0, root));
	}
}

bool RemotePath::verify_and_insert(int pos, uint8_t* hash){
	auto it = hash_map.find(pos);
	if(it == hash_map.end()){
		// This bucket is new to the merkle tree
		hash_map.insert(make_pair(pos, vector<uint8_t>(hash, hash + SHA256_DIGEST_LENGTH)));
	} else{
		vector<uint8_t> ref = it->second;
		for(int i = 0; i < SHA256_DIGEST_LENGTH; i++){
			if(ref[i] != hash[i]){
				// Hash verification fails!
				cout << "ref: ";
				for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
					cout << (int)(ref[j]) << " ";
				}
				cout << endl;
				cout << "hash: ";
				for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
					cout << (int)(hash[j]) << " ";
				}
				cout << endl;
				return false;
			}
		}
	}
	return true;
}
