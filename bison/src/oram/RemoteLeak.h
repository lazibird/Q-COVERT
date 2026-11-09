#include "Bucket.h"
#include "net_io_channel.h"
#include "OramInterface.h"
#include <cmath>
#include <sstream>
#include <map>

class RemoteLeak {

public: 

    RemoteLeak(NetIO* io, RingOramConfig oram_config, bool is_server, bool in_memory, bool integrity);

    // server 
    void load_server_state(const char* fname);
    void run_server();
    void run_server_memory();

    //client
    void ReadBlockBatch(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks);
    
    void close_server();

private:

    // IO
    NetIO* io;

    // config
    bool integrity;
    bool is_server;
    bool in_memory;

    size_t per_bucket_tree_height;
    size_t per_bucket_hashes;
    std::map<int, std::vector<uint8_t>> hash_map;
    std::map<int, std::vector<uint8_t>> reshuffle_inner_hash_map;
    std::vector<uint8_t> roots;

    size_t capacity;
    size_t bucket_size;
    size_t ptx_block_size;
    size_t ctx_block_size;

    bool enable_tree_hash;

    RingOramConfig oram_config;

    // server only
    unsigned char* data;
    uint8_t* per_bucket_hash;
    uint8_t* tree_hash;

};