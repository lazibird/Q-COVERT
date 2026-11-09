//
//
//

#ifndef PORAM_OramRing_H
#define PORAM_OramRing_H
#include "OramInterface.h"
#include "position_map.h"
#include "RandForOramInterface.h"
// #include "UntrustedStorageInterface.h"
// #include "RemoteServerStorage.h"
#include "RemoteRing.h"
#include <cmath>
#include <map>
#include <set>
#include <bitset>
#include <cassert>
#include <span>

class OramMetaData {
public:
    uint8_t count;
    bitset<128> valids;
    // vector<int> block_ids; // positive for
    
    //  vector<int> block_ids;
    
    span<int> block_ids;    //turned from vector to span
    
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
    bool account_metadata = false;
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

    vector<int> bids;

    bool batching;
    bool lazy_eveiction;
    int num_reshuffles = 0;

    IPositionMap* posmap = nullptr; //array (LocalPositionMap or RecursivePositionMap)
    bool owns_posmap = false;
    vector<OramMetaData> metadata;

    // vector<Block> stash;
    map<int, Block> mstash;

    map<std::pair<int, int>, Block*, PairComparator> mmstash;

    OramRing(RemoteRing* storage,
            RandForOramInterface* rand_gen, RingOramConfig config, int num_levels, int cached_levels, bool batch = true, bool lazy = true, bool account_metadata = false);

    void evict_and_write_back();

    void init_cache_top();

    std::vector<int*> access(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l);

    std::vector<int*> batch_multi_access_swap_ro(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l);

    template<typename T>
    std::vector<T*> batch_multi_access_swap_ro_templated(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<T*> newdata , int pad_l){

        // A single XOR read can touch each bucket at most `dummy_size` times
        // (that is how many dummy blocks a reshuffled bucket can serve).
        // Wider batches are split into chunks, each with its own early
        // reshuffle followed by one XOR read, so no bucket is ever accessed
        // more than `dummy_size` times between reshuffles.
        if (pad_l > dummy_size) {
            std::vector<T*> ret;
            ret.reserve(blockIndices.size());
            int real = (int)blockIndices.size();
            int processed = 0;
            while (processed < pad_l) {
                int chunk = std::min(dummy_size, pad_l - processed);
                std::vector<Operation> sub_ops;
                std::vector<int> sub_blocks;
                std::vector<T*> sub_newdata;
                for (int i = processed; i < processed + chunk; i++) {
                    if (i < real) {
                        sub_ops.push_back(ops[i]);
                        sub_blocks.push_back(blockIndices[i]);
                        if (i < (int)newdata.size())
                            sub_newdata.push_back(newdata[i]);
                    }
                }
                std::vector<T*> part = this->batch_multi_access_swap_ro_templated<T>(
                    sub_ops, sub_blocks, sub_newdata, chunk);
                ret.insert(ret.end(), part.begin(), part.end());
                processed += chunk;
            }
            return ret;
        }

        ((RemoteRing*)storage)->semantic_bma_invocations++;

        // cout << "Entering bmasr" << endl;
        int len = ops.size();
        std::vector<T*> ret;
        std::vector<int> leaves;
        {
            vector<int> positions;
            vector<int> block_ids; // only for reshuflle
            bool reshuffle = false;
            set<int> reshuffle_buckets;

            this->posmap->read_batch(blockIndices, leaves);
            std::map<int, int> old_leaf_of;
            for(size_t bi = 0; bi < blockIndices.size(); bi++){
                int block_id = blockIndices[bi];
                int oldLeaf = leaves[bi];
                old_leaf_of[block_id] = oldLeaf;
                for (int l = num_levels - 1; l >= num_cached_levels; l--) {
                    int pos = P(oldLeaf, l);
                    metadata[pos].count++;
                    if(metadata[pos].count >= dummy_size){
                        cnt_early_reshuffle++;
                        reshuffle = true;
                        reshuffle_buckets.insert(pos);
                    }
                    block_ids.push_back(block_id);
                    positions.push_back(pos);
                }
            }

            for(int i = leaves.size(); i < pad_l; i++){
                int newLeaf = rand_gen->getRandomLeafWithBound(num_leaves);

                for (int l = num_levels - 1; l >= num_cached_levels; l--) {
                    int pos = P(newLeaf, l);
                    metadata[pos].count++;
                    if(metadata[pos].count >= dummy_size){
                        cnt_early_reshuffle++;
                        reshuffle = true;
                        reshuffle_buckets.insert(pos);
                    }
                    block_ids.push_back(-1);
                    positions.push_back(pos);
                }
            }

            // Reshuffle before computing any offsets so every offset is
            // generated against the final (post-reshuffle) bucket layout.
            if(reshuffle){
                early_reshuffle(vector<int>(reshuffle_buckets.begin(), reshuffle_buckets.end()));
            }

            vector<int> offsets;
            vector<bool> valids;
            for(int i = 0; i < positions.size(); i++){
                int pos = positions[i];
                bool real = false;
                int offset = metadata[pos].GetBlockOffset(block_ids[i], real);
                offsets.push_back(offset);
                if(block_ids[i] != -1){
                    valids.push_back(real);
                } else{
                    valids.push_back(false);
                }
                if(reshuffle && reshuffle_buckets.find(pos) != reshuffle_buckets.end()){
                    metadata[pos].count++;
                    // Chunks are capped at dummy_size, so this can equal but
                    // never exceed dummy_size.
                    if(metadata[pos].count > dummy_size){
                        assert(0);
                    }
                }
            }

            // revisit valids incase of cache miss
            // base on the assumption that all ctx of dummies are the same
            // Fix this later...
            int interval  = num_levels - num_cached_levels;
            assert(valids.size() == interval * pad_l);
            for(int i = 0; i < pad_l; i++){
                bool found = false;
                for (int j = 0;j < interval; j++) {
                    found = found || valids[i*interval + j];
                }
                if(!found){
                    valids[i*interval] = true;
                }
            }

            cnt_delayed_leaves += pad_l;

            // Batch read
            std::vector<Block*> blocks;

            ((RemoteRing*)storage)->ReadBlockBatchAsBlockRingXor(positions, offsets, blocks, pad_l, valids);

            //saving to stash
            for(Block* b : blocks){
                int idx = b->index;
                if (idx != -1) {
                    auto leaf_it = old_leaf_of.find(idx);
                    int leaf = (leaf_it != old_leaf_of.end()) ? leaf_it->second : this->posmap->read(idx);
                    mmstash[make_pair(leaf, idx)] = b;
                } else{
                    // free dummy blocks
                    // avoid memory leaks
                    delete b;
                }
            }
        }

        // Read request data from stash
        std::vector<int> new_leaves(len);
        for(int i = 0; i < len; i++){
            int blockIndex = blockIndices[i];
            int oldLeaf = leaves[i];
            int newLeaf = rand_gen->getRandomLeafWithBound(num_leaves);
            new_leaves[i] = newLeaf;
            Operation op = ops[i];
            T *data = new T[block_size * sizeof(int) / sizeof(T)];

            auto it = mmstash.find(make_pair(oldLeaf, blockIndex));
            if(it != mmstash.end()){
                // Only support READ for now
                assert(op == Operation::READ);
                memcpy(data, it->second->data.data(), block_size * sizeof(int));

                ret.push_back(data);

                Block* b_tmp = it->second;
                mmstash.erase(it);

                // Assign this block to a new position
                mmstash[make_pair(newLeaf, blockIndex)] = b_tmp;
            } else{
                // This shouldn't happen because we just fetched and stored in stash
                cout << "missing block id: " << blockIndex << endl;
                assert(0);
            }
        }
        this->posmap->write_batch(blockIndices, new_leaves);

        return ret;
    }

    int globalG();
    int ReverseBits(int G, int bits_length);

    void early_reshuffle(vector<int> buckets);

    std::pair<int, int> Q(int leaf, int level);
    int P(int leaf, int level);
    int* getPositionMap();
    void set_position_map(IPositionMap* pm);
    IPositionMap* get_position_map_provider() { return this->posmap; }
    void update_stash_block(int blockIndex, const std::vector<int>& payload);
    void flush_stash() { evict_and_write_back(); }
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
