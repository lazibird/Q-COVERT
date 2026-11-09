//
//
//

#include "OramLeak.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cmath>
#include <cassert>
#include <chrono>
#include <algorithm>
#include <random>

inline double interval(std::chrono::_V2::system_clock::time_point start){
    auto end = std::chrono::high_resolution_clock::now();
    auto interval = (end - start)/1e+9;
    return interval.count();
}

OramLeak::OramLeak(RemoteLeak* storage, RandForOramInterface* rand_gen, RingOramConfig config, int num_levels, int cached_levels, bool batch, bool lazy) {
    this->storage = storage;
    this->rand_gen = rand_gen;
    this->block_size = config.block_size;
    this->real_bucket_size = config.real_bucket_size;
    this->dummy_size = config.dummy_size;
    this->bucket_size = config.real_bucket_size + config.dummy_size;
    this->evict_rate = config.evict_rate;
    this->num_blocks = config.num_blocks;

    this->batching = batch;
    this->lazy_eveiction = lazy;

    // this->num_levels = ceil(log10(2 * num_blocks / evict_rate) / log10(2)) - 1;
    this->num_levels = num_levels;
    this->num_buckets = pow(2, num_levels)-1;

    this->G = 1;
    
    if (this->num_buckets*this->bucket_size < this->num_blocks) //deal with precision loss
    {
        throw new runtime_error("Not enough space for the acutal number of blocks.");
    }
    this->num_leaves = pow(2, num_levels-1);
    this->rand_gen->setBound(num_leaves);
    // cout << "Set capacity" << endl; 
    // this->storage->setCapacity(num_buckets);
    // cout << "Set capacity done" << endl; 
    this->position_map = vector<int>(num_blocks);
    this->metadata = vector<OramMetaData>(num_buckets);

    // this->stash = vector<Block>();
    this->mstash = map<int, Block>();
    this->mmstash = map<std::pair<int, int>, Block*, PairComparator>();
    
    for (int i = 0; i < this->num_blocks; i++){
        position_map[i] = rand_gen->getRandomLeafWithBound(num_leaves);
    }

    cache_top = cached_levels > 0;
    num_cached_levels = cached_levels;
    num_cached_bucket = 0;

    if(cache_top){
        num_cached_bucket = pow(2, num_cached_levels)-1;
    }

    cnt_early_reshuffle = 0;
    cnt_delayed_leaves = 0;
}

void OramLeak::init_cache_top(){
    if (!cache_top) return;

    // following code has an assumption that usually tree top is empty at the beginning
    vector<int> positions;
    vector<int> offsets;
    vector<bool> valids;
    vector<int> num_non_dummies;
    int total_non_dummies = 0;
    for(int pos = 0; pos < num_cached_bucket; pos++){
        int non_dummies = 0;
        for(int i = 0; i < bucket_size; i++){
            if(metadata[pos].block_ids[i] != -1){
                // no need to check valids
                positions.push_back(pos);
                offsets.push_back(i);
                valids.push_back(true);
                non_dummies++;
            }
        }
        total_non_dummies += non_dummies;
        num_non_dummies.push_back(non_dummies);
        assert(non_dummies <= real_bucket_size);
    }

    if(total_non_dummies > 0) {
        assert(0);
    } else{
        return;
    }

    //retrieve
    std::vector<Block*> blocks;
    storage->ReadBlockBatch(positions, offsets, blocks);

    for(auto b : blocks){
        // cached_oram.insert({b->index, b});
        mmstash[make_pair(position_map[b->index], b->index)] = b;
    }

    cout << mmstash.size() << " blocks cached locally." << endl;

    // int cnt = 0;
    // for(int pos = 0; pos < num_cached_bucket; pos++){
    //     int non_dummies = num_non_dummies[pos];
    //     for(int i = 0; i < non_dummies; i++){
    //         cached_oram[pos*real_bucket_size + i] = blocks[cnt];
    //         cnt++;
    //     }
    //     for(int i = non_dummies; i < real_bucket_size; i++){
    //         cached_oram[pos*real_bucket_size + i] = new Block(block_size);
    //     }
    // }
}

int OramLeak::ReverseBits(int g, int bits_length) {
    /*
    INPUT: Integers g and bits_length.
    OUTPUT: Integer reverse of length bits_length consisting of reversed bits of g.
    To be used to traverse the leaves and run eviction in reverse lexicographical order.
    */
    int g_mod = g%num_leaves;
    int reverse = 0;
    while(g_mod) {
        reverse <<= 1;
        reverse |= g_mod & 1;
        g_mod >>= 1;
        bits_length--;
    }
    
    reverse <<= bits_length;
    return reverse;
}

// void OramLeak::early_reshuffle(vector<int> buckets){
    
//     vector<int> positions;
//     vector<int> offsets;
//     vector<bool> valids;

//     for(int i = 0; i < buckets.size(); i++){
//         // Fetch exactly Z blocks from each buckets
//         int pos = buckets[i];
//         int z = 0;
//         for(int j = 0; j < bucket_size; j++){
//             if(metadata[pos].block_ids[j] != -1 && metadata[pos].valids[j] == 1){
//                 // valid non-empty block
//                 positions.push_back(pos);
//                 offsets.push_back(j);
//                 valids.push_back(true);
//                 z++;
//             }
//         }

//         for(int j = 0; j < bucket_size; j++){
//             if(z >= real_bucket_size){
//                 break;
//             }
//             if(metadata[pos].block_ids[j] == -1 && metadata[pos].valids[j] == 1){
//                 // valid empty block
//                 positions.push_back(pos);
//                 offsets.push_back(j); 
//                 valids.push_back(true);
//                 z++;
//             }
//         }

//         if(z < real_bucket_size){
//             metadata[pos].print_stat();
//         }

//         // cout << "pos: " << pos << endl;
//         // cout << "z: " << z << endl;

//         assert(z == real_bucket_size);
//     }

//     std::vector<Block*> blocks;
//     storage->ReadBlockBatchAsBlockRing(positions, offsets, blocks, valids, true);

//     std::random_device rd;  
//     std::mt19937 g(rd());
//     std::map<int, vector<Block*>> reshuffled_buckets;
//     for(int i = 0; i < buckets.size(); i++){
//         int pos = buckets[i];
//         std::vector<Block*> tmp_bkt(
//             blocks.begin() + i * real_bucket_size, 
//             blocks.begin() + (i + 1) * real_bucket_size
//         ); 

//         for(int i = 0; i < dummy_size; i++){
//             Block* dummy = new Block(block_size);
//             tmp_bkt.push_back(dummy); //dummy block
//         }
//         assert(tmp_bkt.size() == bucket_size);

//         // shuffle
//         std::shuffle(tmp_bkt.begin(), tmp_bkt.end(), g);

//         // update metadata
//         for(int j = 0; j < bucket_size; j++){
//             metadata[pos].block_ids[j] = tmp_bkt[j]->index;
//         }
//         metadata[pos].count = 0;
//         metadata[pos].valids.set();

//         reshuffled_buckets[pos] = tmp_bkt;
//     }

//     // write back to server
//     storage->WriteBucketBatchMapAsBlockRing(reshuffled_buckets, bucket_size, true);

// }

std::vector<int*> OramLeak::access(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata, int pad_l){
    if(batching){
        return batch_multi_access_swap_ro(ops, blockIndices, newdata, pad_l);
    } else{
        vector<int*> ret;
        for(int i = 0; i < ops.size(); i++){
            vector<int*> tmp = batch_multi_access_swap_ro(
                {ops[i]},
                {blockIndices[i]},
                {newdata[i]},
                1
            );
            ret.push_back(tmp[0]);
        }
        for(int i = ops.size(); i < pad_l; i++){
            batch_multi_access_swap_ro(
                {},
                {},
                {},
                1
            );
        }
        return ret;
    }
}


/*
    Workflow here:
    1. Find all the leaves
    2. Check the meta data to see if there's any bucket needs reshuffle
    3. Early reshuffle them
    4. Fetch the path
*/


std::vector<int*> OramLeak::batch_multi_access_swap_ro(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata , int pad_l){

    // cout << "Entering bmasr" << endl;
    std::vector<int*> ret;


    vector<int> positions; // bucket offsets
    vector<int> offsets; // interested block offset within each bucket


    // check for real access
    for(int block_id : blockIndices){
        int oldLeaf = position_map[block_id];
        // traverse the path
        for (int l = num_levels - 1; l >= num_cached_levels; l--) {
            int pos = P(oldLeaf, l);
            bool real = false;
            int offset = metadata[pos].GetBlockOffsetLeak(block_id, real);
            if(real){
                positions.push_back(pos);
                offsets.push_back(offset);
                continue;
            }

        }
    }

    std::vector<Block*> blocks;
    storage->ReadBlockBatch(positions, offsets, blocks);

    for(auto b : blocks){
        int *data = new int[block_size];
        for(int i = 0; i < block_size; i++){
            data[i] = b->data[i];
        }
        ret.push_back(data);
        delete b;
    }

    return ret;
}

void OramLeak::evict_and_write_back(){

}

std::pair<int, int> OramLeak::Q(int leaf, int level){
    int l = num_levels - 1 - level;
    // cout << "Q l: " << l << endl;
    int left = leaf >> l;
    // cout << "Q left: " << left << endl;
    left = left << l;
    // cout << "Q left: " << left << endl;
    int right = left + (1 << l);
    // cout << "Q right: " << right << endl;
    return std::make_pair(left, right);
}

int OramLeak::P(int leaf, int level) {
    /*
    * This function should be deterministic. 
    * INPUT: leaf in range 0 to num_leaves - 1, level in range 0 to num_levels - 1. 
    * OUTPUT: Returns the location in the storage of the bucket which is at the input level and leaf.
    */
    return (1<<level) - 1 + (leaf >> (this->num_levels - level - 1));
}


/*
The below functions are to access various parameters, as described by their names.
INPUT: No input
OUTPUT: Value of internal variables given in the name.
*/

int* OramLeak::getPositionMap() {
    return this->position_map.data();
}

void OramLeak::loadPositionMap(std::vector<int>& pmap) {
    this->position_map = std::vector<int>();
    for(int i = 0; i < pmap.size(); i++){
        this->position_map.push_back(pmap[i]);
    }
    // for(int i =0; i < this->num_blocks; i++){
    //     this->position_map[i] = pmap[i];
    // }
}

void OramLeak::loadPositionMapFromFile(const char* fname) {
    FILE* f = fopen(fname, "r");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }
    int s;
    fread(&s, 1, sizeof(int), f);
    this->position_map = std::vector<int>(s);
    fread(this->position_map.data(), 1, s*sizeof(int), f);
    // for(int i = 0; i < pmap.size(); i++){
    //     this->position_map.push_back(pmap[i]);
    // }
}

void OramLeak::loadMetaDataFromFile(const char* fname) {
    FILE* f = fopen(fname, "r");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }
    int s;
    fread(&s, 1, sizeof(int), f);
    vector<int> block_ids = std::vector<int>(s);
    fread(block_ids.data(), 1, s*sizeof(int), f);
    assert(s == num_buckets*bucket_size);
    for(int i = 0; i < num_buckets; i++){
        metadata[i].count = 0;
        metadata[i].valids.set();
        // metadata[i].block_ids = vector<int>(
        //     block_ids.begin() + i * bucket_size,
        //     block_ids.begin() + (i+1) * bucket_size
        // );

        metadata[i].block_ids = span<int>(
            block_ids.begin() + i * bucket_size,
            bucket_size
        );



        // cout << "metadata for "<< i << " : ";
        // for(auto id : metadata[i].block_ids){
        //     cout << id << " ";
        // }
        // cout << endl;

        // cout << "metadata for "<< i << " : ";
        // for(auto id : metadata[i].block_ids){
        //     cout << id << " ";
        // }
        // cout << endl;
    }

}

vector<Block> OramLeak::getStash() {
    vector<Block> tmp;
    for (const auto& pair : this->mmstash) {
        tmp.push_back(Block(*(pair.second))); // Store the value in the vector
    }
    return tmp;
}
    
int OramLeak::getStashSize() {
    return (this->mmstash).size();
}
    
int OramLeak::getNumLeaves() {
    return this->num_leaves;

}

int OramLeak::getNumLevels() {
    return this->num_levels;

}

int OramLeak::getNumBlocks() {
    return this->num_blocks;

}

int OramLeak::getNumBuckets() {
    return this->num_buckets;

}

