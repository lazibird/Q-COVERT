//
//
//

#ifndef PORAM_REMOTESERVERSTORAGE_H
#define PORAM_REMOTESERVERSTORAGE_H

// STL
#include <cmath>
#include <sstream>
#include <map>

#include "OramInterface.h"
#include "RandForOramInterface.h"
#include "UntrustedStorageInterface.h"
#include "net_io_channel.h"


class RemoteServerStorage : public UntrustedStorageInterface {
public:
    
    bool is_initialized;
    bool is_capacity_set;
    //Bucket* buckets;

    int block_size;
    std::vector<SBucket*> buckets;
    NetIO* io;
    bool isServer;
    bool sel;
    int num_levels;
    std::map<int, std::vector<uint8_t>> hash_map;
    std::map<int, std::vector<uint8_t>> reshuffle_inner_hash_map;
    char* buckets_fname;

    RemoteServerStorage(int block_size, NetIO* io, bool isServer, int num_levels, bool integrity, RingOramConfig oram_config);

    void insecure_direct_load();

    void setCapacity(int totalNumOfBuckets, bool integrity);
    
    Bucket ReadBucket(int position);
    void WriteBucket(int position, const Bucket& bucket_to_write);

    void ReadBucketBatch(const std::vector<int>& positions, std::vector<Bucket>& bkts);
    void ReadBucketBatchAsBlock(const std::vector<int>& positions, std::vector<Block*>& blocks);
    void ReadBlockBatchAsBlockRing(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, std::vector<bool> &valids, bool isReshuffle);
    
    void ReadBlockBatchAsBlockRingXor(const std::vector<int>& positions, const std::vector<int>& offsets, std::vector<Block*>& blocks, size_t num_real_blocks, std::vector<bool>& valids);

    // void ReadBucketBatchAsBlockRing(const std::vector<int>& positions, std::vector<Block*>& blocks, int bucket_size);
    void WriteBucketBatch(const std::vector<int>& positions, const std::vector<Bucket>& bucket_to_write);
    void WriteBucketBatchMap(const std::map<int, Bucket>& bucket_to_write);
    void WriteBucketBatchMapAsBlock(const std::map<int, vector<Block*>>& bucket_to_write);
    void WriteBucketBatchMapAsBlockRing(const std::map<int, vector<Block*>>& bucket_to_write, int bucket_size, bool isReshuffle);

    // Insecure Family
    bool integrity;
    std::vector<uint8_t> roots;
    void insecureLoad(vector<Bucket>& input_bkts);
    void insecureLoadPtr(int* bkts);
    void sync_root();

    // Integrity related
    RingOramConfig oram_config;
    size_t per_bucket_tree_height;
	size_t per_bucket_hashes;
    uint8_t* per_bucket_hash;
    // void sync_hash(RingOramConfig config);
    void sync_hash_roots();

    void recv_and_verify_hash(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);

    void recv_and_verify_hash_reshuffle(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);

    void recv_and_verify_hash_bucket(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);

    void recv_and_verify_hash_2(const std::vector<int> &position, const std::vector<int> &offset, unsigned char* payload, std::vector<bool> &valid);

    void update_mt(const std::vector<int> &position, unsigned char* payload);
    void update_mt_reshuffle(const std::vector<int> &position, unsigned char* payload);

    int P(int leaf, int level);
    bool verify_and_insert(int pos, uint8_t* hash);

    void RunServer();
    void RunServer_disk();

    void InitServer();
    void CloseServer();

    void save(const char* fname);
    void load(const char* fname);

    void load_disk(const char* fname);
    
private: 
    int capacity;

};


#endif //PORAM_REMOTESERVERSTORAGE_H