#include "RemoteRing.h"
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

#include <sys/wait.h> // For running dd command in load_server_state
#include "../diskann/include/timer.h"
#include <time.h>
#include <thread>

#include <liburing.h>
#include <vector>
#include <memory>
#define ALIGNMENT 512
// #define IO_MAX_EVENTS 1024	//matches DISKANN MAX_EVENTS 1024

#include "evp.h"

#include "diskann/include/timer.h"

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

RemoteRing::RemoteRing(NetIO* io, RingOramConfig oram_config, bool is_server, bool in_memory, bool integrity, int disk_type, string disk_path)
    : io(io), is_server(is_server), in_memory(in_memory), integrity(integrity), oram_config(oram_config), disk_type(disk_type){

    ptx_block_size = oram_config.block_size;
	ctx_block_size = SBucket::getCipherSize();
    capacity = oram_config.num_buckets;
	bucket_size = oram_config.bucket_size;

	cout << "ptx bs: " << oram_config.block_size << " ctx bs: " << ctx_block_size << " bs:" << oram_config.bucket_size << endl;
	cout << "inmem:" << in_memory << endl;

	per_bucket_tree_height = ceil(log10(oram_config.bucket_size) / log10(2)) + 1;
	per_bucket_hashes = pow(2, per_bucket_tree_height) - 1;

    if(is_server){
		if(in_memory){
			data = new unsigned char [capacity * bucket_size * ctx_block_size];

			if(integrity){				
				size_t per_bucket_hash_size = SHA256_DIGEST_LENGTH * per_bucket_hashes * capacity;
							
				per_bucket_hash = new uint8_t[per_bucket_hash_size];
				tree_hash = new uint8_t[capacity * SHA256_DIGEST_LENGTH];
			}
		}
		else{
			
			if(disk_path == ""){
				perror("buckets destination path not mentioned");
				exit(1);
			}

			//make copy file of buckets.bin, DONT FILL YET
			string base_temp = disk_path;
			string temp_name = base_temp + std::to_string(getpid()) + ".bin";
			cout << "disk copy at: " << temp_name << endl;
			
			disk_bucket_path = temp_name;
			
			
			//DISK TYPE cases
			if(disk_type == 1){
	
				disk_fd = open(disk_bucket_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
				if (disk_fd < 0) {
					perror("Error creating temporary file ORAM bucekts copy");
					exit(EXIT_FAILURE);
				}
				
				//unsigned char* disk_ptr = nullptr;

			}
			else if(disk_type == 2){

				// O_DIRECT used
				disk_fd = open(disk_bucket_path.c_str(), O_DIRECT | O_RDWR | O_CREAT | O_TRUNC, 0600);
				if (disk_fd < 0) {
					perror("Error creating temporary file ORAM bucekts copy");
					exit(EXIT_FAILURE);
				}
				


				cout << "disk_fd value: " << disk_fd << endl;
			}
			else{
				assert(disk_type == -1);
				cout << "UNUSUAL CASE encountered" << endl;
				abort();
			}



			if(integrity){				
				size_t per_bucket_hash_size = SHA256_DIGEST_LENGTH * per_bucket_hashes * capacity;
							
				per_bucket_hash = new uint8_t[per_bucket_hash_size];
				tree_hash = new uint8_t[capacity * SHA256_DIGEST_LENGTH];
			}
		}


        cout << "> Remote storage server config: " << endl;
        cout << " -> in memory: " << in_memory << endl;
		cout << " -> Disk Type: " << disk_type << endl;
		cout << " -> Disk Bucket Path: " << disk_bucket_path << endl;
        cout << " -> integrity: " << integrity << endl;
        cout << " -> # buckets: " << capacity << endl;
        cout << " -> # Z: " << bucket_size << endl;
        cout << " -> block size: " << ctx_block_size << endl;
        cout << " -> total: " << capacity * bucket_size * ctx_block_size << endl;
    }
}

void RemoteRing::clear_data(){
	if(is_server){
		if(in_memory){
			// data = new unsigned char [capacity * bucket_size * ctx_block_size];
			if(data != nullptr){
				delete[] data;      // free the allocated memory
                data = nullptr;     // avoid dangling pointer
			}
		} else {

			unlink(disk_bucket_path.c_str());
			close(disk_fd);

		}
	}
}

void RemoteRing::run_server(int io_max){
    assert(is_server);
    if(in_memory){
		cout << "in mem server running..." << endl;
        run_server_memory();
    } else{
		if(disk_type == 1){
			cout << "on disk - mmap version - server running ..." << endl;
			run_server_disk();

		}
		else if(disk_type == 2){
			cout << "on disk - liburing version - server running ... " << endl;
			run_server_disk_iouring(io_max);
		}
		else{
			assert(disk_type == -1);
		}
    }
    
}

void RemoteRing::close_server(){
	int rt = End;
	io->send_data(&rt, sizeof(int));
}

/*
void RemoteRing::load_server_state(const char* fname){
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
	cout << c << " " << capacity << "\n";
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

	// size_t src_mmap_size = capacity * bucket_size * block_size + sizeof(int)*2 + sizeof(bool);

	// // mmap optimization
	// // Memory-map the file with read-write permissions
    // unsigned char* mmap_data = static_cast<unsigned char*>(mmap(nullptr, src_mmap_size, PROT_READ, MAP_PRIVATE, fd, 0));
    // if (mmap_data == MAP_FAILED) {
    //     perror("Error mapping file");
    //     close(fd);
    //     assert(0);
    // }

	// if (madvise(mmap_data, src_mmap_size, POSIX_MADV_SEQUENTIAL) == -1) {
    //     perror("Error advising kernel");
    //     munmap(mmap_data, src_mmap_size);
    //     close(fd);
    //     assert(0);
    // }
	
	// unsigned char* mmap_payload = mmap_data + sizeof(int)*2 + sizeof(bool);

	// #pragma omp parallel num_threads(NUM_THREADS)
	// for(size_t i = 0; i < capacity; i++){
	// 	memcpy(data + i * bucket_size * block_size, mmap_payload + i * bucket_size * block_size, bucket_size * block_size);
	// }

	// munmap(mmap_data, src_mmap_size);


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

		// assert(total_read == target_size);

        // for (size_t i = 0; i < capacity*bucket_size; i++){
        //     read(fd, this->buckets[i]->data, ctx_block_size);
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
*/
void RemoteRing::load_from_ground(RemoteRing ground_server){

	assert(in_memory);

	// Copying Data buckets from ground
	unsigned char* ground_data = ground_server.data;
	#pragma omp parallel for num_threads(96)
	for (size_t i = 0; i < capacity * bucket_size; ++i) {
		memcpy(data + i * ctx_block_size,
			ground_data + i * ctx_block_size,
			ctx_block_size);
	}


	// copying hash informations from ground
	if(integrity){
		assert(integrity == ground_server.integrity);

		// copy per_bucket_hash
		per_bucket_tree_height = ceil(log10(bucket_size) / log10(2)) + 1;
		per_bucket_hashes = pow(2, per_bucket_tree_height) - 1;
		size_t per_bucket_hash_size = SHA256_DIGEST_LENGTH * per_bucket_hashes * capacity;

		uint8_t* ground_pbh = ground_server.per_bucket_hash;
		// per_bucket_hash = new uint8_t[per_bucket_hash_size];

		#pragma omp parallel for num_threads(96)
		for (size_t i = 0; i < per_bucket_hashes * capacity; ++i) {
			memcpy(per_bucket_hash + i * SHA256_DIGEST_LENGTH,
				ground_pbh + i * SHA256_DIGEST_LENGTH,
				SHA256_DIGEST_LENGTH);
		}

		// copy tree_hash

		uint8_t* ground_th = ground_server.tree_hash;
		// tree_hash = new uint8_t[capacity * SHA256_DIGEST_LENGTH];

		#pragma omp parallel for num_threads(96)
		for (size_t i = 0; i < capacity; ++i) {
			memcpy(tree_hash + i * SHA256_DIGEST_LENGTH,
				ground_th + i * SHA256_DIGEST_LENGTH,
				SHA256_DIGEST_LENGTH);
		}

	}
}

void RemoteRing::load_server_state(const char* fname) {

	// SOURCE FILE
    int src_fd = open(fname, O_RDONLY);
    if (src_fd < 0) {
        perror("Error opening file");
        exit(EXIT_FAILURE);
    }

    // Read header: capacity, ctx_block_size, integrity flag
    int file_capacity, sb_size;
    bool file_integrity;

    if (read(src_fd, &file_capacity, sizeof(int)) != sizeof(int) ||
        read(src_fd, &sb_size, sizeof(int)) != sizeof(int) ||
        read(src_fd, &file_integrity, sizeof(bool)) != sizeof(bool)) {
			
        perror("Error reading file header");
        close(src_fd);
        exit(EXIT_FAILURE);
    }
    if (file_capacity != capacity || sb_size != ctx_block_size) {
        std::cerr << "Buckets.bin is Incompatible file format.\n";
        close(src_fd);
        exit(EXIT_FAILURE);
    }

	
	//IN-MEMORY ---- OR ---- ON-DISK
	
	if (in_memory) {
		
		// Calculate mmap size: skip 2 ints + 1 bool which were read as headers
		size_t offset = sizeof(int) * 2 + sizeof(bool);
		size_t src_mmap_size = offset + capacity * bucket_size * ctx_block_size;
		size_t payload_size = capacity * bucket_size * ctx_block_size;

		// mmap source file to use in copying data
		void* src_mmap = mmap(nullptr, src_mmap_size, PROT_READ, MAP_PRIVATE, src_fd, 0);
		if (src_mmap == MAP_FAILED) {
			perror("mmap failed");
			close(src_fd);
			exit(EXIT_FAILURE);
		}

		// Tell the kernel we’ll access sequentially (performance hint)
		if (madvise(src_mmap, src_mmap_size, MADV_SEQUENTIAL) != 0) {
			perror("madvise failed");
			munmap(src_mmap, src_mmap_size);
			close(src_fd);
			exit(EXIT_FAILURE);
		}	
    
        // Copy data into in-memory buffer AFTER SKIPPING OFFSET
        unsigned char* src_payload = reinterpret_cast<unsigned char*>(src_mmap) + offset;

        #pragma omp parallel for num_threads(96)
        for (size_t i = 0; i < capacity * bucket_size; ++i) {
            memcpy(data + i * ctx_block_size,
                   src_payload + i * ctx_block_size,
                   ctx_block_size);
        }

        std::cout << "Successfully loaded " << capacity * bucket_size << " blocks. " << "Server side memory: " << (double)capacity*bucket_size*ctx_block_size/(1024*1024*1024) << "GB.\n";


		munmap(src_mmap, src_mmap_size);
		// close(src_fd);

    } else {
		
		//On-Disk ORAM
		//preallocate disk file

		size_t offset = sizeof(int) * 2 + sizeof(bool);
		size_t src_mmap_size = offset + capacity * bucket_size * ctx_block_size;
		size_t payload_size = capacity * bucket_size * ctx_block_size;

		int file_padding = ALIGNMENT * 10;

        if (ftruncate(disk_fd, payload_size + file_padding) < 0) {
			perror("ftruncate failed");
            close(disk_fd);
            exit(EXIT_FAILURE);
        }

		//writing zero pad at the end of the file. disk_fd seek position is not changed
		// off_t pad_offset = payload_size;
		// vector<unsigned char> zeros(file_padding, 0);
		// if (pwrite(disk_fd, zeros.data(), file_padding, pad_offset) != file_padding) {
		// 	perror("pwrite for padding failed");
		// 	close(disk_fd);
		// 	exit(EXIT_FAILURE);
		// }


		//copying the file

		string if_arg = "if=" + string(fname);
		string src_path = string(fname);

		string of_arg = "of=" + disk_bucket_path;
		string dst_path = disk_bucket_path;

		string bs_arg = "bs=16M";

		string skip_arg = "skip=" + to_string(offset);

		string iflag = "iflag=skip_bytes";

		cout << "dd command inputs" << endl;
		cout << if_arg << endl << of_arg << endl << bs_arg << endl << skip_arg << endl << iflag << endl;


		pid_t pid = fork();
		if(pid == 0){

			execlp(
				"dd",
				"dd",
				if_arg.c_str(),
				of_arg.c_str(),
				bs_arg.c_str(),
				skip_arg.c_str(),
				iflag.c_str(),
				"status=progress",
				(char*)nullptr
			);

			perror("execld dd failed");
			exit(1);
		}

		waitpid(pid, nullptr, 0);

/*
dd
if=src_path
of=dst_path
bs=16M
skip=SRC_OFFSET
iflag=skip_bytes
staus=progress

*/







	

		/*
		
		void* dest_mmap = mmap(nullptr, payload_size, PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd, 0);
		if (dest_mmap == MAP_FAILED) {
			perror("mmap failed");
			close(disk_fd);
			exit(EXIT_FAILURE);
		}
		if (madvise(dest_mmap, payload_size, MADV_RANDOM) != 0) {
			perror("madvise failed");
			munmap(dest_mmap, payload_size);
			close(disk_fd);
			exit(EXIT_FAILURE);
		}	
		
		unsigned char* src_payload = reinterpret_cast<unsigned char*>(src_mmap) + offset;
		unsigned char* dest_payload = reinterpret_cast<unsigned char*>(dest_mmap);
		
		#pragma omp parallel for num_threads(96)
        for (size_t i = 0; i < capacity * bucket_size; ++i) {
            memcpy(dest_payload + i * ctx_block_size,
			src_payload + i * ctx_block_size,
			ctx_block_size);
        }
		
		std::cout << "Successfully loaded " << capacity * bucket_size << " blocks.\n" << "Copy on disk: " << (double)capacity*bucket_size*ctx_block_size/(1024*1024*1024) << "GB.\n";
		
		//un map the source
		munmap(src_mmap, src_mmap_size);
		// DO NOT UN MAP the dest
		
		*/
		if(disk_type == 1){
			// disk_ptr = reinterpret_cast<unsigned char*>(dest_mmap);

			void* dest_mmap = mmap(nullptr, payload_size, PROT_READ | PROT_WRITE, MAP_SHARED, disk_fd, 0);
			if (dest_mmap == MAP_FAILED) {
				perror("mmap failed");
				close(disk_fd);
				exit(EXIT_FAILURE);
			}
			if (madvise(dest_mmap, payload_size, MADV_RANDOM) != 0) {
				perror("madvise failed");
				munmap(dest_mmap, payload_size);
				close(disk_fd);
				exit(EXIT_FAILURE);
			}
			
		}
		else if(disk_type == 2){
			// munmap(dest_mmap, payload_size);

			cout << "disk_fd value: " << disk_fd << endl;

		}
		else{
			cout << "unusual case" << endl;
			assert(0);
		}


    }

    close(src_fd);
}



void RemoteRing::load_server_hash(const char* fname){
	cout << "loading server hash ..." << endl;
	per_bucket_tree_height = ceil(log10(bucket_size) / log10(2)) + 1;
	per_bucket_hashes = pow(2, per_bucket_tree_height) - 1;
	size_t per_bucket_hash_size = SHA256_DIGEST_LENGTH * per_bucket_hashes * capacity;

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

	// tree_hash = new uint8_t[capacity * SHA256_DIGEST_LENGTH];
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

	cout << "total_read: " << total_read << endl;

	assert(total_read == target_size);

	return;
}

void RemoteRing::run_server_memory(){
	while(1) {
		int rt;
		io->recv_data(&rt, sizeof(int));
		// cout << "RT = " << rt << endl;

		switch (rt){
			case -1: {
				// cout << "case -1" << endl;
				long comm = io->counter - start_comm;
				long rounds = io->num_rounds - start_rounds;
				double online_server_local_time = online_server_side_time.count();

				// cout << "Received req to send comm: " << comm << " bytes" << endl;
				io->send_data(&comm, sizeof(long));
				io->send_data(&rounds, sizeof(long));
				io->send_data(&online_server_local_time, sizeof(double));

				io->counter -= 2*sizeof(long);
				io->counter -= sizeof(double);
				io->num_rounds--;
				break;
			}

			case ReadBatchBlock_R:
			case ReadBatchBlock: {
				// cout << "ReadBatchBlock" << endl;
				
				size_t num_blocks;
				io->recv_data(&num_blocks, sizeof(size_t));

				std::vector<int> position(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks);

				std::vector<int> offset(num_blocks);
				io->recv_data(offset.data(), sizeof(int)*num_blocks);


				auto lts = std::chrono::steady_clock::now();

				size_t len = num_blocks * (ctx_block_size);
				unsigned char* payload = new unsigned char[len];

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_blocks; bucket_id++){
					size_t bucket_pos = position[bucket_id]*bucket_size + offset[bucket_id]; 
					size_t bucket_offset = bucket_id * ctx_block_size;

					unsigned char* tmp_data = data + bucket_pos*ctx_block_size;
					mempcpy(payload + bucket_offset, tmp_data, ctx_block_size);
				} 

				if(rt == ReadBatchBlock_R){
					// this case wont be called other than Reshuffling thats why
					online_server_side_time += (std::chrono::steady_clock::now() - lts);
				}


				long comm = io->counter;
				io->send_data(payload, sizeof(unsigned char)*len);
				comm = io->counter - comm;

				io->send_data(&comm, sizeof(long));
				io->counter -= sizeof(long);

				if(integrity){
					if(rt == ReadBatchBlock){
						// for bucket read
						send_hash_bucket(position, offset);
					} else{
						// for reshuffle
						send_hash_reshuffle(position, offset);
					}
				}

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

				// cout << "pos: ";
				// for(int i=0; i<num_blocks; i++){
				// 	cout << position[i] << " " ;
				// }
				// cout << endl;

				auto lts = std::chrono::steady_clock::now();

				size_t path_len = num_blocks / num_real_blocks;
				size_t len = num_real_blocks * (ctx_block_size - 16);	// no need for ivs

				unsigned char* payload = new unsigned char[len];
				unsigned char* ivs = new unsigned char[num_blocks*16];
				std::memset(payload, 0, len); 

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t block_id = 0; block_id < num_real_blocks; block_id++){
					size_t bucket_offset = block_id * (ctx_block_size - 16);
					for(int i = 0; i < path_len; i++){
						size_t bucket_id = block_id * path_len + i;
						size_t bucket_pos = position[bucket_id]*bucket_size + offset[bucket_id];

						unsigned char* ptr = payload + bucket_offset;
						unsigned char* tmp_data = data + bucket_pos*ctx_block_size;
						
						// first 16 goes to iv
						memcpy(ivs + bucket_id*16, tmp_data, 16);

						// 16 - block_size goes to xor
						for(size_t j = 16; j < ctx_block_size; j++){
							ptr[j - 16] = tmp_data[j] ^ ptr[j - 16]; 
						}
					}
					
				}

				online_server_side_time += (std::chrono::steady_clock::now() - lts);

				long comm = io->counter;

				io->send_data(payload, sizeof(unsigned char)*len);
				io->send_data(ivs, sizeof(unsigned char)*num_blocks*16);
				comm = io->counter - comm;

				io->send_data(&comm, sizeof(long));
				io->counter -= sizeof(long);

				if(integrity){
					send_hash(position, offset);
				}

				delete[] payload;

				break;
			}
			case WriteBatch_R:
			case WriteBatch:{
				// cout << "writebatch" << endl;

				size_t num_buckets;
				io->recv_data(&num_buckets, sizeof(size_t));
				
				std::vector<int> position(num_buckets);
				io->recv_data(position.data(), sizeof(int)*num_buckets);

				size_t len = num_buckets * bucket_size * (ctx_block_size);
				
				unsigned char* payload = new unsigned char[len];
				io->recv_data(payload, sizeof(unsigned char)*len);

				auto lts = std::chrono::steady_clock::now();
				
				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
					for(size_t block_id = 0; block_id < bucket_size; block_id++){
						size_t block_pos = position[bucket_id]*bucket_size + block_id;
						size_t block_offset = (bucket_id*bucket_size + block_id) * ctx_block_size;

						unsigned char* tmp_data = data + block_pos*ctx_block_size;
						mempcpy(tmp_data, payload + block_offset, ctx_block_size);
					}
				} 

				if(integrity){
					if(rt == WriteBatch){
						update_hash(position, payload);
					} else{
						update_hash_reshuffle(position, payload);
					}
				}

				if(rt == WriteBatch_R){
					// this case wont be called other than Reshuffling thats why
					online_server_side_time += (std::chrono::steady_clock::now() - lts);
				}
				
				delete[] payload;
				break;
			}
			
			case End:{
				// cout << "end" << endl;

				cout << "Clearing RAM...";
				delete[] data;      // free the allocated memory
                data = nullptr;     // avoid dangling pointer
				cout << "... done" << endl;

				cout << "Online time spent at server: " << online_server_side_time.count()*1000 << " ms\n";

				cout << "Closing server...\n";
				return;
			}
			default:{
				assert(0);
			}
		
		}
	}
}


void RemoteRing::run_server_disk(){
	while(1) {
		int rt;
		io->recv_data(&rt, sizeof(int));
		// cout << "RT = " << rt << endl;

		switch (rt){
			case -1: {
				long comm = io->counter - start_comm;
				long rounds = io->num_rounds - start_rounds;
				double online_server_local_time = online_server_side_time.count();

				// cout << "Received req to send comm: " << comm << " bytes" << endl;
				io->send_data(&comm, sizeof(long));
				io->send_data(&rounds, sizeof(long));
				io->send_data(&online_server_local_time, sizeof(double));

				io->counter -= 2*sizeof(long);
				io->counter -= sizeof(double);
				io->num_rounds--;
				break;
			}

			case ReadBatchBlock_R:
			case ReadBatchBlock: {
				
				size_t num_blocks;
				io->recv_data(&num_blocks, sizeof(size_t));

				std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks); //bucket number
				io->recv_data(offset.data(), sizeof(int)*num_blocks); //block offset within the bucket

				auto lts = std::chrono::steady_clock::now();

				size_t len = num_blocks * (ctx_block_size);
				unsigned char* payload = new unsigned char[len];

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_blocks; bucket_id++){

					size_t block_pos = position[bucket_id]*bucket_size + offset[bucket_id]; 
					size_t block_pos_bytes = bucket_id * ctx_block_size;

					// cout << "before read" << endl;
					unsigned char* tmp_data = disk_ptr + block_pos*ctx_block_size;
					mempcpy(payload + block_pos_bytes, tmp_data, ctx_block_size);
					// cout << "after read" << endl;
				} 

				if(rt == ReadBatchBlock_R){
					online_server_side_time += (std::chrono::steady_clock::now() - lts);
				}

				long comm = io->counter;
				io->send_data(payload, sizeof(unsigned char)*len);
				comm = io->counter - comm;

				io->send_data(&comm, sizeof(long));
				io->counter -= sizeof(long);

				if(integrity){
					if(rt == ReadBatchBlock){
						// for bucket read
						send_hash_bucket(position, offset);
					} else{
						// for reshuffle
						send_hash_reshuffle(position, offset);
					}
				}

				delete[] payload;

				break;
			}
			
			case ReadBatchBlockXor:{
				// cout << "ReadBatchBlockXor" << endl;
				size_t num_blocks;
				size_t num_real_blocks;
				io->recv_data(&num_blocks, sizeof(size_t));
				io->recv_data(&num_real_blocks, sizeof(size_t));
				// cout << "Num real blocks: " << num_real_blocks << "\n";

				std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks);
				io->recv_data(offset.data(), sizeof(int)*num_blocks);

				auto lts = std::chrono::steady_clock::now();

				size_t path_len = num_blocks / num_real_blocks;

				// no need for ivs
				size_t len = num_real_blocks * (ctx_block_size - 16);

				unsigned char* payload = new unsigned char[len];
				unsigned char* ivs = new unsigned char[num_blocks*16];
				std::memset(payload, 0, len); 

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t block_id = 0; block_id < num_real_blocks; block_id++){

					size_t block_pos_bytes = block_id * (ctx_block_size - 16);

					for(int i = 0; i < path_len; i++){

						size_t bucket_id = block_id * path_len + i;
						size_t block_pos = position[bucket_id]*bucket_size + offset[bucket_id];

						unsigned char* ptr = payload + block_pos_bytes;
						unsigned char* tmp_data = disk_ptr + block_pos*ctx_block_size;
						
						// cout << "before xor read" << endl;
						// first 16 goes to iv
						memcpy(ivs + bucket_id*16, tmp_data, 16);

						// 16 - block_size goes to xor
						for(size_t j = 16; j < ctx_block_size; j++){
							ptr[j - 16] = tmp_data[j] ^ ptr[j - 16]; 
						}

						// cout << "after xor read" << endl;
					}
					
				}

				online_server_side_time += (std::chrono::steady_clock::now() - lts);

				long comm = io->counter;

				io->send_data(payload, sizeof(unsigned char)*len);
				io->send_data(ivs, sizeof(unsigned char)*num_blocks*16);
				comm = io->counter - comm;

				io->send_data(&comm, sizeof(long));
				io->counter -= sizeof(long);

				if(integrity){
					send_hash(position, offset);
				}

				// {
				// 	std::ofstream payload_out("payload_dump.bin", std::ios::binary);
				// 	if (payload_out.is_open()) {
				// 		payload_out.write(reinterpret_cast<char*>(payload), len);
				// 		payload_out.close();
				// 	} else {
				// 		std::cerr << "Failed to open payload_dump.bin for writing.\n";
				// 	}
				// }

				// // Output IVs to a binary file
				// {
				// 	std::ofstream ivs_out("ivs_dump.bin", std::ios::binary);
				// 	if (ivs_out.is_open()) {
				// 		ivs_out.write(reinterpret_cast<char*>(ivs), num_blocks * 16);
				// 		ivs_out.close();
				// 	} else {
				// 		std::cerr << "Failed to open ivs_dump.bin for writing.\n";
				// 	}
				// }

				delete[] payload;

				break;
			}
			case WriteBatch_R:
			case WriteBatch:{

				size_t num_buckets;

				io->recv_data(&num_buckets, sizeof(size_t));
				
				std::vector<int> position(num_buckets);
				io->recv_data(position.data(), sizeof(int)*num_buckets);
				
				size_t len = num_buckets * bucket_size * (ctx_block_size);
				
				unsigned char* payload = new unsigned char[len];
				io->recv_data(payload, sizeof(unsigned char)*len);

				auto lts = std::chrono::steady_clock::now();
				
				
				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
					for(size_t block_id = 0; block_id < bucket_size; block_id++){
						size_t block_pos = position[bucket_id]*bucket_size + block_id;
						size_t block_offset = (bucket_id*bucket_size + block_id) * ctx_block_size;

						// cout << "before write" << endl;
						unsigned char* tmp_data = disk_ptr + block_pos*ctx_block_size;
						mempcpy(tmp_data, payload + block_offset, ctx_block_size);
						// cout << "after write" << endl;
					}
				} 

				if(integrity){
					if(rt == WriteBatch){
						update_hash(position, payload);
					} else{
						update_hash_reshuffle(position, payload);
					}
				}

				if(rt == WriteBatch_R){
					// this case wont be called other than Reshuffling thats why
					online_server_side_time += (std::chrono::steady_clock::now() - lts);
				}

				
				delete[] payload;
				break;
			}
			
			case End:{
				cout << "syncing mmap with file..." ;
				munmap(disk_ptr, capacity * bucket_size * ctx_block_size);
				cout << "... done" << endl;

				cout << "Online time spent at server: " << online_server_side_time.count()*1000 << " ms\n";

				cout << "Closing server...\n";
				return;
			}
			default:{
				assert(0);
			}
		
		}
	}

}


void RemoteRing::run_server_disk_iouring(int io_max) {
    assert(is_server);

    struct io_uring ring;
    if (io_uring_queue_init(io_max, &ring, 0) < 0) {
        perror("io_uring_queue_init failed");
        exit(EXIT_FAILURE);
    }

    while (1) {

        int rt;
        io->recv_data(&rt, sizeof(int));
		// cout << "RT: " << rt << endl;
        
		switch (rt) {
            case -1: {
                long comm = io->counter - start_comm;
                long rounds = io->num_rounds - start_rounds;
                double online_server_local_time = online_server_side_time.count();

                io->send_data(&comm, sizeof(long));
                io->send_data(&rounds, sizeof(long));
                io->send_data(&online_server_local_time, sizeof(double));

                io->counter -= 2 * sizeof(long);
                io->counter -= sizeof(double);
                io->num_rounds--;
                break;
            }

            case ReadBatchBlock_R:
            case ReadBatchBlock: {
				// cout << "RBB" << endl;

                size_t num_blocks;
                io->recv_data(&num_blocks, sizeof(size_t));

                std::vector<int> position(num_blocks);
                io->recv_data(position.data(), sizeof(int) * num_blocks);
				
                std::vector<int> offset(num_blocks);
                io->recv_data(offset.data(), sizeof(int) * num_blocks);

				auto lts = std::chrono::steady_clock::now();


				/// calculate max cached bucket that is in server cache mmap
				// int max_cached_bucket;




				//======================================
				// Temporary aligned buffers
				//========================================
				std::vector<off_t> aligned_left_offset(num_blocks);
				std::vector<off_t> aligned_right_offset(num_blocks);
				std::vector<size_t> aligned_num_bytes(num_blocks);
				std::vector<size_t> aligned_cumulative_bytes(num_blocks + 1, 0);


				for(size_t i=0; i < num_blocks; i++){
					off_t offset_bytes = (position[i] * bucket_size + offset[i]) * ctx_block_size;

					off_t left = offset_bytes - (offset_bytes % ALIGNMENT);
					off_t right = ((offset_bytes + ctx_block_size + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT;

					aligned_left_offset[i] = left;
					aligned_right_offset[i] = right;
					aligned_num_bytes[i] = right - left;
					aligned_cumulative_bytes[i + 1] = aligned_cumulative_bytes[i] + aligned_num_bytes[i];
				}

				size_t aligned_total_bytes = aligned_cumulative_bytes[num_blocks];
               

				void* tmp_buf;
				if (posix_memalign(&tmp_buf, ALIGNMENT, aligned_total_bytes) != 0) {
					std::cerr << "posix_memalign failed\n";
					exit(EXIT_FAILURE);
				}
				unsigned char* tmp_payload = static_cast<unsigned char*>(tmp_buf);
                

				void* out_buf;
				if (posix_memalign(&out_buf, ALIGNMENT, num_blocks * ctx_block_size) != 0) {
					std::cerr << "posix_memalign failed\n";
					exit(EXIT_FAILURE);
				}
				unsigned char* payload = static_cast<unsigned char*>(out_buf);


				//==================================
				// ISSUE aligned reads
				//==================================
				size_t submitted = 0;
				size_t in_flight = 0;

				// cout << "num IOs: " << num_blocks << endl;
				while (submitted < num_blocks) {

					io_uring_sqe* sqe = io_uring_get_sqe(&ring);
					if (!sqe) break;

					off_t read_offset = aligned_left_offset[submitted];
					unsigned char* dst = tmp_payload + aligned_cumulative_bytes[submitted];
					size_t read_len = aligned_num_bytes[submitted];

					io_uring_prep_read(sqe, disk_fd, dst, read_len, read_offset);
					io_uring_sqe_set_data(sqe, (void*)(uintptr_t)submitted);
					
					in_flight++;
					submitted++;
					
					if (in_flight >= io_max || submitted == num_blocks) {
						
						//batched submission
						io_uring_submit(&ring);

						//wait for completion
						for (size_t j = 0; j < in_flight; j++) {
							struct io_uring_cqe* cqe;
							int ret = io_uring_wait_cqe(&ring, &cqe);
							if (ret < 0) {
								std::cerr << "io_uring_wait_cqe failed: " << strerror(-ret) << std::endl;
								exit(EXIT_FAILURE);
							}
							if (cqe->res < 0) {
								std::cerr << "IO operation failed: " << strerror(-cqe->res) << std::endl;
								exit(EXIT_FAILURE);
							}
							io_uring_cqe_seen(&ring, cqe);
						}
						in_flight = 0;
					}
				}

				#pragma omp parallel for num_threads(NUM_THREADS)
				for (size_t i = 0; i < num_blocks; i++) {
					off_t true_offset = (position[i] * bucket_size + offset[i]) * ctx_block_size;
					off_t diff = true_offset - aligned_left_offset[i];
					std::memcpy(payload + i * ctx_block_size, tmp_payload + aligned_cumulative_bytes[i] + diff, ctx_block_size);
				}


				

				if (rt == ReadBatchBlock_R) {
					online_server_side_time += (std::chrono::steady_clock::now() - lts);
    			}


                long comm = io->counter;
                io->send_data(payload, num_blocks * ctx_block_size);
                comm = io->counter - comm;

                io->send_data(&comm, sizeof(long));
                io->counter -= sizeof(long);

                if (integrity) {
                    if (rt == ReadBatchBlock) {
                        send_hash_bucket(position, offset);
                    } else {
                        send_hash_reshuffle(position, offset);
                    }
                }

				// cout << "RBB end" << endl;

				free(tmp_payload);
                free(payload);
                break;
            }

            case ReadBatchBlockXor: {

				// cout << "RBBX" << endl;

                size_t num_blocks;
				size_t num_real_blocks;
                io->recv_data(&num_blocks, sizeof(size_t));
                io->recv_data(&num_real_blocks, sizeof(size_t));

                std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
                io->recv_data(position.data(), sizeof(int) * num_blocks);
                io->recv_data(offset.data(), sizeof(int) * num_blocks);

				
				auto lts = std::chrono::steady_clock::now();

                size_t path_len = num_blocks / num_real_blocks;
                size_t len = num_real_blocks * (ctx_block_size - 16); //no need for ivs

				// // cacluate server cache
				// int max_cached_bucket;

				
				//========================
				// temporary aligned buffers
				//====================
				vector<off_t> aligned_left_offset(num_blocks);
				vector<off_t> aligned_right_offset(num_blocks);
				vector<size_t> aligned_num_bytes(num_blocks);
				vector<size_t> aligned_cumulative_bytes(num_blocks + 1, 0);

				for(size_t i=0; i < num_blocks; i++){
					off_t offset_bytes = (position[i] * bucket_size + offset[i]) * ctx_block_size;

					off_t left = offset_bytes - (offset_bytes % ALIGNMENT);
					off_t right = ((offset_bytes + ctx_block_size + ALIGNMENT - 1)/ALIGNMENT)*ALIGNMENT;
					
					aligned_left_offset[i] = left;
					aligned_right_offset[i] = right;
					aligned_num_bytes[i] = right-left;
					aligned_cumulative_bytes[i + 1] = aligned_cumulative_bytes[i] + aligned_num_bytes[i];
				}
				
				size_t aligned_total_bytes = aligned_cumulative_bytes[num_blocks];
                // size_t len = num_blocks * ctx_block_size;
				

				void* buf_payload;
				void* buf_ivs; 
				void* buf_tmp;
				if (posix_memalign(&buf_payload, ALIGNMENT, len) != 0 ||
					posix_memalign(&buf_ivs, ALIGNMENT, num_blocks * 16) != 0 ||
					posix_memalign(&buf_tmp, ALIGNMENT, aligned_total_bytes) != 0) {
					std::cerr << "posix_memalign failed\n";
					exit(EXIT_FAILURE);
				}

				unsigned char* payload = static_cast<unsigned char*>(buf_payload);
				unsigned char* ivs = static_cast<unsigned char*>(buf_ivs);
				unsigned char* tmp_data = static_cast<unsigned char*>(buf_tmp);

				std::memset(payload, 0, len);
				std::memset(ivs, 0, num_blocks * 16);
				std::memset(tmp_data, 0, aligned_total_bytes);


				//==========================
				// ISSUE ALIGNED READS
				//=========================
				size_t submitted = 0;
				size_t in_flight = 0;
				
				// cout << "num IOs: " << num_blocks << endl;
				while (submitted < num_blocks) {

					io_uring_sqe* sqe = io_uring_get_sqe(&ring);
					if (!sqe) break;

					size_t idx = submitted;
					off_t read_offset = aligned_left_offset[idx];
					unsigned char* dst = tmp_data + aligned_cumulative_bytes[idx];
					size_t read_len = aligned_num_bytes[idx];

					io_uring_prep_read(sqe, disk_fd, dst, read_len, read_offset);
					io_uring_sqe_set_data(sqe, (void*)(uintptr_t)idx);
					
					in_flight++;
					submitted++;
					
					if (in_flight >= io_max || submitted == num_blocks) {
						
						io_uring_submit(&ring);

						for (size_t j = 0; j < in_flight; j++) {
							struct io_uring_cqe* cqe;
							int ret = io_uring_wait_cqe(&ring, &cqe);
							if (ret < 0) {
								std::cerr << "io_uring_wait_cqe failed: " << strerror(-ret) << std::endl;
								exit(EXIT_FAILURE);
							}
							if (cqe->res < 0) {
								std::cerr << "IO operation failed: " << strerror(-cqe->res) << std::endl;
								exit(EXIT_FAILURE);
							}
							io_uring_cqe_seen(&ring, cqe);
						}
						in_flight = 0;
					}
				}

				#pragma omp parallel for num_threads(NUM_THREADS)
				for (size_t real_block_idx = 0; real_block_idx < num_real_blocks; real_block_idx++){

					size_t payload_offset = real_block_idx * (ctx_block_size - 16);

					for (int i=0; i< path_len; i++){
						size_t block_idx = real_block_idx * path_len + i;

						off_t true_offset = (position[block_idx] * bucket_size + offset[block_idx]) * ctx_block_size;

						off_t diff = true_offset - aligned_left_offset[block_idx];

						unsigned char* ptr = payload + payload_offset;
						unsigned char* cur_data = tmp_data + aligned_cumulative_bytes[block_idx] + diff;

						memcpy(ivs + block_idx * 16, cur_data, 16);

						for( size_t j=16; j < ctx_block_size; j++){
							ptr[j-16] = cur_data[j] ^ ptr[j-16];
						}
					}

				}

				
                online_server_side_time += (std::chrono::steady_clock::now() - lts);

                long comm = io->counter;

                io->send_data(payload, len);
                io->send_data(ivs, sizeof(unsigned char) * num_blocks * 16);

                comm = io->counter - comm;
                io->send_data(&comm, sizeof(long));
                io->counter -= sizeof(long);

                if (integrity) {
                    send_hash(position, offset);
                }

                free(payload);
                free(ivs);
                free(tmp_data);

                break;
            }

            case WriteBatch_R:
            case WriteBatch: {

				// cout << "WB" << endl;

                size_t num_buckets;
                io->recv_data(&num_buckets, sizeof(size_t));

                std::vector<int> position(num_buckets);
                io->recv_data(position.data(), sizeof(int) * num_buckets);

                size_t len = num_buckets * bucket_size * ctx_block_size;

                void* payload_void;
                if (posix_memalign(&payload_void, ALIGNMENT, len) != 0) {
                    std::cerr << "posix_memalign failed\n";
                    exit(EXIT_FAILURE);
                }
                unsigned char* payload = static_cast<unsigned char*>(payload_void);
                io->recv_data(payload, len);

				auto lts = std::chrono::steady_clock::now();


				// ---------------------
				// ALIGNED BUFFERS
				// -------------------------
				std::vector<off_t> aligned_left_offset(num_buckets);
				std::vector<off_t> aligned_right_offset(num_buckets);
				std::vector<size_t> aligned_num_bytes(num_buckets);
				std::vector<size_t> aligned_cumulative_bytes(num_buckets + 1, 0);

				for ( int i=0; i < num_buckets; i++){

					off_t true_offset = position[i] * bucket_size * ctx_block_size;

					off_t left = true_offset - (true_offset % ALIGNMENT);
					off_t right = ((true_offset + ( bucket_size * ctx_block_size ) + ALIGNMENT - 1)/ ALIGNMENT)*ALIGNMENT;

					aligned_left_offset[i] = left;
					aligned_right_offset[i] = right;
					aligned_num_bytes[i] = right - left;
					aligned_cumulative_bytes[i + 1] = aligned_cumulative_bytes[i] + aligned_num_bytes[i];					
				}

				size_t aligned_total_bytes = aligned_cumulative_bytes[num_buckets];



				//=================================
				// ISSUING READS before Modify
				//=================================
				
				size_t R_aligned_total_bytes = num_buckets * ALIGNMENT;
				void* R_buf_left;
				void* R_buf_right;
				if (posix_memalign(&R_buf_left, ALIGNMENT, R_aligned_total_bytes) != 0 ||
					posix_memalign(&R_buf_right, ALIGNMENT, R_aligned_total_bytes) != 0 )
				 {
					std::cerr << "posix_memalign failed for tmp_write_buf\n";
					exit(EXIT_FAILURE);
				}
				unsigned char* R_tmp_left = static_cast<unsigned char*>(R_buf_left);
				unsigned char* R_tmp_right = static_cast<unsigned char*>(R_buf_right);
				std::memset(R_tmp_left, 0, R_aligned_total_bytes);
				std::memset(R_tmp_right, 0, R_aligned_total_bytes);


				size_t submitted = 0;
				size_t in_flight = 0;
				// cout << "num IOs: " << 2* num_buckets << endl;
				while (submitted < 2 * num_buckets) {
					
					unsigned char* dst;
					off_t read_offset;
					size_t count = submitted / 2;

					if(submitted % 2){
						dst = R_tmp_left + count * ALIGNMENT;
						read_offset = aligned_left_offset[count];						
					}
					else{
						dst = R_tmp_right + count * ALIGNMENT;
						read_offset = aligned_right_offset[count] - ALIGNMENT;
					}

					io_uring_sqe* sqe = io_uring_get_sqe(&ring);
					if (!sqe) break;


					io_uring_prep_read(sqe, disk_fd, dst, ALIGNMENT, read_offset);
					io_uring_sqe_set_data(sqe, (void*)(uintptr_t)submitted);
					
					in_flight++;
					submitted++;
					
					if (in_flight >= io_max || submitted == 2 * num_buckets) {
						
						io_uring_submit(&ring);

						for (size_t j = 0; j < in_flight; j++) {
							struct io_uring_cqe* cqe;
							int ret = io_uring_wait_cqe(&ring, &cqe);
							if (ret < 0) {
								std::cerr << "io_uring_wait_cqe failed: " << strerror(-ret) << std::endl;
								exit(EXIT_FAILURE);
							}
							if (cqe->res < 0) {
								std::cerr << "IO operation failed: " << strerror(-cqe->res) << std::endl;
								exit(EXIT_FAILURE);
							}
							io_uring_cqe_seen(&ring, cqe);
						}
						in_flight = 0;
					}
				}


				//================================
				// PREPARE ALIGNED WRITE PAYLOAD
				//==============================

				void* tmp_write_buf_void;
				if (posix_memalign(&tmp_write_buf_void, ALIGNMENT, aligned_total_bytes) != 0) {
					std::cerr << "posix_memalign failed for tmp_write_buf\n";
					exit(EXIT_FAILURE);
				}
				unsigned char* tmp_write_buf = static_cast<unsigned char*>(tmp_write_buf_void);
				std::memset(tmp_write_buf, 0, aligned_total_bytes);
				

				#pragma omp parallel for num_threads(NUM_THREADS)
				for (size_t i = 0; i < num_buckets; i++) {

					off_t true_offset = (position[i] * bucket_size) * ctx_block_size;

					size_t bucket_bytes = bucket_size * ctx_block_size;

					// off_t left_start = aligned_left_offset[0];
					size_t left_bytes = true_offset - aligned_left_offset[i];


					size_t right_bytes = aligned_right_offset[i] - ( true_offset + bucket_bytes ); 
					off_t R_right_start = ALIGNMENT - right_bytes;

					
					std::memcpy(
						tmp_write_buf + aligned_cumulative_bytes[i], 
						R_tmp_left + i * ALIGNMENT, 
						left_bytes
					);
					std::memcpy(
						tmp_write_buf + aligned_cumulative_bytes[i] + left_bytes, 
						payload + i* bucket_bytes, 
						bucket_bytes
					);
					std::memcpy(
						tmp_write_buf + aligned_cumulative_bytes[i] + left_bytes + bucket_bytes, 
						R_tmp_right + i * ALIGNMENT + R_right_start, 
						right_bytes
					);

				}

				// ================================
				// ISSUE WRITE REQUESTS
				// ================================

				submitted = 0;
				in_flight = 0;
				// cout << "num IOs: " << num_buckets << endl;
				while (submitted < num_buckets) {
					io_uring_sqe* sqe = io_uring_get_sqe(&ring);
					if (!sqe) break;

					off_t write_offset = aligned_left_offset[submitted];
					unsigned char* src = tmp_write_buf + aligned_cumulative_bytes[submitted];
					size_t write_len = aligned_num_bytes[submitted];

					io_uring_prep_write(sqe, disk_fd, src, write_len, write_offset);
					io_uring_sqe_set_data(sqe, (void*)(uintptr_t)submitted);
					
					in_flight++;
					submitted++;
					
					if (in_flight >= io_max || submitted == num_buckets) {

						io_uring_submit(&ring);
						
						for (size_t j = 0; j < in_flight; j++) {
							struct io_uring_cqe* cqe;
							int ret = io_uring_wait_cqe(&ring, &cqe);
							if (ret < 0) {
								std::cerr << "io_uring_wait_cqe failed: " << strerror(-ret) << std::endl;
								exit(EXIT_FAILURE);
							}
							if (cqe->res < 0) {
								std::cerr << "IO operation failed: " << strerror(-cqe->res) << std::endl;
								exit(EXIT_FAILURE);
							}
							io_uring_cqe_seen(&ring, cqe);
						}
						in_flight = 0;
					}
				}

				if(rt == WriteBatch_R){
					// this case wont be called other than Reshuffling thats why
					online_server_side_time += (std::chrono::steady_clock::now() - lts);
				}

                if (integrity) {
                    if(rt == WriteBatch) {
                        update_hash(position, payload);
                    } else {
                        update_hash_reshuffle(position, payload);
                    }
                }



				free(R_tmp_left);
				free(R_tmp_right);
				free(tmp_write_buf);
                free(payload);
                break;
            }

            case End: {
                std::cout << "Online time spent at server: " << online_server_side_time.count()*1000 << " ms\n";
                std::cout << "Closing io_uring server...\n";
                io_uring_queue_exit(&ring);
                return;
            }

            default:
                std::cerr << "Unknown request code: " << rt << std::endl;
                assert(0);
        }
    }
}






/////////////////////// CLIENT SIDE ////////////////////////

void RemoteRing::ReadBlockBatchAsBlockRing(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, std::vector<bool> &valids, bool isReshuffle, bool isSetup){

	// isSetup = "true" when oram init_top_cache() calls it, we avoid doing integrity checks
	// isSetup = "false" when reshuffle/eviction

	// cout << "ReadBlockBatchAsBlockRing: 1" << "reading: " << positions.size() << " blocks" << endl;

	
	


	int rounds = io->num_rounds;
	long comm = io->counter;
	
	int rt = isReshuffle ? ReadBatchBlock_R : ReadBatchBlock;

	// auto lts = std::chrono::steady_clock::now();
	diskann::Timer oram_timer;

	io->send_data(&rt, sizeof(int));

	size_t num_blocks = positions.size();
	io->send_data(&num_blocks, sizeof(size_t));
	io->send_data(positions.data(), sizeof(int)*num_blocks);
	io->send_data(offsets.data(), sizeof(int)*num_blocks);
	// cout << "ReadBucketBatch num_blocks: " << num_blocks << endl;

	size_t len = num_blocks * (ctx_block_size);
	// cout << "ReadBucketBatch allocate payload for size: " << len << endl;
	unsigned char* payload = new unsigned char[len];
	// cout << "ReadBucketBatch allocate done" << endl;

	// long long rbbns = rbbnonsend.elapsed();
	// diskann::Timer rbbnonrecv;

	io->recv_data(payload, len);
	io->recv_data(&comm, sizeof(long));


	// if (!integrity) {  // semi-honest case
	// 	std::this_thread::sleep_for(std::chrono::milliseconds(2));
	// }


	// long long rbbnonserver=0;
	// io->recv_data(&rbbnonserver, sizeof(long long));

	// long long rbbnr = rbbnonrecv.elapsed();

	
	// diskann::Timer rbbnoncomput;

	// cout << "sent and recieved oram cache" << endl;

	// std::chrono::duration<double> oram_reshuffle_wait_time = (std::chrono::steady_clock::now() - lts);
	// this->current_query_stats->oram_wait_time += oram_reshuffle_wait_time;
	if(rt == ReadBatchBlock_R){
		this->current_query_stats->oram_wait_time_us += oram_timer.elapsed();
		oram_timer.reset();
	}


	if(isReshuffle){
		rounds_for_reshuffles += (io->num_rounds - rounds);
		comm_for_reshuffles += (sizeof(int) + sizeof(size_t) + 2*sizeof(int)*num_blocks);
		server_comm_for_reshuffles += len;

	} else {
		rounds_for_evictions += (io->num_rounds - rounds);
		comm_for_evictions += (sizeof(int) + sizeof(size_t) + 2*sizeof(int)*num_blocks);
		server_comm_for_evictions += len;
	}


	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);
	blocks.resize(num_blocks);

	// cout << "ReadBlockBatchAsBlockRing: recv_data" << endl;
	
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t block_id = 0; block_id < num_blocks; block_id++){
		// printf("numThreads = %d\n", omp_get_num_threads()); 
		unsigned char* ctx_payload = payload + block_id * ctx_block_size;
	
		unsigned char* ptx_payload = new unsigned char[ctx_block_size];
		size_t ptx_len = decrypt_wrapper(ctx_payload, ctx_block_size, ptx_payload);
		
		Block* b = new Block(ptx_block_size);
		b->from_ptr(ptx_payload);
		blocks[block_id] = b;


		delete[] ptx_payload;
	}

	// cout << "before integrity" << endl;
	// long long rbbnoncomp = rbbnoncomput.elapsed();

	// diskann::Timer rbbinteg;
	
	//no need for integrity verification when ORAM tree top caching is done

	if(integrity){
		if(!isReshuffle){
			recv_and_verify_hash_bucket(
				positions,
				offsets,
				payload,
				valids
			);
		} else{
			recv_and_verify_hash_reshuffle(
				positions,
				offsets,
				payload,
				valids
			);
		}
	}


	// long long rbbserv;
	// io->recv_data(&rbbserv, sizeof(long long));
	// cout << "rbbnon comm: " << comm << endl;
	// cout << " rrbnoninteg: " << rbbns << "+"  << rbbnonserver << "+" << rbbnr << "+" << rbbnoncomp << " rbbinteg: " << rbbinteg.elapsed() << endl;

	// cout << "after integrity" << endl;

	// cout << "ReadBlockBatchAsBlockRing: recv_data done" << endl;

	delete[] payload;
}


void RemoteRing::ReadBlockBatchAsBlockRingXor(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, size_t num_real_blocks, std::vector<bool>& valids){

	// cout << "rbbabrXOR" << endl;
	// cout << "ReadBlockBatchAsBlockRingXor - pos_len:  " << positions.size() << ", offsets_len: " << offsets.size() << ", num_real_blocks: " << num_real_blocks << endl; 

	// cout << "ReadBlockBatchAsBlockRingXor - valids_len:  " << valids.size() << ", num_levels: " << num_levels << endl;
	
	int rounds = io->num_rounds;
	long comm = io->counter;

	int rt = ReadBatchBlockXor;

	diskann::Timer oram_timer;
  // auto lts = std::chrono::steady_clock::now();
	io->send_data(&rt, sizeof(int));

	size_t num_blocks = positions.size();
	// assert(num_blocks % num_levels == 0);
	// size_t num_real_blocks = num_blocks / num_levels;

	// no need for ivs
	size_t len = num_real_blocks * (ctx_block_size - 16);

	unsigned char* payload = new unsigned char[len];
	unsigned char* ivs = new unsigned char[num_blocks*16];
	unsigned char* dexor_payload;
	if(integrity){
		dexor_payload = new unsigned char[num_blocks * ctx_block_size];
	}

	// cout << "numblocks hit:" << num_blocks << " num_real_blocks:" << num_real_blocks << endl;

	io->send_data(&num_blocks, sizeof(size_t));
	io->send_data(&num_real_blocks, sizeof(size_t));
	io->send_data(positions.data(), sizeof(int)*num_blocks);
	io->send_data(offsets.data(), sizeof(int)*num_blocks);

	io->recv_data(payload, len);
	io->recv_data(ivs, num_blocks*16);

	rounds_for_oram_access += (io->num_rounds - rounds);
	comm_for_oram_access += (io->counter - comm);

	io->recv_data(&comm, sizeof(long));
	server_comm_for_oram_access += comm;

  	// std::chrono::duration<double> oram_read_wait_time = (std::chrono::steady_clock::now() - lts);
	// this->current_query_stats->oram_wait_time += oram_read_wait_time;
	if (this->current_query_stats)
		this->current_query_stats->oram_wait_time_us += oram_timer.elapsed();
	oram_timer.reset();

	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);
	blocks.resize(num_real_blocks);

	// assert(valids.size() == num_levels*num_real_blocks);

	// #pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t block_id = 0; block_id < num_real_blocks; block_id++){
		unsigned char* ctx_payload = payload + block_id * (ctx_block_size - 16);
		int valid_block_offset = -1;

		// dummy block for de-xor
		Block dummy_block(ptx_block_size);
		for(int i = 0; i < num_blocks / num_real_blocks; i++){
			size_t block_offset = block_id*(num_blocks / num_real_blocks) + i;

			if(integrity){
				// copy the ivs to dexor payload
				memcpy(
					dexor_payload + block_offset*ctx_block_size,
					ivs + block_offset*16, 
					16
				);
			}

			if(valids[block_offset]){
				// Skip the valid block
				assert(valid_block_offset == -1);//only 1 valid block for each real block path
				valid_block_offset = block_offset;
			} else{
				// Undo xor of dummies
				unsigned char* dummy_ctx = new unsigned char[ctx_block_size];
				unsigned char* dummy_ptx = new unsigned char[ctx_block_size];
				
				dummy_block.to_ptr(dummy_ptx);
				size_t ctx_len = encrypt_wrapper_with_iv(
					ivs + block_offset*16, 
					dummy_ptx, 
					per_block_size*Bucket::getMaxSize(), 
					dummy_ctx
				);

				assert(ctx_len == ctx_block_size - 16);

				// for (int i = 0; i < 10; i++) { // Adjust the length as needed
				// 	// Convert each byte to an unsigned value and print as hex
				// 	std::cout << std::hex << std::setw(2) << std::setfill('0') 
				// 			<< static_cast<int>(static_cast<unsigned char>(dummy_ctx[i])) << " ";
				// }
				// std::cout << std::endl;

				if(integrity){
					// copy the dummy ctx to dexor payload
					memcpy(
						dexor_payload + block_offset*ctx_block_size + 16,
						dummy_ctx, 
						ctx_block_size - 16
					);
				}

				for(int j = 0; j < ctx_block_size - 16; j++){
					// de-xor
					ctx_payload[j] = ctx_payload[j] ^ dummy_ctx[j];
				}

				delete[] dummy_ctx;
				delete[] dummy_ptx;
			}
		}

		if(integrity){
			// copy the real ctx to dexor payload
			// ctx_payload is already dexored, so its equal to ctx of real block
			memcpy(
				dexor_payload + valid_block_offset*ctx_block_size + 16,
				ctx_payload, 
				ctx_block_size - 16
			);
		}

		assert(valid_block_offset != -1);

		unsigned char* ptx_payload = new unsigned char[ctx_block_size];
		// cout << "try decrypt" << endl;
		
		// size_t cipher_len = ctx_block_size - 16;
		// cout << "Cipher length: " << cipher_len << endl;

		// cout << "IV: ";
		// for (int i = 0; i < 16; i++)
		// 	cout << std::hex << (int)ivs[valid_block_offset * 16 + i] << " ";
		// cout << endl;

		// cout << "Ciphertext head: ";
		// for (int i = 0; i < 32; i++)
		// 	cout << std::hex << (int)ctx_payload[i] << " ";
		// cout << endl;


		size_t ptx_len = decrypt_wrapper_with_iv(
			ivs + valid_block_offset* 16, 
			ctx_payload, 
			ctx_block_size - 16, 
			ptx_payload);

		// cout << "try haha" << endl;
		Block* b = new Block(ptx_block_size);
		b->from_ptr(ptx_payload);

		//Payload is getting nbr ids as 0

		// cout << "ptx payload\n";
		// for(int i=0; i<ctx_block_size; i++){
		// 	cout << static_cast<int>(ptx_payload[i]) << " ";
		// }
		// cout << endl;

		// cout << "got this block: ";
		// cout << "block says id: " << b->index << endl;
		// if(b->index != -1){ b->printBlock();}
		// cout << endl;
		// cout << "returning from rbbabx" << endl;

		// cout << "rbbabx after block payload loading" << endl;


		// int temp_bs = ptx_block_size*sizeof(int);
		// string data_holder = "";
		// for (int i = 0; i< temp_bs; i++) {
		// 	data_holder += to_string(ptx_payload[i]);
		// 	data_holder += " ";
		// }
		// cout <<  "ptx_payload: " << data_holder << endl;


		// cout << "bytes of ptx_payload: ";
		// unsigned char* bptx = (ptx_payload);
		// cout << bptx[0] << bptx[1] <<bptx[2] << bptx[3] << bptx[4] << bptx[5] << bptx[6] << bptx[7] << endl; 

		// b->printBlockByte();
		// b->printBlock();



		// if(b->index == 38400259){
		// 	cout << "printBlockInt" << endl;
		// 	b->printBlockInt();

		// 	cout << "ptx payload stream " << ptx_block_size <<  endl;
		// 	int* ptxpld = (int*)ptx_payload;
		// 	for (size_t i = 0; i < ptx_block_size ; ++i) {
		// 		cout << ptxpld[i] << " " ;
		// 	}
		// 	cout << "done" << endl;
		// 	abort();
		// }

		blocks[block_id] = b;
		delete[] ptx_payload;
	}

	if(integrity){
		recv_and_verify_hash(
			positions,
			offsets,
			dexor_payload,
			valids
		);
	}

	// cout << "ReadBlockBatchAsBlockRingXor - done."  << endl; 

	delete[] ivs;
	delete[] payload;
}

void RemoteRing::WriteBucketBatchMapAsBlockRing(const std::map<int, vector<Block*>>& bucket_to_write, int bucket_size, bool isReshuffle){

	// cout << "WriteBucketBatchMapAsBlockRing" << endl;

	// diskann::Timer wbbnoninteg;

	int rounds = io->num_rounds;
	int comm = io->counter;
	
	int rt = isReshuffle ? WriteBatch_R : WriteBatch;
	io->send_data(&rt, sizeof(int));

	size_t num_buckets = bucket_to_write.size();
	io->send_data(&num_buckets, sizeof(size_t));

	size_t num_blocks = num_buckets*bucket_size;

	size_t len = num_blocks * (ctx_block_size);
	unsigned char* payload = new unsigned char[len];
	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);

	std::vector<int> positions;
	size_t bucket_id = 0;
	vector<vector<Block*>> bkts;

	// reverse order!!!
	for (auto it = bucket_to_write.rbegin(); it != bucket_to_write.rend(); ++it) {
		positions.push_back(it->first);
		bkts.push_back(it->second);
	}

	#pragma omp parallel for num_threads(NUM_THREADS)
	for (int bucket_id = 0; bucket_id < positions.size(); bucket_id++) {
		for(int block_id = 0; block_id < bucket_size; block_id++){
			unsigned char* ptx_payload = new unsigned char[ctx_block_size];
			bkts[bucket_id][block_id]->to_ptr(ptx_payload);
			unsigned char* ctx_payload = payload + (bucket_id * bucket_size + block_id) * ctx_block_size;
			size_t ctx_len = encrypt_wrapper(ptx_payload, per_block_size*Bucket::getMaxSize(), ctx_payload);
			delete[] ptx_payload;
			delete bkts[bucket_id][block_id];
		}
	}

	
	io->send_data(positions.data(), positions.size()*sizeof(int));
	io->send_data(payload, len);

	if(isReshuffle){
		rounds_for_reshuffles += (io->num_rounds - rounds);
		comm_for_reshuffles += (sizeof(int) + sizeof(size_t) + positions.size()*sizeof(int) + len);
		// comm_for_reshuffles += 0;
	} else {
		rounds_for_evictions += (io->num_rounds - rounds);
		comm_for_evictions += (sizeof(int) + sizeof(size_t) + positions.size()*sizeof(int) + len);
		// comm_for_reshuffles += 0;
	}

	// TODO: this could be optimized
	// Instead of sending the hashes, the server could just recompute by it self
	
	// long long wbbnon = wbbnoninteg.elapsed();


	// diskann::Timer wbbinteg;
	if(integrity){
		if(!isReshuffle){
			update_mt(positions, payload);
		} else{
			update_mt_reshuffle(positions, payload);
		}
	}
	// cout << "wbbnoninteg: " << wbbnon << " wbbinteg: " << wbbinteg.elapsed() << endl;
	delete[] payload;

	io->last_call = LastCall::Recv;
}


// ----------------------------- hash ---------------------------------- //

void RemoteRing::sync_roots(){

	cout << "SYNCING ROOTS: bucket pos:" << endl;

	size_t bucket_id = (1 << oram_config.cached_levels) - 1;

	for(size_t hid = 0; hid < (1 << oram_config.cached_levels); hid++){
		cout << bucket_id << " " ;
		bucket_id ++;
	}
	cout << endl;

    if(is_server){
		// Send the hashes of *****UNCACHED**** roots

		/*
		if i cache 6 levels i.e. 0,1,2,...,5
		7th level will have 2^6 UNCACHED roots of subtree in primary merkle tree		
		*/

		cout << "sync hash start ..." << "oram cache levels=" << oram_config.cached_levels << endl;

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


		//the server is also sending hash of cached buckets in the outermt
		//in practice the client would already have it, or can compute it from its cache. but this function is simplest place to add this implementation
		size_t start_bkt = 0;
		size_t first_uncached_bkt = (1 << oram_cached_levels) - 1;

		uint8_t* cached_roots = new uint8_t[first_uncached_bkt*SHA256_DIGEST_LENGTH];

		for(size_t bid = start_bkt; bid < first_uncached_bkt; bid++){
			memcpy(
				cached_roots + bid*SHA256_DIGEST_LENGTH,
				tree_hash + bid*SHA256_DIGEST_LENGTH,
				SHA256_DIGEST_LENGTH
			);
		}
		io->send_data(cached_roots, first_uncached_bkt*SHA256_DIGEST_LENGTH);


    } else{
		size_t oram_cached_levels = oram_config.cached_levels;

		// multiple roots in case of oram caching
		size_t roots_length = (1 << oram_cached_levels)*SHA256_DIGEST_LENGTH;

		this->roots.resize(roots_length);
		io->recv_data(this->roots.data(), roots_length);

		// put roots in the hash map
		size_t bucket_id = (1 << oram_cached_levels) - 1;
		for(size_t hid = 0; hid < (1 << oram_cached_levels); hid++){
			// cout << "add root id: " << bucket_id << endl;
			hash_map[bucket_id] = vector<uint8_t>(
				this->roots.begin() + hid*SHA256_DIGEST_LENGTH, 
				this->roots.begin() + (hid+1)*SHA256_DIGEST_LENGTH
			);
			// cout << "hash_map updated at pos" << bucket_id << endl;
			bucket_id ++;
		}


		cout << "client hash_map[bkt] after sync_roots()" << endl;
		for (const auto& [key, vec] : hash_map) {
			if (!vec.empty()) {
				std::cout << key << " -> " << static_cast<int>(vec[0]) << "\n";
			} else {
				std::cout << key << " -> (empty vector)\n";
			}
		}


		// client still needs to know the hash of CACHED buckets in the outermt
		// here we just make the server also send those hashes, though in practice, the client would have computed these before sending oram buckets to server
		size_t first_uncached_bkt = (1 << oram_cached_levels) - 1;
		uint8_t* cached_roots = new uint8_t[first_uncached_bkt*SHA256_DIGEST_LENGTH];

		io->recv_data(cached_roots, first_uncached_bkt*SHA256_DIGEST_LENGTH);

		for(size_t bid = 0; bid < first_uncached_bkt; bid++){
			hash_map[bid] = vector<uint8_t>(
				cached_roots + bid*SHA256_DIGEST_LENGTH, 
				cached_roots + (bid+1)*SHA256_DIGEST_LENGTH
			);

			// cout << "hash_map updated at pos" << bid << endl;
		}

	}
}

// server hashes
void RemoteRing::send_hash(std::vector<int> &position, std::vector<int> &offset){
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

void RemoteRing::send_hash_bucket(std::vector<int> &position, std::vector<int> &offset){

	// cout << "is this at eviction" << endl;

	size_t num_buckets = position.size() / oram_config.real_bucket_size;
	std::vector<int> bucket_positions;

	for(size_t i = 0 ; i < num_buckets; i++){
		bucket_positions.push_back(position[i*oram_config.real_bucket_size]);
	}

	// cout << "send_hash bucket with bucket_size: " << num_buckets << endl;	

	size_t num_hashes = num_buckets * oram_config.bucket_size;
	uint8_t* inner_leaves_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];

	size_t first_leaf_off = (per_bucket_hashes - 1) / 2; // offset to the first leaf node

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
}

void RemoteRing::send_hash_reshuffle(std::vector<int> &position, std::vector<int> &offset){
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

void RemoteRing::update_hash(std::vector<int> &position, uint8_t* payload){
	size_t num_buckets = position.size();
	size_t num_blocks = num_buckets * oram_config.bucket_size;

	// hash for each block
	uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * num_blocks];

	// uint8_t* recvd_macs = new uint8_t[SHA256_DIGEST_LENGTH*num_blocks];
	// io->recv_data(recvd_macs, num_blocks*SHA256_DIGEST_LENGTH);


	// #pragma omp parallel for num_threads(NUM_THREADS)
	// for(size_t i = 0; i < num_blocks; i++){
	// 	sha256_wrapper(payload + i*ctx_block_size, ctx_block_size, per_block_hash + i*SHA256_DIGEST_LENGTH);


	// 	assert(
	// 		(int)(per_block_hash + i*SHA256_DIGEST_LENGTH)[0] 
	// 		== 
	// 		(int)(recvd_macs + i*SHA256_DIGEST_LENGTH)[0]
	// 	);
	// }
	
	io->recv_data(per_block_hash, num_blocks*SHA256_DIGEST_LENGTH);



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

void RemoteRing::update_hash_reshuffle(std::vector<int> &position, uint8_t* payload){
	// cout << "update_mt_reshuffle" << endl;
	// assert(0);

	size_t num_buckets = position.size();
	size_t num_blocks = num_buckets * oram_config.bucket_size;

	// hash for each block
	uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * num_blocks];


	// uint8_t* recvd_macs = new uint8_t[SHA256_DIGEST_LENGTH * num_blocks];
	// io->recv_data(recvd_macs, num_blocks*SHA256_DIGEST_LENGTH);

	// #pragma omp parallel for num_threads(NUM_THREADS)
	// for(size_t i = 0; i < num_blocks; i++){
	// 	sha256_wrapper(payload + i*ctx_block_size, ctx_block_size, per_block_hash + i*SHA256_DIGEST_LENGTH);

	// 	assert(
	// 		(int)(per_block_hash + i*SHA256_DIGEST_LENGTH)[0] 
	// 		== 
	// 		(int)(recvd_macs + i*SHA256_DIGEST_LENGTH)[0]
	// 	);
	// }

	io->recv_data(per_block_hash, num_blocks*SHA256_DIGEST_LENGTH);

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


// client hashes
void RemoteRing::recv_and_verify_hash(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* dexor_payload, std::vector<bool> &valid){

	// cout << "recv_and_verify_hash_2..." << endl;

	diskann::Timer oram_wait;

	size_t num_hashes = position.size() * (per_bucket_tree_height - 1);
	// cout << "recv_and_verify_hash_2333..." << endl;
	uint8_t* inner_mt_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];



	// cout << "reconstruct bucket hash 0" << endl;

	// sibling and ancestors of block in  innermt, EXPECT the root of innermt
	io->recv_data(inner_mt_payload, num_hashes*SHA256_DIGEST_LENGTH);

	// cout << "reconstruct bucket hash 1" << endl;

	uint8_t* outer_mt_payload = new uint8_t[position.size() *SHA256_DIGEST_LENGTH];
	uint8_t* reconstruct_bucket_hash = new uint8_t[position.size() *SHA256_DIGEST_LENGTH];

	//hash of siblings of all bkts in position
	io->recv_data(outer_mt_payload, position.size() *SHA256_DIGEST_LENGTH);


	
	// communication metrics for malicious security @ oram access
	//rounds is weird to calc
	this->current_query_stats->oram_wait_time_us += oram_wait.elapsed();
	mal_server_comm_for_oram_access += (num_hashes + position.size())*SHA256_DIGEST_LENGTH;



	// cout << "reconstruct bucket hash" << endl;
	
	// reconstruct per bucket hash
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < position.size(); i++){
		
		size_t pos = position[i];
		size_t cur_offset = offset[i] + (per_bucket_hashes + 1) / 2 - 1;
		// unsigned char* ctx = dexor_payload + valid_cnt*ctx_block_size;
		unsigned char* ctx = dexor_payload + i*ctx_block_size;
		uint8_t* hash = reconstruct_bucket_hash + i*SHA256_DIGEST_LENGTH;
		sha256_wrapper(ctx, ctx_block_size, hash);

		for(int cur_height = per_bucket_tree_height; cur_height > 1; cur_height --){
			uint8_t* sib_hash = inner_mt_payload + i * (per_bucket_tree_height - 1) * SHA256_DIGEST_LENGTH + (per_bucket_tree_height - cur_height) * SHA256_DIGEST_LENGTH;

			// determine left and right
			uint8_t* l_hash = hash;
			uint8_t* r_hash = sib_hash;

			if(cur_offset % 2 == 0){
				l_hash = sib_hash;
				r_hash = hash;
			} 

			sha256_twin_input_wrapper(
				l_hash, SHA256_DIGEST_LENGTH,
				r_hash, SHA256_DIGEST_LENGTH,
				hash
			);
			cur_offset = (cur_offset - 1) / 2;

		}
	}

	// reconstuct tree hash
	// assume that the positions in each path are from bottom to top 

	// cout << "reconstruct tree hash" << endl;
	for(size_t i = 0; i < position.size(); i++){
		// cout << "reconstructing pos: " << position[i] << endl;

		int pos = position[i];
		int sib = get_sibling3(pos);

		uint8_t* sib_hash = outer_mt_payload + i*SHA256_DIGEST_LENGTH;

		// cout << "inserting sib: " << sib << endl;
		if(!verify_and_insert(sib, sib_hash)){
			cout << "Hash verification fails at sib: " << sib << endl;
			assert(0);
		}

		// Compute and insert current hash
		uint8_t* bucket_hash = reconstruct_bucket_hash + i*SHA256_DIGEST_LENGTH;
		uint8_t* tree_bucket_hash = new uint8_t[SHA256_DIGEST_LENGTH];
		if(pos >= capacity / 2){
			// leaf bucket
			memcpy(tree_bucket_hash, bucket_hash, SHA256_DIGEST_LENGTH);
		} else{
			int l = 2*pos + 1;
			int r = l + 1;
			auto lit = hash_map.find(l);
			auto rit = hash_map.find(r);

			if(lit == hash_map.end() || rit == hash_map.end()){
				// This shouldn't happen
				assert(0);
			}

			sha256_trib_input_wrapper(
				bucket_hash, SHA256_DIGEST_LENGTH,
				lit->second.data(), SHA256_DIGEST_LENGTH,
				rit->second.data(), SHA256_DIGEST_LENGTH,
				tree_bucket_hash
			);

			// assert(0);
		}


		if(!verify_and_insert(pos, tree_bucket_hash)){
			cout << "Hash verification fails at pos: " << pos << endl;
			assert(0);
		}

		delete[] tree_bucket_hash;
	
	}

	// cout << "recv_and_verify_hash: success!" << endl;
	// assert(0);

	delete[] inner_mt_payload;
	delete[] outer_mt_payload;
	delete[] reconstruct_bucket_hash;
}

void RemoteRing::recv_and_verify_hash_bucket(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid){
	
	// cout << "recv_and_verify_hash_bucket..." << endl;
	// cout << position.size() << " " << oram_config.real_bucket_size << endl;
	
	// THIS ASSERTION DOES NOT HOLD WHEN ORAM CACHING calls recv and verify hash bucket, that is why its commented. OTHERWISE IT SHOULD FOLLOW
	assert(position.size() % oram_config.real_bucket_size == 0);



	size_t num_buckets = position.size() / oram_config.real_bucket_size;

	// here we just fetch all block's hashes for each bucket
	// size_t inner_mt_num_leaves = 1 << (oram_config.bucket_size - 1);
	size_t num_hashes = num_buckets*oram_config.bucket_size;
	uint8_t* inner_leaves_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];

	uint8_t* outer_mt_payload = new uint8_t[num_buckets*SHA256_DIGEST_LENGTH];
	uint8_t* reconstruct_bucket_hash = new uint8_t[num_buckets *SHA256_DIGEST_LENGTH];

	io->recv_data(inner_leaves_payload, num_hashes*SHA256_DIGEST_LENGTH);
	io->recv_data(outer_mt_payload, num_buckets *SHA256_DIGEST_LENGTH);


	mal_server_comm_for_evictions += (num_hashes + num_buckets)*SHA256_DIGEST_LENGTH;
	
	// VERIFICATION 1: hash of each block

	// overwrite the fetched blocks hashes
	// instead 
	// client calculates hash for each blocks and verifies equality with inner_leaves_payload
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < position.size(); i++){
		size_t pos = position[i];
		size_t off = offset[i];
		size_t bucket_id = i / oram_config.real_bucket_size;

		unsigned char* ctx = payload + i*ctx_block_size;
		uint8_t* ref = inner_leaves_payload + (bucket_id*oram_config.bucket_size + off) * SHA256_DIGEST_LENGTH;
		
		// sha256_wrapper(ctx, ctx_block_size, ref);
		
		// check per block hash for each buckets
		uint8_t* ref_calc = new uint8_t[SHA256_DIGEST_LENGTH];
		sha256_wrapper(ctx, ctx_block_size, ref_calc);
		
		// calculated hash should be equal to hash recvd
		assert(*ref_calc == *ref);
	}


	// VERIFICATION 1.1: copy inner leaf hashes to reconstruct innermt at client

	// copy the hashes to a larger chunk for eaiser reconstruction
	size_t inner_mt_size = num_buckets*per_bucket_hashes*SHA256_DIGEST_LENGTH;
	uint8_t* inner_mt = new uint8_t[inner_mt_size];
	memset(inner_mt, 0, inner_mt_size);

	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0 ; i < num_buckets; i++){
		memcpy(
			inner_mt + (i*per_bucket_hashes + first_leaf_off)*SHA256_DIGEST_LENGTH,
			inner_leaves_payload + (i*oram_config.bucket_size)*SHA256_DIGEST_LENGTH,
			oram_config.bucket_size*SHA256_DIGEST_LENGTH
		);
	}
	// reconstruct inner mt for each bucket
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0 ; i < num_buckets; i++){
		for(size_t j = first_leaf_off; j >= 1; j--){
			size_t id = j-1;
			size_t lid = 2*j-1;
			size_t rid = 2*j;

			assert(lid <=  per_bucket_hashes - 1);

			uint8_t* dst = inner_mt + (i * per_bucket_hashes + id)* SHA256_DIGEST_LENGTH;
			uint8_t* lsrc = inner_mt + (i * per_bucket_hashes + lid)* SHA256_DIGEST_LENGTH;
			uint8_t* rsrc = inner_mt + (i * per_bucket_hashes + rid)* SHA256_DIGEST_LENGTH;

			sha256_twin_input_wrapper(
				lsrc, SHA256_DIGEST_LENGTH,
				rsrc, SHA256_DIGEST_LENGTH,
				dst
			);
		}
	}

	// VERIFICATION 2: reconstruct outermt - if bkt hash already there, verify equality. else insert to hash_map

	// reconstruct outer hash
	for(size_t i = 0 ; i < num_buckets; i++){
		int bucket_pos = position[i*oram_config.real_bucket_size];
		int sib = get_sibling3(bucket_pos);

		uint8_t* sib_hash = outer_mt_payload + i*SHA256_DIGEST_LENGTH;

		// verify sibling bkt hash
		if(!verify_and_insert(sib, sib_hash)){
			cout << "Hash verification fails at sibling bkt: " << sib << endl;
			assert(0);
		}

		// Compute and insert current hash
		uint8_t* bucket_hash = inner_mt + i * per_bucket_hashes * SHA256_DIGEST_LENGTH;
		uint8_t* tree_bucket_hash = new uint8_t[SHA256_DIGEST_LENGTH];
		if(bucket_pos >= capacity / 2){
			// leaf bucket
			memcpy(tree_bucket_hash, bucket_hash, SHA256_DIGEST_LENGTH);
		} else{
			int l = 2*bucket_pos + 1;
			int r = l + 1;
			auto lit = hash_map.find(l);
			auto rit = hash_map.find(r);

			// cout << "l:" << l << " r:" << r << endl;
			// for (const auto& [key, vec] : hash_map) {
			// 	if (!vec.empty()) {
			// 		std::cout << key << " -> " << static_cast<int>(vec[0]) << "\n";
			// 	} else {
			// 		std::cout << key << " -> (empty vector)\n";
			// 	}
			// }

			if(lit == hash_map.end() || rit == hash_map.end()){
				// This shouldn't happen
				assert(0);
			}

			sha256_trib_input_wrapper(
				bucket_hash, SHA256_DIGEST_LENGTH,
				lit->second.data(), SHA256_DIGEST_LENGTH,
				rit->second.data(), SHA256_DIGEST_LENGTH,
				tree_bucket_hash
			);

		}


		if(!verify_and_insert(bucket_pos, tree_bucket_hash)){
			cout << "Hash verification fails at pos: " << bucket_pos << endl;
			assert(0);
		}

		delete[] tree_bucket_hash;
	}

	delete[] inner_leaves_payload;
	delete[] outer_mt_payload;
	delete[] reconstruct_bucket_hash;
	delete[] inner_mt;

}

void RemoteRing::recv_and_verify_hash_reshuffle(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid){
	// cout << "recv and verify reshuffle" << endl;
	// Each bucket in early reshuffle doesn't have path constraint
	size_t num_buckets = position.size() / oram_config.real_bucket_size;


	diskann::Timer oram_wait;

	// here we just fetch all block's hashes for each bucket
	size_t num_hashes = num_buckets*oram_config.bucket_size;
	uint8_t* inner_leaves_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];
	io->recv_data(inner_leaves_payload, num_hashes*SHA256_DIGEST_LENGTH);

	this->current_query_stats->oram_wait_time_us += oram_wait.elapsed();

	mal_server_comm_for_reshuffles += num_hashes*SHA256_DIGEST_LENGTH;

	// VERIFICATION 1: per block hashes: recalculate and verify equality with recvd hashes of all blocks
	for(size_t i = 0; i < position.size(); i++){
		size_t pos = position[i];
		size_t off = offset[i];
		size_t bucket_id = i / oram_config.real_bucket_size;

		unsigned char* ctx = payload + i*ctx_block_size;
		// TODO opt this later
		uint8_t* hash = new uint8_t[SHA256_DIGEST_LENGTH];
		uint8_t* ref = inner_leaves_payload + (bucket_id*oram_config.bucket_size + off) * SHA256_DIGEST_LENGTH;

		sha256_wrapper(ctx, ctx_block_size, hash);

		for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			if(hash[j] != ref[j]){
				assert(0);
			}
		}

		delete[] hash;
	}

	// VERIFICATION 1.1: reconstruct innermt 
	// copy block hashes to leaf of innermt
	size_t inner_mt_size = num_buckets*per_bucket_hashes*SHA256_DIGEST_LENGTH;
	uint8_t* inner_mt = new uint8_t[inner_mt_size];
	memset(inner_mt, 0, inner_mt_size);

	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;
	for(size_t i = 0 ; i < num_buckets; i++){
		memcpy(
			inner_mt + (i*per_bucket_hashes + first_leaf_off)*SHA256_DIGEST_LENGTH,
			inner_leaves_payload + (i*oram_config.bucket_size)*SHA256_DIGEST_LENGTH,
			oram_config.bucket_size*SHA256_DIGEST_LENGTH
		);
	}

	delete[] inner_leaves_payload;

	// reconstruct bucket hash i.e. inner m
	for(size_t i = 0 ; i < num_buckets; i++){
		for(size_t j = first_leaf_off; j >= 1; j--){
			size_t id = j-1;
			size_t lid = 2*j-1;
			size_t rid = 2*j;

			assert(lid <=  per_bucket_hashes - 1);

			uint8_t* dst = inner_mt + (i * per_bucket_hashes + id)* SHA256_DIGEST_LENGTH;
			uint8_t* lsrc = inner_mt + (i * per_bucket_hashes + lid)* SHA256_DIGEST_LENGTH;
			uint8_t* rsrc = inner_mt + (i * per_bucket_hashes + rid)* SHA256_DIGEST_LENGTH;

			sha256_twin_input_wrapper(
				lsrc, SHA256_DIGEST_LENGTH,
				rsrc, SHA256_DIGEST_LENGTH,
				dst
			);
		}
	}

	// now we need 
	// 1. the bucket (inner root) hash of the ancestors of each bucket till the root (cached layer) 
	// 2. the full hash of the siblings and two immediate childrent if the bucket is not leaf node
	vector<int> bucket_positions;
	vector<int> inner_hash_pos;
	vector<int> outer_hash_pos;
	vector<int> inner_hash_interval;
	int interval = 0;
	for(int i = 0; i < num_buckets; i++){
		int pos = position[i*oram_config.real_bucket_size];
		bucket_positions.push_back(pos);

		if(pos >= capacity / 2){
			// leaf bucket
		} else{
			int l = 2*pos + 1;
			int r = l + 1;
			outer_hash_pos.push_back(l);
			outer_hash_pos.push_back(r);
		}

		// outer_hash_pos.push_back(get_sibling(pos));
		inner_hash_interval.push_back(interval);

		int pos_iter = pos;
		// loop ancestors
		while(pos_iter >= (1 << oram_config.cached_levels) - 1){
			outer_hash_pos.push_back(get_sibling3(pos_iter));
			inner_hash_pos.push_back(pos_iter);
			interval++;

			pos_iter = (pos_iter - 1) / 2;
		}
	}

	// std::cout << "interval: ";
	// for (const auto& element : inner_hash_interval) {
	// 	std::cout << element << ' ';
	// }
	// std::cout << '\n';

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

	oram_wait.reset();

	size_t inner_hash_size = inner_hash_pos.size();
	size_t outer_hash_size = outer_hash_pos.size();

	uint8_t* inner_hash_payload = new uint8_t[inner_hash_size*SHA256_DIGEST_LENGTH];
	uint8_t* outer_hash_payload = new uint8_t[outer_hash_size*SHA256_DIGEST_LENGTH];

	io->recv_data(inner_hash_payload, inner_hash_size*SHA256_DIGEST_LENGTH);
	io->recv_data(outer_hash_payload, outer_hash_size*SHA256_DIGEST_LENGTH);

	this->current_query_stats->oram_wait_time_us += oram_wait.elapsed();
	mal_server_comm_for_reshuffles += (inner_hash_size + outer_hash_size)*SHA256_DIGEST_LENGTH;

	// check inner hash consistency
	for(size_t i = 0; i < num_buckets; i++){
		uint8_t* local = inner_mt + i*per_bucket_hashes*SHA256_DIGEST_LENGTH;
		size_t interval = inner_hash_interval[i];
		uint8_t* remote = inner_hash_payload + interval*SHA256_DIGEST_LENGTH;

		if(memcmp(local, remote, SHA256_DIGEST_LENGTH) != 0){
			assert(0);
		}
	}

	// insert outer hashes
	for(size_t i = 0; i < outer_hash_size; i++){
		int pos = outer_hash_pos[i];

		// verify and insert outer hash
		vector<uint8_t> outer_hash = vector<uint8_t>(outer_hash_payload + i*SHA256_DIGEST_LENGTH, outer_hash_payload + (i+1)*SHA256_DIGEST_LENGTH);

		if(!verify_and_insert(pos, outer_hash.data())){
			cout << "Hash verification fails at outer bkt hash: " << pos << endl;
			assert(0);
		}
		
	}

	// reconstruct outer mt
	for(size_t i = 0; i < inner_hash_size; i++){

		int pos = inner_hash_pos[i];
		uint8_t* bucket_hash = inner_hash_payload + i*SHA256_DIGEST_LENGTH;
		reshuffle_inner_hash_map[pos] = vector<uint8_t>(bucket_hash, bucket_hash+SHA256_DIGEST_LENGTH);
		
		uint8_t* tree_bucket_hash = new uint8_t[SHA256_DIGEST_LENGTH];

		if(pos >= capacity / 2){
			// leaf bucket
			memcpy(tree_bucket_hash, bucket_hash, SHA256_DIGEST_LENGTH);
		} else{
			int l = 2*pos + 1;
			int r = l + 1;
			auto lit = hash_map.find(l);
			auto rit = hash_map.find(r);

			if(lit == hash_map.end() || rit == hash_map.end()){
				// This shouldn't happen
				assert(0);
			}

			sha256_trib_input_wrapper(
				bucket_hash, SHA256_DIGEST_LENGTH,
				lit->second.data(), SHA256_DIGEST_LENGTH,
				rit->second.data(), SHA256_DIGEST_LENGTH,
				tree_bucket_hash
			);
		}

		verify_and_insert(pos, tree_bucket_hash);
		delete[] tree_bucket_hash;
	}

	// communication 
	mal_server_comm_for_reshuffles += (num_hashes + inner_hash_size + outer_hash_size)*SHA256_DIGEST_LENGTH;
	
	delete[] inner_mt;
	delete[] inner_hash_payload;
	delete[] outer_hash_payload;
}

bool RemoteRing::verify_and_insert(int pos, uint8_t* hash){
	auto it = hash_map.find(pos);
	if(it == hash_map.end()){
		// This bucket is new to the merkle tree
		hash_map.insert(make_pair(pos, vector<uint8_t>(hash, hash + SHA256_DIGEST_LENGTH)));
		// cout << "hash_map updated at pos" << pos << endl;
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


void RemoteRing::update_mt(const std::vector<int> &position, unsigned char* payload){

	// cout << "update_mt" << endl;
	size_t num_buckets = position.size();
	size_t num_blocks = num_buckets * oram_config.bucket_size;

	// hash for each block
	uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * num_blocks];

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < num_blocks; i++){
		
		sha256_wrapper(payload + i*ctx_block_size, ctx_block_size, per_block_hash + i*SHA256_DIGEST_LENGTH);

	}


	//client should send MAC for each blocks, server shouldnt be able to compute it right?
	io->send_data(per_block_hash, num_blocks*SHA256_DIGEST_LENGTH);






	// assert(0);

	// Merkle Trees for each bucket
	size_t inner_mt_size = num_buckets * per_bucket_hashes * SHA256_DIGEST_LENGTH;
	uint8_t* inner_mt = new uint8_t[inner_mt_size];
	memset(inner_mt, 0, inner_mt_size);

	// copy leaf hashes
	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < num_buckets; i++){
		uint8_t* src = per_block_hash + i * oram_config.bucket_size * SHA256_DIGEST_LENGTH;
		uint8_t* dst = inner_mt + (i * per_bucket_hashes +  first_leaf_off) * SHA256_DIGEST_LENGTH;
		memcpy(dst, src, oram_config.bucket_size * SHA256_DIGEST_LENGTH);
	}

	// reconstruct inner mt
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0 ; i < num_buckets; i++){
		for(size_t j = first_leaf_off; j >= 1; j--){
			size_t id = j-1;
			size_t lid = 2*j-1;
			size_t rid = 2*j;

			assert(lid <=  per_bucket_hashes - 1);

			uint8_t* dst = inner_mt + (i * per_bucket_hashes + id)* SHA256_DIGEST_LENGTH;
			uint8_t* lsrc = inner_mt + (i * per_bucket_hashes + lid)* SHA256_DIGEST_LENGTH;
			uint8_t* rsrc = inner_mt + (i * per_bucket_hashes + rid)* SHA256_DIGEST_LENGTH;

			sha256_twin_input_wrapper(
				lsrc, SHA256_DIGEST_LENGTH,
				rsrc, SHA256_DIGEST_LENGTH,
				dst
			);

		}
	}

	// reconstruct outer mt
	for(size_t i = 0; i < num_buckets; i++){
		int bucket_pos = position[i];

		// cout << "reconstructing: " << bucket_pos << endl;

		uint8_t* bucket_hash = inner_mt + i * per_bucket_hashes * SHA256_DIGEST_LENGTH;
		uint8_t* tree_bucket_hash = new uint8_t[SHA256_DIGEST_LENGTH];

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
			int l = 2*bucket_pos + 1;
			int r = l + 1;
			auto lit = hash_map.find(l);
			auto rit = hash_map.find(r);

			if(lit == hash_map.end() || rit == hash_map.end()){
				// This shouldn't happen
				assert(0);
			}

			sha256_trib_input_wrapper(
				bucket_hash, SHA256_DIGEST_LENGTH,
				lit->second.data(), SHA256_DIGEST_LENGTH,
				rit->second.data(), SHA256_DIGEST_LENGTH,
				tree_bucket_hash
			);

			// cout << "bucket_hash: " << bucket_pos << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(bucket_hash[j]) << " ";
			// }
			// cout << endl;

			// cout << "lit: " << l << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(lit->second.data()[j]) << " ";
			// }
			// cout << endl;

			// cout << "rit: " << r << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(rit->second.data()[j]) << " ";
			// }
			// cout << endl;

			// cout << "reconstructed: " << bucket_pos << ": ";
			// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
			// 	cout << (int)(tree_bucket_hash[j]) << " ";
			// }
			// cout << endl;
			// cout << endl;
		}

		hash_map[bucket_pos] = vector<uint8_t>(tree_bucket_hash, tree_bucket_hash+SHA256_DIGEST_LENGTH);
		// cout << "hash_map updated at pos" << bucket_pos << endl;
		delete[] tree_bucket_hash;
	}

	delete[] per_block_hash;
	delete[] inner_mt;

	// clear hash map
	auto it = hash_map.upper_bound((1 << (oram_config.cached_levels + 1)) - 2);
	hash_map.erase(it, hash_map.end());
	// cout << "clear hash map key greater than: " << (1 << (oram_config.cached_levels + 1)) - 2 << endl;

	// for(size_t i = (1 << oram_config.cached_levels) - 1; i < (1 << (oram_config.cached_levels + 1)) - 1; i++){
	// 	vector<uint8_t> hash = hash_map[i];

	// 	cout << "root_hash: " << i << ": ";
	// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
	// 		cout << (int)(hash[j]) << " ";
	// 	}
	// 	cout << endl;

	// }

	// assert(0);
}
void RemoteRing::update_mt_reshuffle(const std::vector<int> &position, unsigned char* payload){
	// cout << "update_mt_reshuffle" << endl;
	// assert(0);

	size_t num_buckets = position.size();
	size_t num_blocks = num_buckets * oram_config.bucket_size;

	// hash for each block
	uint8_t* per_block_hash = new uint8_t[SHA256_DIGEST_LENGTH * num_blocks];

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < num_blocks; i++){
		sha256_wrapper(payload + i*ctx_block_size, ctx_block_size, per_block_hash + i*SHA256_DIGEST_LENGTH);

		// cout << "bid: " << i << ": ";
		// for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
		// 	cout << (int)((per_block_hash + i*SHA256_DIGEST_LENGTH)[j]) << " ";
		// }
		// cout << endl;
	}



	//client should send MAC for each blocks, server shouldnt be able to compute it right?
	io->send_data(per_block_hash, num_blocks*SHA256_DIGEST_LENGTH);




	// assert(0);

	// Merkle Trees for each bucket
	size_t inner_mt_size = num_buckets * per_bucket_hashes * SHA256_DIGEST_LENGTH;
	uint8_t* inner_mt = new uint8_t[inner_mt_size];
	memset(inner_mt, 0, inner_mt_size);

	// copy leaf hashes
	size_t first_leaf_off = (per_bucket_hashes - 1) / 2;
	for(size_t i = 0; i < num_buckets; i++){
		uint8_t* src = per_block_hash + i * oram_config.bucket_size * SHA256_DIGEST_LENGTH;
		uint8_t* dst = inner_mt + (i * per_bucket_hashes +  first_leaf_off) * SHA256_DIGEST_LENGTH;
		memcpy(dst, src, oram_config.bucket_size * SHA256_DIGEST_LENGTH);
	}

	// reconstruct inner mt
	for(size_t i = 0 ; i < num_buckets; i++){
		for(size_t j = first_leaf_off; j >= 1; j--){
			size_t id = j-1;
			size_t lid = 2*j-1;
			size_t rid = 2*j;

			assert(lid <=  per_bucket_hashes - 1);

			uint8_t* dst = inner_mt + (i * per_bucket_hashes + id)* SHA256_DIGEST_LENGTH;
			uint8_t* lsrc = inner_mt + (i * per_bucket_hashes + lid)* SHA256_DIGEST_LENGTH;
			uint8_t* rsrc = inner_mt + (i * per_bucket_hashes + rid)* SHA256_DIGEST_LENGTH;

			sha256_twin_input_wrapper(
				lsrc, SHA256_DIGEST_LENGTH,
				rsrc, SHA256_DIGEST_LENGTH,
				dst
			);

		}
	}

	for(size_t i = 0; i < num_buckets; i++){
		int bucket_pos = position[i];

		uint8_t* bucket_hash = inner_mt + i * per_bucket_hashes * SHA256_DIGEST_LENGTH;
		reshuffle_inner_hash_map[bucket_pos] = vector<uint8_t>(bucket_hash, bucket_hash+SHA256_DIGEST_LENGTH);

		int pos_iter = bucket_pos;
		// loop ancestors
		while(pos_iter >= (1 << oram_config.cached_levels) - 1){

			if(pos_iter >= capacity / 2){
				hash_map[pos_iter] = reshuffle_inner_hash_map[pos_iter];
				// cout << "hash_map updated at pos" << pos_iter << endl;
			} else{
				int l = 2*pos_iter + 1;
				int r = l + 1;
				auto lit = hash_map.find(l);
				auto rit = hash_map.find(r);
				auto it = reshuffle_inner_hash_map.find(pos_iter);

				if(lit == hash_map.end() || rit == hash_map.end() || it == reshuffle_inner_hash_map.end()){
					// This shouldn't happen
					assert(0);
				}

				uint8_t* tree_bucket_hash = new uint8_t[SHA256_DIGEST_LENGTH];

				sha256_trib_input_wrapper(
					it->second.data(), SHA256_DIGEST_LENGTH,
					lit->second.data(), SHA256_DIGEST_LENGTH,
					rit->second.data(), SHA256_DIGEST_LENGTH,
					tree_bucket_hash
				);

				hash_map[pos_iter] = vector<uint8_t>(tree_bucket_hash, tree_bucket_hash+SHA256_DIGEST_LENGTH);
				// cout << "hash_map updated at pos" << pos_iter << endl;

				delete[] tree_bucket_hash;

			}
			pos_iter = (pos_iter - 1) / 2;
		}
	}

	// clear hash map
	auto it = hash_map.upper_bound((1 << (oram_config.cached_levels + 1)) - 2);
	hash_map.erase(it, hash_map.end());
	reshuffle_inner_hash_map.clear();
	// cout << "clear hash map key greater than: " << (1 << (oram_config.cached_levels + 1)) - 2 << endl;

	// for(size_t i = (1 << oram_config.cached_levels) - 1; i < (1 << (oram_config.cached_levels + 1)) - 1; i++){
	// 	vector<uint8_t> hash = hash_map[i];

	// 	cout << "root_hash: " << i << ": ";
	// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
	// 		cout << (int)(hash[j]) << " ";
	// 	}
	// 	cout << endl;

	// }

	// assert(0);
}
