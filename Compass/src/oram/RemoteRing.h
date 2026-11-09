#include "Bucket.h"
#include "net_io_channel.h"
#include "OramInterface.h"
#include <cmath>
#include <sstream>
#include <map>

class RemoteRing {

public: 

    RemoteRing(NetIO* io, RingOramConfig oram_config, bool is_server, bool in_memory, bool integrity);

    // server 
    void load_server_state(const char* fname);
    void load_server_state_from_ciphertexts(const std::vector<unsigned char>& ciphertexts);
    void run_server();
    void run_server_memory();

    //client
    void ReadBlockBatchAsBlockRing(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, std::vector<bool> &valids, bool isReshuffle);
    
    void ReadBlockBatchAsBlockRingXor(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, size_t num_real_blocks, std::vector<bool>& valids);

    void WriteBucketBatchMapAsBlockRing(const std::map<int, vector<Block*>>& bucket_to_write, int bucket_size, bool isReshuffle);

    void close_server();


    // ----------------------------- hash ---------------------------------- //

    void load_server_hash(const char* fname);

    // both
    void sync_roots();

    // server hashes 
    void send_hash(std::vector<int> &position, std::vector<int> &offset);
    void send_hash_bucket(std::vector<int> &position, std::vector<int> &offset);
    void send_hash_reshuffle(std::vector<int> &position, std::vector<int> &offset);
    void update_hash(std::vector<int> &position, uint8_t* payload);
    void update_hash_reshuffle(std::vector<int> &position, uint8_t* payload);

    // client hashes 
    void recv_and_verify_hash(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);
    void recv_and_verify_hash_reshuffle(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);
    void recv_and_verify_hash_bucket(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);

    void update_mt(const std::vector<int> &position, unsigned char* payload);
    void update_mt_reshuffle(const std::vector<int> &position, unsigned char* payload);
    bool verify_and_insert(int pos, uint8_t* hash);

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
