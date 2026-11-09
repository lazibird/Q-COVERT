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
#include <cerrno>
#include <filesystem>
#include <thread>

#include "evp.h"

#define NUM_THREADS 4
#define SHA256_DIGEST_LENGTH 32

namespace double_oram {

namespace {

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

inline size_t ring_plain_block_bytes(size_t ptx_block_size) {
    return (1 + ptx_block_size) * sizeof(int);
}

constexpr uint32_t kRemoteRingStateMagic = 0x32524f51U; // "QOR2"
constexpr uint32_t kRemoteRingStateVersion = 2;

#pragma pack(push, 1)
struct RemoteRingStateHeader {
	uint32_t magic;
	uint32_t version;
	uint64_t capacity;
	uint64_t bucket_size;
	uint64_t ciphertext_block_size;
	uint64_t payload_bytes;
	uint8_t integrity;
	uint8_t reserved[7];
};
#pragma pack(pop)

void read_exact_fd(int fd, void* output, size_t size) {
	auto* cursor = static_cast<unsigned char*>(output);
	while (size != 0) {
		const ssize_t count = read(fd, cursor, size);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) throw std::runtime_error("truncated RemoteRing state file");
		cursor += count;
		size -= static_cast<size_t>(count);
	}
}

void write_exact_fd(int fd, const void* input, size_t size) {
	const auto* cursor = static_cast<const unsigned char*>(input);
	while (size != 0) {
		const ssize_t count = write(fd, cursor, size);
		if (count < 0 && errno == EINTR) continue;
		if (count <= 0) throw std::runtime_error("cannot write RemoteRing state file");
		cursor += count;
		size -= static_cast<size_t>(count);
	}
}

void fsync_parent_directory(const std::filesystem::path& path) {
	const int fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (fd < 0) throw std::runtime_error("cannot open RemoteRing state directory");
	const int result = fsync(fd);
	close(fd);
	if (result != 0) throw std::runtime_error("cannot fsync RemoteRing state directory");
}

} // 匿名命名空间

RemoteRing::RemoteRing(NetIO* io, RingOramConfig oram_config, bool is_server, bool in_memory,
                       bool integrity, const AeadKey* key)
    : io(io), is_server(is_server), in_memory(in_memory), integrity(integrity), oram_config(oram_config), data(nullptr), per_bucket_hash(nullptr), tree_hash(nullptr), local_server(nullptr){

	    // AES-GCM authenticates every ciphertext block. The inherited Merkle-hash
	    // mode is disabled because its unchecked assertion paths are not protocol-safe.
	    if (integrity) {
		throw std::invalid_argument("legacy RemoteRing integrity mode is unsupported; AES-GCM is always enabled");
	    }
	    // currently only support in memory server
    assert(in_memory);

	    ptx_block_size = oram_config.block_size;
#ifdef QCOVERT_REMOTE_RING_SERVER_ONLY
	ctx_block_size = ring_plain_block_bytes(ptx_block_size) + kAeadNonceBytes + kAeadTagBytes;
#else
	ctx_block_size = compute_ctx_len(ring_plain_block_bytes(ptx_block_size));
#endif
    capacity = oram_config.num_buckets;
	bucket_size = oram_config.bucket_size;

	per_bucket_tree_height = ceil(log10(oram_config.bucket_size) / log10(2)) + 1;
	per_bucket_hashes = pow(2, per_bucket_tree_height) - 1;

    if(is_server){
	        data = new unsigned char [capacity * bucket_size * ctx_block_size]();

        cout << "> Remote storage server config: " << endl;
        cout << " -> in memory: " << in_memory << endl;
        cout << " -> integrity: " << integrity << endl;
        cout << " -> # buckets: " << capacity << endl;
        cout << " -> # Z: " << bucket_size << endl;
        cout << " -> block size: " << ctx_block_size << endl;
		cout << " -> total: " << capacity * bucket_size * ctx_block_size << endl;
	} else {
		if (key == nullptr) {
			throw std::invalid_argument("RemoteRing client requires a generation encryption key");
		}
		encryption_key = *key;
		has_encryption_key = true;
	}
}

#ifndef QCOVERT_REMOTE_RING_SERVER_ONLY
RemoteRing::RemoteRing(RemoteRing* local_server, RingOramConfig oram_config, bool integrity,
                       const AeadKey& key)
    : io(nullptr), is_server(false), in_memory(true), integrity(integrity), oram_config(oram_config), data(nullptr), per_bucket_hash(nullptr), tree_hash(nullptr), local_server(local_server){

	    if (integrity) {
		throw std::invalid_argument("legacy RemoteRing integrity mode is unsupported; AES-GCM is always enabled");
	    }
	    ptx_block_size = oram_config.block_size;
    ctx_block_size = compute_ctx_len(ring_plain_block_bytes(ptx_block_size));
    capacity = oram_config.num_buckets;
    bucket_size = oram_config.bucket_size;

    per_bucket_tree_height = ceil(log10(oram_config.bucket_size) / log10(2)) + 1;
	per_bucket_hashes = pow(2, per_bucket_tree_height) - 1;
	encryption_key = key;
	has_encryption_key = true;
}
#endif

RemoteRing::~RemoteRing() {
	delete[] data;
	delete[] per_bucket_hash;
	delete[] tree_hash;
#ifndef QCOVERT_REMOTE_RING_SERVER_ONLY
	OPENSSL_cleanse(encryption_key.data(), encryption_key.size());
#endif
}

void RemoteRing::run_server(){
	    if (!is_server || !in_memory) {
		throw std::logic_error("RemoteRing server is not configured for in-memory serving");
	}
	    if(in_memory){
	        run_server_memory();
	    }
    
}

void RemoteRing::close_server(){
	if(local_server != nullptr){
		return;
	}
	int rt = End;
	io->send_data(&rt, sizeof(int));
}


void RemoteRing::load_server_state(const char* fname){
	if (!is_server || !in_memory || data == nullptr) {
		throw std::logic_error("RemoteRing state can only be loaded by an in-memory server");
	}
	const uint64_t payload_bytes = capacity * bucket_size * ctx_block_size;
	const int fd = open(fname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) throw std::runtime_error(std::string("cannot open RemoteRing state: ") + strerror(errno));
	try {
		struct stat info{};
		if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
			throw std::runtime_error("RemoteRing state is not a regular file");
		}
		RemoteRingStateHeader header{};
		read_exact_fd(fd, &header, sizeof(header));
		if (header.magic != kRemoteRingStateMagic || header.version != kRemoteRingStateVersion ||
			header.capacity != capacity || header.bucket_size != bucket_size ||
			header.ciphertext_block_size != ctx_block_size || header.payload_bytes != payload_bytes ||
			header.integrity != static_cast<uint8_t>(integrity) ||
			static_cast<uint64_t>(info.st_size) != sizeof(header) + payload_bytes) {
			throw std::runtime_error("RemoteRing state does not match the requested authenticated tree format");
		}
		read_exact_fd(fd, data, static_cast<size_t>(payload_bytes));
		close(fd);
	} catch (...) {
		close(fd);
		throw;
	}
}

void RemoteRing::save_server_state(const char* fname) const {
	if (!is_server || !in_memory || data == nullptr) {
		throw std::logic_error("RemoteRing state can only be saved by an in-memory server");
	}
	const std::filesystem::path target(fname);
	const uint64_t payload_bytes = capacity * bucket_size * ctx_block_size;
	const RemoteRingStateHeader header{
		kRemoteRingStateMagic, kRemoteRingStateVersion, capacity, bucket_size,
		ctx_block_size, payload_bytes, static_cast<uint8_t>(integrity), {0,0,0,0,0,0,0}};
	const std::filesystem::path temporary = target.string() + ".tmp." +
		std::to_string(getpid()) + "." +
		std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
	const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0) throw std::runtime_error(std::string("cannot create RemoteRing checkpoint: ") + strerror(errno));
	try {
		write_exact_fd(fd, &header, sizeof(header));
		write_exact_fd(fd, data, static_cast<size_t>(payload_bytes));
		if (fsync(fd) != 0) throw std::runtime_error("cannot fsync RemoteRing checkpoint");
		if (close(fd) != 0) throw std::runtime_error("cannot close RemoteRing checkpoint");
		if (rename(temporary.c_str(), target.c_str()) != 0) {
			throw std::runtime_error(std::string("cannot install RemoteRing checkpoint: ") + strerror(errno));
		}
		fsync_parent_directory(target);
	} catch (...) {
		close(fd);
		unlink(temporary.c_str());
		throw;
	}
}

void RemoteRing::load_server_hash(const char* fname){
	throw std::logic_error("legacy RemoteRing hash-state loading is disabled by authenticated encryption");
	/*
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

	

	cout << "total_read: " << total_read << endl;

	assert(total_read == target_size);

	return;
	*/
}



void RemoteRing::run_server_memory(){
	// cout << "Remote storage server running ..." << endl;
	auto validate_locations = [&](const std::vector<int>& positions,
	                              const std::vector<int>* offsets) {
		if (positions.size() > capacity * bucket_size) {
			throw std::runtime_error("RemoteRing request contains too many locations");
		}
		for (size_t i = 0; i < positions.size(); ++i) {
			if (positions[i] < 0 || static_cast<size_t>(positions[i]) >= capacity ||
				(offsets != nullptr && ((*offsets)[i] < 0 ||
				 static_cast<size_t>((*offsets)[i]) >= bucket_size))) {
				throw std::runtime_error("RemoteRing request location is out of range");
			}
		}
	};
	while(1) {
		int rt;
		io->recv_data(&rt, sizeof(int));
		switch (rt){
				case Read:{
					throw std::runtime_error("legacy RemoteRing Read request is disabled");
				}
				case Write:{
					throw std::runtime_error("legacy RemoteRing Write request is disabled");
			}
			case ReadBatchBlock_R:
			case ReadBatchBlock: {
					size_t num_blocks;
					io->recv_data(&num_blocks, sizeof(size_t));
					if (num_blocks > capacity * bucket_size) {
						throw std::runtime_error("RemoteRing read request is too large");
					}

				// auto t_fetch = std::chrono::high_resolution_clock::now();
				
				std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks);
					io->recv_data(offset.data(), sizeof(int)*num_blocks);
					validate_locations(position, &offset);

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


#ifndef QCOVERT_REMOTE_RING_SERVER_ONLY
					if(integrity){
					if(rt == ReadBatchBlock){
						// for bucket read
						send_hash_bucket(position, offset);
					} else{
						// for reshuffle
							send_hash_reshuffle(position, offset);
							// send_hash(position, offset);
						}
					}
#endif

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
					if (num_real_blocks == 0 || num_blocks == 0 ||
						num_blocks > capacity * bucket_size ||
						num_blocks % num_real_blocks != 0) {
						throw std::runtime_error("invalid RemoteRing XOR request dimensions");
					}
				
				std::vector<int> position(num_blocks);
				std::vector<int> offset(num_blocks);
				io->recv_data(position.data(), sizeof(int)*num_blocks);
					io->recv_data(offset.data(), sizeof(int)*num_blocks);
					validate_locations(position, &offset);

				size_t path_len = num_blocks / num_real_blocks;

				// no need for ivs
				size_t len = num_real_blocks * (ctx_block_size - 16);

				unsigned char* payload = new unsigned char[len];
				unsigned char* ivs = new unsigned char[num_blocks*16];
				std::memset(payload, 0, len); 

				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t block_id = 0; block_id < num_real_blocks; block_id++){
					size_t bucket_offset = block_id * (ctx_block_size - 16);
					for(int i = 0; i < path_len; i++){
						size_t bucket_id = block_id * path_len + i;
						size_t bucket_pos = position[bucket_id]*bucket_size + offset[bucket_id]; 
						// this->buckets[bucket_pos]->data_xor_to_ptr(payload + bucket_offset);

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

				io->send_data(payload, sizeof(unsigned char)*len);
				io->send_data(ivs, sizeof(unsigned char)*num_blocks*16);

#ifndef QCOVERT_REMOTE_RING_SERVER_ONLY
					if(integrity){
						send_hash(position, offset);
					}
#endif

				// cout << "ReadBatchBlockXor send to client done" << endl;

				delete[] payload;

				break;
			}
				case WriteBatchBlock:{
					throw std::runtime_error("legacy RemoteRing WriteBatchBlock request is disabled");
				}
				case ReadBatch:{
					throw std::runtime_error("legacy RemoteRing ReadBatch request is disabled");
			}
			case WriteBatch_R:
			case WriteBatch:{

					size_t num_buckets;
					io->recv_data(&num_buckets, sizeof(size_t));
					if (num_buckets > capacity) {
						throw std::runtime_error("RemoteRing write request is too large");
					}

				std::vector<int> position(num_buckets);
					io->recv_data(position.data(), sizeof(int)*num_buckets);
					validate_locations(position, nullptr);

				size_t len = num_buckets * bucket_size * (ctx_block_size);

				unsigned char* payload = new unsigned char[len];
				// cout << "WriteBatch allocate done" << endl;

				io->recv_data(payload, sizeof(unsigned char)*len);


				#pragma omp parallel for num_threads(NUM_THREADS)
				for(size_t bucket_id = 0; bucket_id < num_buckets; bucket_id++){
					for(size_t block_id = 0; block_id < bucket_size; block_id++){
						size_t block_pos = position[bucket_id]*bucket_size + block_id;
						size_t block_offset = (bucket_id*bucket_size + block_id) * ctx_block_size;
						// this->buckets[block_pos]->data_from_ptr(payload + block_offset);
						unsigned char* tmp_data = data + block_pos*ctx_block_size;
						mempcpy(tmp_data, payload + block_offset, ctx_block_size);
					}
				} 

#ifndef QCOVERT_REMOTE_RING_SERVER_ONLY
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
#endif
				
				delete[] payload;
				break;
			}
			
				case Init:{
					throw std::runtime_error("legacy RemoteRing Init request is disabled");
			}
			case End:{
				cout << "Remote storage server closing ..." << endl;
				return;
			}
			default:{
					throw std::runtime_error("invalid RemoteRing request type");
			}
			
		}
	}
}


#ifndef QCOVERT_REMOTE_RING_SERVER_ONLY

void RemoteRing::ReadBlockBatchAsBlockRing(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, std::vector<bool> &valids, bool isReshuffle){
	if (positions.size() != offsets.size()) {
		throw std::invalid_argument("RemoteRing positions and offsets must have equal length");
	}

	// Here a trick is that for ring oram we consider each bucket with only one block
	// For the sake of simple impelementation

	// cout << "ReadBlockBatchAsBlockRing: 1" << endl;

	if(local_server != nullptr){
		size_t num_blocks = positions.size();
		blocks.resize(num_blocks);
		try {
		for(size_t block_id = 0; block_id < num_blocks; block_id++){
			size_t bucket_pos = positions[block_id] * bucket_size + offsets[block_id];
			unsigned char* ctx_payload = local_server->data + bucket_pos * ctx_block_size;
			std::vector<unsigned char> ptx_payload(ctx_block_size);
			decrypt_wrapper(encryption_key, ctx_payload, ctx_block_size, ptx_payload.data());
			Block* b = new Block(ptx_block_size);
			b->from_ptr(ptx_payload.data());
			blocks[block_id] = b;
		}
		} catch (...) {
			for (Block* block : blocks) delete block;
			blocks.clear();
			throw;
		}
		return;
	}
	
	int rt = isReshuffle ? ReadBatchBlock_R : ReadBatchBlock;
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

	io->recv_data(payload, len);

	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);
	blocks.resize(num_blocks);

	// cout << "ReadBlockBatchAsBlockRing: recv_data" << endl;
	
	try {
	for(size_t block_id = 0; block_id < num_blocks; block_id++){
		// printf("numThreads = %d\n", omp_get_num_threads()); 
		unsigned char* ctx_payload = payload + block_id * ctx_block_size;
		// // for (int i = 0; i < 10; i++) { // Adjust the length as needed
		// // 	// Convert each byte to an unsigned value and print as hex
		// // 	std::cout << std::hex << std::setw(2) << std::setfill('0') 
		// // 			<< static_cast<int>(static_cast<unsigned char>(ctx_payload[i])) << " ";
		// // }
		// std::cout << std::endl;
		std::vector<unsigned char> ptx_payload(ctx_block_size);
		decrypt_wrapper(encryption_key, ctx_payload, ctx_block_size, ptx_payload.data());
		// assert(ptx_len == per_block_size*bucket_size);
		// unsigned char* tmp = ptx_payload;

		Block* b = new Block(ptx_block_size);
		b->from_ptr(ptx_payload.data());
		blocks[block_id] = b;
		
		// bkts.push_back(std::move(init_bkt));
	}
	} catch (...) {
		for (Block* block : blocks) delete block;
		blocks.clear();
		delete[] payload;
		throw;
	}

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

	// cout << "ReadBlockBatchAsBlockRing: recv_data done" << endl;

	delete[] payload;
}

void RemoteRing::ReadBlockBatchAsBlockRingXor(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, size_t num_real_blocks, std::vector<bool>& valids){
	if (positions.size() != offsets.size() || positions.size() != valids.size() ||
		num_real_blocks == 0 || positions.empty() || positions.size() % num_real_blocks != 0) {
		throw std::invalid_argument("invalid RemoteRing XOR read dimensions");
	}

	// cout << "ReadBlockBatchAsBlockRingXor - pos_len:  " << positions.size() << ", offsets_len: " << offsets.size() << ", num_real_blocks: " << num_real_blocks << endl; 

	// cout << "ReadBlockBatchAsBlockRingXor - valids_len:  " << valids.size() << ", num_levels: " << num_levels << endl;

	if(local_server != nullptr){
		size_t num_blocks = positions.size();
		size_t path_len = num_blocks / num_real_blocks;
		blocks.resize(num_real_blocks);

		try {
		for(size_t block_id = 0; block_id < num_real_blocks; block_id++){
			int valid_block_offset = -1;
			for(size_t i = 0; i < path_len; i++){
				size_t block_offset = block_id * path_len + i;
				if(valids[block_offset]){
					if (valid_block_offset != -1) throw std::runtime_error("multiple real blocks selected on one XOR path");
					valid_block_offset = block_offset;
				}
			}
			if (valid_block_offset == -1) throw std::runtime_error("XOR path contains no real block");
			size_t bucket_pos = positions[valid_block_offset] * bucket_size + offsets[valid_block_offset];
			unsigned char* ctx_payload = local_server->data + bucket_pos * ctx_block_size;
			std::vector<unsigned char> ptx_payload(ctx_block_size);
			decrypt_wrapper(encryption_key, ctx_payload, ctx_block_size, ptx_payload.data());
			Block* b = new Block(ptx_block_size);
			b->from_ptr(ptx_payload.data());
			blocks[block_id] = b;
		}
		} catch (...) {
			for (Block* block : blocks) delete block;
			blocks.clear();
			throw;
		}
		return;
	}
	
	int rt = ReadBatchBlockXor;
	io->send_data(&rt, sizeof(int));

	size_t num_blocks = positions.size();
	// assert(num_blocks % num_levels == 0);
	// size_t num_real_blocks = num_blocks / num_levels;
	io->send_data(&num_blocks, sizeof(size_t));
	io->send_data(&num_real_blocks, sizeof(size_t));
	io->send_data(positions.data(), sizeof(int)*num_blocks);
	io->send_data(offsets.data(), sizeof(int)*num_blocks);

	// no need for ivs
	size_t len = num_real_blocks * (ctx_block_size - 16);

	unsigned char* payload = new unsigned char[len];
	unsigned char* ivs = new unsigned char[num_blocks*16];
	unsigned char* dexor_payload = nullptr;
	if(integrity){
		dexor_payload = new unsigned char[num_blocks * ctx_block_size];
	}

	io->recv_data(payload, len);
	io->recv_data(ivs, num_blocks*16);

	size_t per_block_size = (1 + ptx_block_size) * sizeof(int);
	blocks.resize(num_real_blocks);

	// assert(valids.size() == num_levels*num_real_blocks);
	
	try {
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
					if (valid_block_offset != -1) throw std::runtime_error("multiple real blocks selected on one XOR path");
				valid_block_offset = block_offset;
			} else{
				// Undo xor of dummies
				std::vector<unsigned char> dummy_ctx(ctx_block_size);
				std::vector<unsigned char> dummy_ptx(ctx_block_size, 0);
				
				dummy_block.to_ptr(dummy_ptx.data());
					size_t ctx_len = encrypt_wrapper_with_iv(
						encryption_key,
						ivs + block_offset*16,
						dummy_ptx.data(),
						per_block_size,
						dummy_ctx.data()
					);

				if (ctx_len != ctx_block_size - 16) throw std::runtime_error("unexpected AES-GCM XOR body length");

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
						dummy_ctx.data(), 
						ctx_block_size - 16
					);
				}

				for(int j = 0; j < ctx_block_size - 16; j++){
					// de-xor
					ctx_payload[j] = ctx_payload[j] ^ dummy_ctx[j];
				}
			}
		}

		if(integrity){
			// copy the dummy ctx to dexor payload
			memcpy(
				dexor_payload + valid_block_offset*ctx_block_size + 16,
				ctx_payload, 
				ctx_block_size - 16
			);
		}

		if (valid_block_offset == -1) throw std::runtime_error("XOR path contains no real block");

		std::vector<unsigned char> ptx_payload(ctx_block_size);
		// cout << "try decrypt" << endl;
		size_t ptx_len = decrypt_wrapper_with_iv(
			encryption_key,
			ivs + valid_block_offset* 16, 
			ctx_payload, 
			ctx_block_size - 16,
			ptx_payload.data());

		// cout << "try haha" << endl;
		Block* b = new Block(ptx_block_size);
		b->from_ptr(ptx_payload.data());
		blocks[block_id] = b;
	}
	} catch (...) {
		for (Block* block : blocks) delete block;
		blocks.clear();
		delete[] dexor_payload;
		delete[] ivs;
		delete[] payload;
		throw;
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
	delete[] dexor_payload;
}

void RemoteRing::WriteBucketBatchMapAsBlockRing(const std::map<int, vector<Block*>>& bucket_to_write, int bucket_size, bool isReshuffle){
	if(local_server != nullptr){
		size_t per_block_size = (1 + ptx_block_size) * sizeof(int);
		for (const auto& pair : bucket_to_write) {
			int pos = pair.first;
			for(int block_id = 0; block_id < bucket_size; block_id++){
				std::vector<unsigned char> ptx_payload(ctx_block_size, 0);
				pair.second[block_id]->to_ptr(ptx_payload.data());
				unsigned char* ctx_payload = local_server->data + (pos * this->bucket_size + block_id) * ctx_block_size;
				encrypt_wrapper(encryption_key, ptx_payload.data(), per_block_size, ctx_payload);
				delete pair.second[block_id];
			}
		}
		return;
	}

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

	for (int bucket_id = 0; bucket_id < positions.size(); bucket_id++) {
		for(int block_id = 0; block_id < bucket_size; block_id++){
			std::vector<unsigned char> ptx_payload(ctx_block_size, 0);
			bkts[bucket_id][block_id]->to_ptr(ptx_payload.data());
			unsigned char* ctx_payload = payload + (bucket_id * bucket_size + block_id) * ctx_block_size;
			size_t ctx_len = encrypt_wrapper(encryption_key, ptx_payload.data(), per_block_size, ctx_payload);
			if (ctx_len != ctx_block_size) throw std::runtime_error("unexpected AES-GCM block length");
			delete bkts[bucket_id][block_id];
		}
	}

	
	io->send_data(positions.data(), positions.size()*sizeof(int));
	io->send_data(payload, len);


	// TODO: this could be optimized
	// Instead of sending the hashes, the server could just recompute by it self
	if(integrity){
		if(!isReshuffle){
			update_mt(positions, payload);
		} else{
			update_mt_reshuffle(positions, payload);
		}
	}

	delete[] payload;
}


// ----------------------------- hash ---------------------------------- //

void RemoteRing::sync_roots(){
    if(is_server){
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
			bucket_id ++;
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

	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < num_blocks; i++){
		sha256_wrapper(payload + i*ctx_block_size, ctx_block_size, per_block_hash + i*SHA256_DIGEST_LENGTH);

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

void RemoteRing::update_hash_reshuffle(std::vector<int> &position, uint8_t* payload){
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
void RemoteRing::recv_and_verify_hash(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid){

	// cout << "recv_and_verify_hash_2..." << endl;

	size_t num_hashes = position.size() * (per_bucket_tree_height - 1);
	// cout << "recv_and_verify_hash_2333..." << endl;
	uint8_t* inner_mt_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];

	// cout << "reconstruct bucket hash 0" << endl;

	io->recv_data(inner_mt_payload, num_hashes*SHA256_DIGEST_LENGTH);

	// cout << "reconstruct bucket hash 1" << endl;

	uint8_t* outer_mt_payload = new uint8_t[position.size() *SHA256_DIGEST_LENGTH];
	uint8_t* reconstruct_bucket_hash = new uint8_t[position.size() *SHA256_DIGEST_LENGTH];

	io->recv_data(outer_mt_payload, position.size() *SHA256_DIGEST_LENGTH);

	// cout << "reconstruct bucket hash" << endl;
	
	// reconstruct per bucket hash
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < position.size(); i++){
		
		size_t pos = position[i];
		size_t cur_offset = offset[i] + (per_bucket_hashes + 1) / 2 - 1;
		// unsigned char* ctx = payload + valid_cnt*ctx_block_size;
		unsigned char* ctx = payload + i*ctx_block_size;
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

	// overwrite the fetched blocks hashes
	#pragma omp parallel for num_threads(NUM_THREADS)
	for(size_t i = 0; i < position.size(); i++){
		size_t pos = position[i];
		size_t off = offset[i];
		size_t bucket_id = i / oram_config.real_bucket_size;

		unsigned char* ctx = payload + i*ctx_block_size;
		uint8_t* ref = inner_leaves_payload + (bucket_id*oram_config.bucket_size + off) * SHA256_DIGEST_LENGTH;
		sha256_wrapper(ctx, ctx_block_size, ref);
	}

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

	// cout << "recv_and_verify_hash_bucket: reconstruct" << endl;

	// reconstruct bucket hash
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

	// for(size_t i = 0 ; i < num_buckets; i++){
	// 	uint8_t* bucket_hash = inner_mt + (i * per_bucket_hashes)* SHA256_DIGEST_LENGTH;
	// 	cout << "reconstructed hash: " << position[i*oram_config.real_bucket_size] << ": ";
	// 	for(int j = 0; j < SHA256_DIGEST_LENGTH; j++){
	// 		cout << (int)(bucket_hash[j]) << " ";
	// 	}
	// 	cout << endl;
	// }

	// assert(0);

	// cout << "recv_and_verify_hash_bucket: reconstruct outer mt" << endl;

	// reconstruct outer hash
	for(size_t i = 0 ; i < num_buckets; i++){
		int bucket_pos = position[i*oram_config.real_bucket_size];
		int sib = get_sibling3(bucket_pos);

		uint8_t* sib_hash = outer_mt_payload + i*SHA256_DIGEST_LENGTH;

		//  cout << "inserting sib: " << sib << endl;
		if(!verify_and_insert(sib, sib_hash)){
			cout << "Hash verification fails at sib: " << sib << endl;
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

			// assert(0);
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

	// here we just fetch all block's hashes for each bucket
	size_t num_hashes = num_buckets*oram_config.bucket_size;
	uint8_t* inner_leaves_payload = new uint8_t[num_hashes*SHA256_DIGEST_LENGTH];
	io->recv_data(inner_leaves_payload, num_hashes*SHA256_DIGEST_LENGTH);

	// overwrite the fetched blocks hashes
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

	// reconstruct bucket hash
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

	size_t inner_hash_size = inner_hash_pos.size();
	size_t outer_hash_size = outer_hash_pos.size();

	uint8_t* inner_hash_payload = new uint8_t[inner_hash_size*SHA256_DIGEST_LENGTH];
	uint8_t* outer_hash_payload = new uint8_t[outer_hash_size*SHA256_DIGEST_LENGTH];

	io->recv_data(inner_hash_payload, inner_hash_size*SHA256_DIGEST_LENGTH);
	io->recv_data(outer_hash_payload, outer_hash_size*SHA256_DIGEST_LENGTH);

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
		hash_map[pos] = vector<uint8_t>(outer_hash_payload + i*SHA256_DIGEST_LENGTH, outer_hash_payload + (i+1)*SHA256_DIGEST_LENGTH);
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

	delete[] inner_mt;
	delete[] inner_hash_payload;
	delete[] outer_hash_payload;
}

bool RemoteRing::verify_and_insert(int pos, uint8_t* hash){
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


void RemoteRing::update_mt(const std::vector<int> &position, unsigned char* payload){
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

#endif // QCOVERT_REMOTE_RING_SERVER_ONLY

} // double_oram 命名空间
