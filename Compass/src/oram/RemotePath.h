#include "Bucket.h"
#include "net_io_channel.h"
#include "OramInterface.h"
#include <cmath>
#include <sstream>
#include <map>

class RemotePath {

public: 

RemotePath(NetIO* io, OramConfig oram_config, bool is_server, bool in_memory, bool integrity);


// client
void ReadBucketBatchAsBlock(const std::vector<int>& positions, std::vector<Block*>& blocks);
void WriteBucketBatchMapAsBlock(const std::map<int, vector<Block*>>& bucket_to_write);
void close_server();


// server
void load_server_state(const char* fname);
void run_server();
void run_server_memory();


// hash
void sync_root();
bool verify_and_insert(int pos, uint8_t* hash);


private: 

    // IO
    NetIO* io;

    // config
    bool integrity;
    bool is_server;
    bool in_memory;


    size_t capacity;
    size_t bucket_size;
    size_t ptx_block_size;
    size_t ctx_block_size;

    OramConfig oram_config;

    std::vector<uint8_t> root;
    std::map<int, std::vector<uint8_t>> hash_map;

    std::vector<SBucket*> buckets;
};