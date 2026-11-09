#ifndef PORAM_REMOTERING_H
#define PORAM_REMOTERING_H

#include "Bucket.h"
#include "net_io_channel.h"
#include "OramInterface.h"
#include <cmath>
#include <sstream>
#include <map>
#include <chrono>

// #include <liburing.h>

#include "diskann/include/percentile_stats.h"

class RemoteRing {

public: 

    RemoteRing(NetIO* io, RingOramConfig oram_config, bool is_server, bool in_memory, bool integrity, int disk_type, string disk_path);

    // server 
    void load_server_state(const char* fname);
    void run_server(int io_max);
    void run_server_memory();
    void run_server_disk();

    void run_server_disk_iouring(int io_max);

    void load_from_ground(RemoteRing ground_server);

    //client
    void ReadBlockBatchAsBlockRing(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, std::vector<bool> &valids, bool isReshuffle, bool isSetup);
    // isSetup = "true" when oram init_top_cache() calls it, we avoid doing integrity checks
	// isSetup = "false" when reshuffle/eviction

    void ReadBlockBatchAsBlockRingXor(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, size_t num_real_blocks, std::vector<bool>& valids);

    void WriteBucketBatchMapAsBlockRing(const std::map<int, vector<Block*>>& bucket_to_write, int bucket_size, bool isReshuffle);

    void close_server();
    void clear_data();


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
    void recv_and_verify_hash(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* dexor_payload, std::vector<bool> &valid);
    void recv_and_verify_hash_reshuffle(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);
    void recv_and_verify_hash_bucket(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);

    void update_mt(const std::vector<int> &position, unsigned char* payload);
    void update_mt_reshuffle(const std::vector<int> &position, unsigned char* payload);
    bool verify_and_insert(int pos, uint8_t* hash);

    int start_rounds = 0;
    long start_comm = 0;

    // Semantic ORAM RTT accounting (Compass model). One batch multi-access
    // counts as one data RTT; an early reshuffle and an effective eviction
    // each count as two (read + write). Metadata logically stored on the
    // server adds one extra RTT per semantic operation, not extra bytes.
    long long semantic_bma_invocations = 0;
    long long semantic_early_reshuffle_invocations = 0;
    long long semantic_effective_evict_invocations = 0;

    int rounds_for_oram_access = 0;
    int rounds_for_reshuffles = 0;
    int rounds_for_evictions = 0;

    long comm_for_oram_access = 0;
    long comm_for_reshuffles = 0;
    long comm_for_evictions = 0;

    long server_comm_for_oram_access = 0;
    long server_comm_for_reshuffles = 0;
    long server_comm_for_evictions = 0;



    int mal_rounds_for_oram_access = 0;
    int mal_rounds_for_reshuffles = 0;
    int mal_rounds_for_evictions = 0;

    long mal_comm_for_oram_access = 0;
    long mal_comm_for_reshuffles = 0;
    long mal_comm_for_evictions = 0;

    long mal_server_comm_for_oram_access = 0;
    long mal_server_comm_for_reshuffles = 0;
    long mal_server_comm_for_evictions = 0;

    diskann::QueryStats* current_query_stats;
    std::chrono::duration<double> online_server_side_time = std::chrono::duration<double>(0);

private:

    // IO
    NetIO* io;

    // config
    bool integrity;
    bool is_server;
    bool in_memory;
    int disk_type;

    size_t per_bucket_tree_height;
    size_t per_bucket_hashes;
    std::map<int, std::vector<uint8_t>> hash_map;               // roots of uncached subtrees in primary merkle tree
    std::map<int, std::vector<uint8_t>> reshuffle_inner_hash_map;
    std::vector<uint8_t> roots;

    size_t capacity;
    size_t bucket_size;
    size_t ptx_block_size;
    size_t ctx_block_size;

    bool enable_tree_hash;

    RingOramConfig oram_config;

    // server only
    //fill these in load_from_ground
    unsigned char* data;
    uint8_t* per_bucket_hash;
    uint8_t* tree_hash;


    //disk
    int disk_fd;
    string disk_bucket_path ="";


    //Case 1: mmap
    unsigned char* disk_ptr;

    //Case 2: iouring
    unsigned char* server_cache;

    off_t start_server_cache;
    off_t end_server_cache;    


};

#endif