#include "RemoteLeak.h"
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

inline double interval(std::chrono::_V2::system_clock::time_point start){
    auto end = std::chrono::high_resolution_clock::now();
    auto interval = (end - start)/1e+9;
    return interval.count();
}

RemoteLeak::RemoteLeak(NetIO* io, RingOramConfig oram_config, bool is_server, bool in_memory, bool integrity)
    : io(io), is_server(is_server), in_memory(in_memory), integrity(integrity), oram_config(oram_config){

    // currently only support in memory server
    assert(in_memory);

    ptx_block_size = oram_config.block_size;
	ctx_block_size = SBucket::getCipherSize();
    capacity = oram_config.num_buckets;
	bucket_size = oram_config.bucket_size;

	per_bucket_tree_height = ceil(log10(oram_config.bucket_size) / log10(2)) + 1;
	per_bucket_hashes = pow(2, per_bucket_tree_height) - 1;

    if(is_server){
        data = new unsigned char [capacity * bucket_size * ctx_block_size];

        cout << "> Remote storage server config: " << endl;
        cout << " -> in memory: " << in_memory << endl;
        cout << " -> integrity: " << integrity << endl;
        cout << " -> # buckets: " << capacity << endl;
        cout << " -> # Z: " << bucket_size << endl;
        cout << " -> block size: " << ctx_block_size << endl;
        cout << " -> total: " << capacity * bucket_size * ctx_block_size << endl;
    }
}

void RemoteLeak::run_server(){
    assert(is_server);
    if(in_memory){
        run_server_memory();
    } else{
        assert(0);
    }
    
}

void RemoteLeak::close_server(){
	int rt = End;
	io->send_data(&rt, sizeof(int));
}


void RemoteLeak::load_server_state(const char* fname){
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
	if(sb_size != ctx_block_size){
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

		ssize_t target_size = capacity * bucket_size * ctx_block_size;
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


void RemoteLeak::run_server_memory(){
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
			case ReadBatchBlock: {
				size_t num_blocks;
				io->recv_data(&num_blocks, sizeof(size_t));

				// auto t_fetch = std::chrono::high_resolution_clock::now();
				
				std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks);
				io->recv_data(offset.data(), sizeof(int)*num_blocks);

				size_t len = num_blocks * (ctx_block_size);
				// cout << "ReadBucketBatch allocate payload for size: " << len << endl;
				unsigned char* payload = new unsigned char[len];
				// cout << "ReadBucketBatch allocate done" << endl;

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_blocks; bucket_id++){
					size_t bucket_pos = position[bucket_id]*bucket_size + offset[bucket_id]; 
					size_t bucket_offset = bucket_id * ctx_block_size;
					// this->buckets[bucket_pos]->data_to_ptr(payload + bucket_offset);
					unsigned char* tmp_data = data + bucket_pos*ctx_block_size;
					mempcpy(payload + bucket_offset, tmp_data, ctx_block_size);
				} 

				// cout << "ReadBucketBatch write to payload done" << endl;

				io->send_data(payload, sizeof(unsigned char)*len);

				delete[] payload;

				break;
			}
			case WriteBatch_R:
			case WriteBatch:
			case ReadBatch:
			case ReadBatchBlock_R:
			case ReadBatchBlockXor:
			case WriteBatchBlock:{
				assert(0);
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


void RemoteLeak::ReadBlockBatch(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks){

	
	int rt = ReadBatchBlock;
	io->send_data(&rt, sizeof(int));

	size_t num_blocks = positions.size();
	io->send_data(&num_blocks, sizeof(size_t));
	io->send_data(positions.data(), sizeof(int)*num_blocks);
	io->send_data(offsets.data(), sizeof(int)*num_blocks);
	// cout << "ReadBucketBatch num_blocks: " << num_blocks << endl;

	size_t len = num_blocks * (SBucket::getCipherSize());
	// cout << "ReadBucketBatch allocate payload for size: " << len << endl;
	unsigned char* payload = new unsigned char[len];
	// cout << "ReadBucketBatch allocate done" << endl;

	io->recv_data(payload, len);

	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);
	blocks.resize(num_blocks);

	// cout << "ReadBlockBatchAsBlockRing: recv_data" << endl;
	
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t block_id = 0; block_id < num_blocks; block_id++){
		// printf("numThreads = %d\n", omp_get_num_threads()); 
		unsigned char* ctx_payload = payload + block_id * SBucket::getCipherSize();
		// // for (int i = 0; i < 10; i++) { // Adjust the length as needed
		// // 	// Convert each byte to an unsigned value and print as hex
		// // 	std::cout << std::hex << std::setw(2) << std::setfill('0') 
		// // 			<< static_cast<int>(static_cast<unsigned char>(ctx_payload[i])) << " ";
		// // }
		// std::cout << std::endl;
		unsigned char* ptx_payload = new unsigned char[SBucket::getCipherSize()];
		size_t ptx_len = decrypt_wrapper(ctx_payload, SBucket::getCipherSize(), ptx_payload);
		// assert(ptx_len == per_block_size*Bucket::getMaxSize());
		// unsigned char* tmp = ptx_payload;

		Block* b = new Block(ptx_block_size);
		b->from_ptr(ptx_payload);
		blocks[block_id] = b;
		
		// bkts.push_back(std::move(init_bkt));
		delete[] ptx_payload;
	}

	delete[] payload;
}