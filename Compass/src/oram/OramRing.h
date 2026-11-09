//
//
//

#ifndef PORAM_OramRing_H
#define PORAM_OramRing_H
#include "OramInterface.h"
#include "RandForOramInterface.h"
// #include "UntrustedStorageInterface.h"
// #include "RemoteServerStorage.h"
#include "RemoteRing.h"
#include <cstdint>
#include <cmath>
#include <map>
#include <set>
#include <bitset>
#include <functional>

class OramMetaData {
public:
    uint8_t count;
    bitset<128> valids;
    vector<int> block_ids; // positive for

    // OramMetaData();

    int GetBlockOffset(int block_id, bool& real); 
    int GetBlockOffsetLeak(int block_id, bool& real); 
    void print_stat(); 
    // int ShuffleAndReset();
};

class OramRing : public OramInterface {
public:
    RemoteRing* storage;
    RandForOramInterface* rand_gen;
    int G;

    int block_size;
    int real_bucket_size;
    int dummy_size;
    int bucket_size;
    int evict_rate;

    int num_levels;
    int num_leaves;
    int num_blocks;
    int num_buckets;

    int cnt_delayed_leaves;

    bool cache_top;
    int num_cached_levels;
    int num_cached_bucket;
    map<int, Block*> cached_oram;
    int cnt_early_reshuffle;
    int cnt_q;

    /** Per-query ORAM statistics (reset by reset_query_access_stats(); snapshot after each search + gcache->reset()). */
    uint64_t stat_bmasr_invocations;
    uint64_t stat_real_block_indices;
    uint64_t stat_pad_slots;
    /** Ciphertext blocks read from server (sum of read batch sizes in Ring ORAM paths). */
    uint64_t stat_server_cipher_block_reads;

    /** `evict_and_write_back()` entries (including no-op when num_paths==0). */
    uint64_t stat_evict_invocations;
    /** Evictions that actually ran (num_paths > 0). */
    uint64_t stat_evict_effective;
    /** Early reshuffle batches that actually ran. */
    uint64_t stat_early_reshuffle_invocations;
    /** Wall time spent inside `evict_and_write_back` (seconds). */
    double stat_evict_time_sec;
    /** Wall time spent inside `batch_multi_access_swap_ro` (seconds, per call summed). */
    double stat_bmasr_time_sec;

    void reset_query_access_stats();

    using PositionMapBatchHook = std::function<void(
            const std::vector<int>& block_indices,
            const std::vector<int>& old_leaves,
            const std::vector<int>& new_leaves)>;
    PositionMapBatchHook position_map_batch_hook;
    void set_position_map_batch_hook(PositionMapBatchHook hook);

    bool batching;
    bool lazy_eveiction;

    vector<int> position_map; //array
    vector<OramMetaData> metadata;

    // vector<Block> stash;
    map<int, Block> mstash;

    map<std::pair<int, int>, Block*, PairComparator> mmstash;

    OramRing(RemoteRing* storage,
            RandForOramInterface* rand_gen, RingOramConfig config, int num_levels, int cached_levels, bool batch = true, bool lazy = true);

    void evict_and_write_back();

    void init_cache_top();

    std::vector<int*> access(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l);

    std::vector<int*> batch_multi_access_swap_ro(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l);

    int globalG();
    int ReverseBits(int G, int bits_length);

    void early_reshuffle(vector<int> buckets);

    std::pair<int, int> Q(int leaf, int level);
    int P(int leaf, int level);
    int* getPositionMap();
    void loadPositionMap(std::vector<int>& pmap);
    void loadPositionMapFromFile(const char* fname);
    void loadMetaDataFromFile(const char* fname);
    void loadMetaDataFromBlockIds(const std::vector<int>& block_ids);
    vector<Block> getStash();
    int getStashSize();
    int getNumLeaves();
    int getNumLevels();
    int getNumBlocks();
    int getNumBuckets();

};


#endif 
