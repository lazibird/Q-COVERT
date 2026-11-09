//
//
//

#ifndef PORAM_ORAMREADPATHEVICTION_H
#define PORAM_ORAMREADPATHEVICTION_H
#include "OramInterface.h"
#include "RandForOramInterface.h"
// #include "UntrustedStorageInterface.h"
// #include "RemoteServerStorage.h"
#include "RemotePath.h"
#include <cmath>
#include <map>
#include <set>

class OramReadPathEviction : public OramInterface {
public:
    RemotePath* storage;
    RandForOramInterface* rand_gen;

    int block_size;
    int bucket_size;
    int num_levels;
    int num_leaves;
    int num_blocks;
    int num_buckets;

    double t_read;
    double t_write;

    // for multiple oram in on remote server
    int storage_offset;

    vector<int> position_map; //array
    // vector<Block> stash;
    map<int, Block> mstash;

    map<std::pair<int, int>, Block*, PairComparator> mmstash;

    set<int> leaves_cache;
    set<int> position_cache;

    OramReadPathEviction(RemotePath* storage,
            RandForOramInterface* rand_gen, int block_size, int bucket_size, int num_blocks);
    
    int* access(Operation op, int blockIndex, int newdata[]);
    int* batch_access(Operation op, int block_id, int newdata[]);
    std::vector<int*> batch_multi_access(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata);
    std::vector<int*> batch_multi_access_swap(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata);

    void evict_and_write_back();

    std::vector<int*> batch_multi_access_swap_ro(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l);

    int* access_local( Operation op, int blockIndex, int newdata[], std::map<int, Bucket>& local_storage);


    std::pair<int, int> Q(int leaf, int level);
    int P(int leaf, int level);
    int* getPositionMap();
    void loadPositionMap(std::vector<int>& pmap);
    void loadPositionMapFromFile(const char* fname);
    vector<Block> getStash();
    int getStashSize();
    int getNumLeaves();
    int getNumLevels();
    int getNumBlocks();
    int getNumBuckets();

};


#endif 
