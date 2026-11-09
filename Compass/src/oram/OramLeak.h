//
//
//

#ifndef PORAM_ORAMLEAK_H
#define PORAM_ORAMLEAK_H
#include "OramInterface.h"
#include "RandForOramInterface.h"
// #include "UntrustedStorageInterface.h"
// #include "RemoteServerStorage.h"
#include "RemoteLeak.h"
#include "OramRing.h"
#include <cmath>
#include <map>
#include <set>
#include <bitset>

class OramLeak : public OramInterface {
public:
    RemoteLeak* storage;
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

    bool batching;
    bool lazy_eveiction;

    vector<int> position_map; //array
    vector<OramMetaData> metadata;

    // vector<Block> stash;
    map<int, Block> mstash;

    map<std::pair<int, int>, Block*, PairComparator> mmstash;

    OramLeak(RemoteLeak* storage,
            RandForOramInterface* rand_gen, RingOramConfig config, int num_levels, int cached_levels, bool batch = true, bool lazy = true);

    void evict_and_write_back();

    void init_cache_top();

    std::vector<int*> access(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l);

    std::vector<int*> batch_multi_access_swap_ro(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l);

    int globalG();
    int ReverseBits(int G, int bits_length);

    // void early_reshuffle(vector<int> buckets);

    std::pair<int, int> Q(int leaf, int level);
    int P(int leaf, int level);
    int* getPositionMap();
    void loadPositionMap(std::vector<int>& pmap);
    void loadPositionMapFromFile(const char* fname);
    void loadMetaDataFromFile(const char* fname);
    vector<Block> getStash();
    int getStashSize();
    int getNumLeaves();
    int getNumLevels();
    int getNumBlocks();
    int getNumBuckets();

};


#endif 
