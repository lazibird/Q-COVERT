//
//
//

#include "OramReadPathEviction.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cmath>
#include <cassert>
#include <chrono>

inline double interval(std::chrono::_V2::system_clock::time_point start){
    auto end = std::chrono::high_resolution_clock::now();
    auto interval = (end - start)/1e+9;
    return interval.count();
}



OramReadPathEviction::OramReadPathEviction(RemotePath* storage, RandForOramInterface* rand_gen,
                                           int block_size, int bucket_size, int num_blocks) {
    this->storage = storage;
    this->rand_gen = rand_gen;
    this->block_size = block_size;
    this->bucket_size = bucket_size;
    this->num_blocks = num_blocks;
    this->num_levels = ceil(log10(num_blocks) / log10(2))-1;
    this->num_buckets = pow(2, num_levels)-1;
    this->storage_offset = storage_offset;
    
    if (this->num_buckets*this->bucket_size < this->num_blocks) //deal with precision loss
    {
        throw new runtime_error("Not enough space for the acutal number of blocks.");
    }
    this->num_leaves = pow(2, num_levels-1);
    Bucket::resetState();
    Bucket::setMaxSize(bucket_size);
    this->rand_gen->setBound(num_leaves);
    // cout << "Set capacity" << endl; 
    // this->storage->setCapacity(num_buckets);
    // cout << "Set capacity done" << endl; 
    this->position_map = vector<int>(num_blocks);
    // this->stash = vector<Block>();
    this->mstash = map<int, Block>();
    this->mmstash = map<std::pair<int, int>, Block*, PairComparator>();
    
    for (int i = 0; i < this->num_blocks; i++){
        position_map[i] = rand_gen->getRandomLeafWithBound(num_leaves);
        // if(i < 100){
        //     std:: cout << position_map[i] << " " << std::endl;
        // } else{
        //     assert(0);
        // }
        
    }

    // ((RemoteServerStorage*)storage)->InitServer();

    // for(int i = 0; i < num_buckets; i++){

    //     Bucket init_bkt = Bucket();
    //     for(int j = 0; j < bucket_size; j++){
    //         init_bkt.addBlock(Block());
    //     }
    //     storage->WriteBucket(i, Bucket(init_bkt));
    //     // assert(0);
    // }

}

std::vector<int*> OramReadPathEviction::batch_multi_access_swap(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata){
    assert(0);
    return {};

    // int len = ops.size();

    // // remote position -> bucket
    // std::map<int, Bucket> local_storage;

    // // path, index -> block
    // // std::vector<std::map<int, Block>> local_stash(len);

    // // index -> path
    // // std::map<int, int> reverse_path_mapping;
    // std::vector<int*> ret;

    // // std::cout << "Batch Reading" << std::endl;

    // {
    //     // Batch Read
    //     std::vector<Bucket> bkts;
    //     std::set<int> position_set;
    //     for(int blockIndex : blockIndices){
    //         int oldLeaf = position_map[blockIndex];
    //         for (int i = 0; i < num_levels; i++) {
    //             position_set.insert(P(oldLeaf, i));
    //         }
    //     }

    //     std::vector<int> positions(position_set.begin(), position_set.end());
    //     ((RemoteServerStorage*)storage)->ReadBucketBatch(positions, bkts);

    //     for(int i = 0; i < positions.size(); i++){
    //         local_storage[positions[i]] = bkts[i];
    //     }
    // }

    // std::vector<int> oldLeaves;

    // // std::cout << "Start parsing" << std::endl;

    // for(int i = 0; i < len; i++){
    //     int blockIndex = blockIndices[i];
    //     int oldLeaf = position_map[blockIndex];
    //     position_map[blockIndex] = rand_gen->getRandomLeafWithBound(num_leaves);
    //     oldLeaves.push_back(oldLeaf);
    // }

    // for(int i = 0; i < len; i++){
    //     int blockIndex = blockIndices[i];
    //     Operation op = ops[i];
    //     int* newdata_ = newdata[i];
    //     int *data = new int[block_size];
    //     int oldLeaf = oldLeaves[i];

    //     Block* targetBlock;

    //     for (int j = 0; j < num_levels; j++) {
    //         for (Block b: local_storage[P(oldLeaf, j)].getBlocks()) {
    //             if (b.index != -1) {
    //                 // stash.push_back(Block(b));
    //                 auto it = mmstash.find(make_pair(position_map[b.index], b.index));
    //                 if(it == mmstash.end()){

    //                     mmstash[make_pair(position_map[b.index], b.index)] = Block(b);

    //                     if(b.index == blockIndex){
    //                         // targetBlock = &mstash[b.index];
    //                         targetBlock = &mmstash[make_pair(position_map[b.index], b.index)];
    //                         // std::cout << "find 1: " << targetBlock->block_size << endl;
    //                     }

    //                 } else{
    //                     if(b.index == blockIndex){
    //                         targetBlock = &(it->second);
    //                     }
    //                 }
    //             }
    //         }
    //     }

    //     // std::cout << "Detect Node 1" << std::endl;

    //     if (op == Operation::WRITE) {
    //         if (targetBlock == NULL) {
    //             // std::cout << "Detect Node 2" << std::endl;
    //             Block newBlock = Block(block_size, position_map[blockIndex], blockIndex, newdata_);
    //             // mstash[newBlock.index] = newBlock;
    //             mmstash[make_pair(position_map[newBlock.index], newBlock.index)] = newBlock;
    //         } else {
    //             // std::cout << "Detect Node 3" << std::endl;
    //             for(int i =0; i < block_size; i++){
    //                 targetBlock->data[i] = newdata_[i];
    //             }
    //             // mstash[targetBlock.index] = targetBlock;
    //         }
    //     } 
    //     else {
    //         if(targetBlock == NULL){
    //             // std::cout << "Detect Node 4" << std::endl;
    //             data = NULL;
    //             // std::cout << "Block not found: " << blockIndex << " oldLeaf: "<< oldLeaf << std::endl;
    //             assert(0);
    //         }
    //         else {
    //             // std::cout << "Detect Node 5: " << targetBlock->block_size << std::endl;
    //             for(int i = 0; i < block_size; i++){
    //                 data[i] = targetBlock->data[i];
    //             }
    //         }
    //     }
    //     // std::cout << "Detect Node done" << std::endl;
    //     ret.push_back(data);
    // }

    // // std::cout << "Start eviction" << std::endl;

    // // Evictions
    // // std::set<int> visited;
    // std::map<int, Bucket> evicted_storage;

    // for (int l = num_levels - 1; l >= 0; l--) {
    //     for(int i = 0; i < len; i++){
    //         int oldLeaf = oldLeaves[i];
    //         int Pxl = P(oldLeaf, l);
    //         if(evicted_storage.find(Pxl) == evicted_storage.end()){
    //             Bucket bkt = Bucket();
    //             int counter = 0;

    //             std::pair<int, int> leaf_range = Q(oldLeaf, l);

    //             // Inefficient Version
    //             // for (auto it = mmstash.begin(), next_it = it; it != mmstash.end(); it = next_it) {
    //             //     ++next_it;
    //             //     if (counter >= bucket_size) {
    //             //         break;
    //             //     }
    //             //     if (Pxl == P(position_map[it->first.second], l)) {
    //             //         bkt.addBlock(Block(it->second));
    //             //         mmstash.erase(it);
    //             //         counter++;
    //             //     }
    //             // }

    //             for (auto it = mmstash.upper_bound(make_pair(leaf_range.first, 0)), next_it = it; it != mmstash.end(); it = next_it) {
    //                 ++next_it;

    //                 if (counter >= bucket_size) {
    //                     break;
    //                 }

    //                 if(it->first.first >= leaf_range.second){
    //                     break;
    //                 }

    //                 // bkt.addBlock(it->second);
    //                 // mmstash.erase(it);
    //                 // counter++;

    //                 if (Pxl == P(position_map[it->first.second], l)) {
    //                     bkt.addBlock(it->second);
    //                     mmstash.erase(it);
    //                     counter++;
                        
    //                 } else{
    //                     cout << "oldleaf: " << oldLeaf << endl;
    //                     cout << "l: " << l << endl;
    //                     cout << "leaf_range left: " << leaf_range.first << endl;
    //                     cout << "leaf_range right: " << leaf_range.second << endl;
    //                     cout << "current leaf id: " << it->first.first << endl;
    //                     cout << "current index: " << it->first.second << endl;
    //                     cout << "actual leaf id: " << position_map[it->first.second] << endl;
    //                     assert(0);
    //                 }
    //             }



    //             while(counter < bucket_size){
    //                 bkt.addBlock(Block(block_size)); //dummy block
    //                 counter++;
    //             }
    //             // evicted_storage[Pxl] = bucket;
    //             evicted_storage[Pxl] = bkt;
    //         }
            
    //     }
    // }

    // // std::cout << "Done eviction" << std::endl;

    // {
    //     // Batch Write
    //     // for (auto& pair : evicted_storage) {
    //     //     pair.second.padDummy(block_size);
    //     // }
    //     ((RemoteServerStorage*)storage)->WriteBucketBatchMap(evicted_storage);
    // }

    std::cout << "stash size: " << mmstash.size() << std::endl;

    // return ret;
}

std::vector<int*> OramReadPathEviction::batch_multi_access_swap_ro(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata , int pad_l){

    int len = ops.size();
    std::vector<int*> ret;

    {
        vector<int> leaves;
        // Generate fixed size leaves
        for(int blockIndex : blockIndices){
            int oldLeaf = position_map[blockIndex];
            // cout << "Need index: " << blockIndex << " oldleaf: " << oldLeaf << endl;

            if(leaves_cache.find(oldLeaf) == leaves_cache.end()){
                leaves.push_back(oldLeaf);
                leaves_cache.insert(oldLeaf);
            } else{
                // Generate a random one
                while(1){
                    int newLeaf = rand_gen->getRandomLeafWithBound(num_leaves);
                    if(leaves_cache.find(newLeaf) == leaves_cache.end()){
                        leaves.push_back(newLeaf);
                        leaves_cache.insert(newLeaf);
                        break;
                    }
                }
            }
        }

        for(int i = leaves.size(); i < pad_l; i++){
            while(1){
                int newLeaf = rand_gen->getRandomLeafWithBound(num_leaves);
                if(leaves_cache.find(newLeaf) == leaves_cache.end()){
                    leaves.push_back(newLeaf);
                    leaves_cache.insert(newLeaf);
                    break;
                }
            }
        }

        assert(leaves.size() == pad_l);
        // cout << "leaves.size(): " << pad_l << endl;
        
        // Generate unread positions
        // Sequence is important here
        vector<int> positions;
        for (int l = num_levels - 1; l >= 0; l--) {
            for(int leaf : leaves){
                int pos = P(leaf, l);
                if(position_cache.find(pos) == position_cache.end()){
                    // This position was not read before
                    positions.push_back(pos);
                    position_cache.insert(pos);
                }
            }
        }

        // Batch read
        std::vector<Block*> blocks;
        // auto t_0 = std::chrono::high_resolution_clock::now();
        storage->ReadBucketBatchAsBlock(positions, blocks);
        // t_read += interval(t_0);

        // Insert each block into stash
        for(Block* b : blocks){
            int idx = b->index;
            if (idx != -1) {
                // cout << "Got: " << b.index  << endl;
                auto it = mmstash.find(make_pair(position_map[idx], idx));
                mmstash[make_pair(position_map[idx], idx)] = b;
            } else{
                // free dummy blocks
                // avoid memory leaks
                delete b;
            }
        }
    }
    
    // Read request data from stash
    for(int i = 0; i < len; i++){
        int blockIndex = blockIndices[i];
        int oldLeaf = position_map[blockIndex];
        int newLeaf = rand_gen->getRandomLeafWithBound(num_leaves);
        position_map[blockIndex] = newLeaf;
        Operation op = ops[i];
        int *data = new int[block_size];

        // cout << "Read index: " << blockIndex << " oldleaf: " << oldLeaf << " newLeaf: " << newLeaf << endl;

        auto it = mmstash.find(make_pair(oldLeaf, blockIndex));
        if(it != mmstash.end()){

            // Only support READ for now
            assert(op == Operation::READ);
            for(int i = 0; i < block_size; i++){
                data[i] = it->second->data[i];
            }
            ret.push_back(data);

            Block* b_tmp = it->second;
            mmstash.erase(it);

            // Assign this block to a new position
            mmstash[make_pair(newLeaf, blockIndex)] = b_tmp;
        } else{
            // This shouldn't happen
            assert(0);
        }
    } 

    return ret;
}

void OramReadPathEviction::evict_and_write_back(){
    // Evictions
    std::map<int, vector<Block*>> evicted_storage;
    std::vector<int> leaves;
    for (int l = num_levels - 1; l >= 0; l--) {
        for(int oldLeaf :  leaves_cache){
            leaves.push_back(oldLeaf);
            int Pxl = P(oldLeaf, l);
            if(evicted_storage.find(Pxl) == evicted_storage.end()){
                vector<Block*> bkt;
                int counter = 0;

                std::pair<int, int> leaf_range = Q(oldLeaf, l);
                for (auto it = mmstash.upper_bound(make_pair(leaf_range.first, 0)), next_it = it; it != mmstash.end(); it = next_it) {
                    ++next_it;

                    if (counter >= bucket_size) {
                        break;
                    }

                    if(it->first.first >= leaf_range.second){
                        break;
                    }

                    if (Pxl == P(position_map[it->first.second], l)) {
                        bkt.push_back(it->second);
                        // cout << "Evict: " << it->first.second << " newLeaf: " << l << endl;
                        mmstash.erase(it);
                        counter++;
                        
                    } else{
                        cout << "oldleaf: " << oldLeaf << endl;
                        cout << "l: " << l << endl;
                        cout << "leaf_range left: " << leaf_range.first << endl;
                        cout << "leaf_range right: " << leaf_range.second << endl;
                        cout << "current leaf id: " << it->first.first << endl;
                        cout << "current index: " << it->first.second << endl;
                        cout << "actual leaf id: " << position_map[it->first.second] << endl;
                        assert(0);
                    }
                }



                while(counter < bucket_size){
                    Block* dummy = new Block(block_size);
                    bkt.push_back(dummy); //dummy block
                    counter++;
                }
                evicted_storage[Pxl] = bkt;
            }
            
        }
    }

    // cout << endl;
    // assert(0);

    // Batch Write
    // auto t_0 = std::chrono::high_resolution_clock::now();
    storage->WriteBucketBatchMapAsBlock(evicted_storage);
    // t_write += interval(t_0);

    // cout << "ReadBucketTime: " << t_read << endl;
    // cout << "WriteBucketTime: " << t_write << endl;

    t_read = 0;
    t_write = 0;

    // cout << "mmstash size: " << mmstash.size() << endl;

    // if(mmstash.size() != 0) {
    //     cout << "### mmstash size: " << mmstash.size() << endl;
    // }
    
    // Clear State
    leaves_cache.clear();
    position_cache.clear();
}


std::vector<int*> OramReadPathEviction::batch_multi_access(std::vector<Operation> ops, std::vector<int> blockIndices, std::vector<int*> newdata){
    assert(0);
    return {};

    // // a mapping from remote position to bucket
    // std::map<int, Bucket> local_storage;
    // int len = ops.size();
    // std::vector<int*> ret;

    // for(int i = 0; i < len; i++){
    //     ret.push_back(batch_access(ops[i], blockIndices[i], newdata[i]));
    // }

    // return ret;

    // {
    //     // Batch Read
    //     std::vector<Bucket> bkts;
    //     std::set<int> position_set;
    //     for(int blockIndex : blockIndices){
    //         int oldLeaf = position_map[blockIndex];
    //         for (int i = 0; i < num_levels; i++) {
    //             position_set.insert(P(oldLeaf, i));
    //         }
    //     }

    //     std::vector<int> positions(position_set.begin(), position_set.end());
    //     std::vector<int> leaves;
    //     ((RemoteServerStorage*)storage)->ReadBucketBatch(positions, bkts);

    //     for(int i = 0; i < positions.size(); i++){
    //         local_storage[positions[i]] = bkts[i];
    //     }
    // }

    // for(int i = 0; i < len; i++){
    //     int *data = new int[block_size];
    //     data = access_local(ops[i], blockIndices[i], newdata[i], local_storage);
    //     ret.push_back(data);
    // }

    // {
    //     // Batch Write
    //     std::vector<Bucket> bkts;
    //     std::vector<int> positions;
        
    //     for (const auto& pair : local_storage) {
    //         positions.push_back(pair.first);
    //         bkts.push_back(pair.second);
    //     }
    //     std::vector<int> leaves;
    //     ((RemoteServerStorage*)storage)->WriteBucketBatch(positions, bkts);
    // }

    // return ret;
}

int* OramReadPathEviction::access_local(Operation op, int blockIndex, int newdata[], std::map<int, Bucket>& local_storage){
    assert(0);
    return nullptr;
    // int *data = new int[block_size];
    // int oldLeaf = position_map[blockIndex];
    // position_map[blockIndex] = rand_gen->getRandomLeafWithBound(num_leaves);
    // for (int i = 0; i < num_levels; i++) {
    //     vector<Block> blocks = local_storage[OramReadPathEviction::P(oldLeaf, i)].getBlocks();
    //     for (Block b: blocks) {
    //         if (b.index != -1) {
    //             // stash.push_back(Block(b));
    //             // mstash[b.index] = Block(b);
    //             if(b.block_size == -1){
    //                 assert(0);
    //             }
    //             mmstash[make_pair(position_map[b.index], b.index)] = Block(b);
    //         }
    //     }

    // }

    // Block *targetBlock = NULL;
    // // int targetPos = 0;

    // for(auto& pair : mmstash){
    //     if(pair.first.second == blockIndex){
    //         targetBlock = &(pair.second);
    //         // targetPos = i;
    //         break;
    //     }
    // }


    // if (op == Operation::WRITE) {
    //     if (targetBlock == NULL) {
    //         Block newBlock = Block(block_size, position_map[blockIndex], blockIndex, newdata);
    //         // mstash[newBlock.index] = newBlock;
    //         mmstash[make_pair(position_map[newBlock.index], newBlock.index)] = newBlock;
    //     } else {
    //         for(int i =0; i < block_size; i++){
    //             targetBlock->data[i] = newdata[i];
    //         }
    //         // stash[targetPos] = Block(*targetBlock);
    //     }
    // } 
    // else {
    //     if(targetBlock == NULL){
    //         data = NULL;
    //         std::cout << "Block not found: " << blockIndex << " oldLeaf: "<< oldLeaf << std::endl;
    //         assert(0);
    //     }
    //     else {
    //         for(int i = 0; i < block_size; i++){
    //             data[i] = targetBlock->data[i];
    //         }
    //     }
    // }


    // // Eviction steps: write to the same path that was read from.
    // for (int l = num_levels - 1; l >= 0; l--) {

    //     vector<int> bid_evicted = vector<int>();
    //     Bucket bucket = Bucket();
    //     int Pxl = P(oldLeaf, l);
    //     int counter = 0;

    //     for (auto it = mmstash.begin(), next_it = it; it != mmstash.end(); it = next_it) {
    //         ++next_it;
    //         if (counter >= bucket_size) {
    //             break;
    //         }
    //         if (Pxl == P(position_map[it->first.second], l)) {
    //             bucket.addBlock(Block(it->second));

    //             mmstash.erase(it);
    //             counter++;
    //         }
    //     }

    //     while(counter < bucket_size)
    //     {
    //         bucket.addBlock(Block(block_size)); //dummy block
    //         counter++;
    //     }
    //     local_storage[Pxl] = bucket;

    // }

    // return data;
}

int* OramReadPathEviction::batch_access(Operation op, int blockIndex, int *newdata) {
    assert(0);
    return nullptr;

//     int *data = new int[block_size];
//     int oldLeaf = position_map[blockIndex];
//     position_map[blockIndex] = rand_gen->getRandomLeafWithBound(num_leaves);
//     // std::cout << "Accessing leaf: "<< oldLeaf << endl; 

//     {
//         // Batch Read
//         std::vector<Bucket> bkts;
//         std::vector<int> positions(num_levels);
//         for (int i = 0; i < num_levels; i++) {
//             positions[i] = P(oldLeaf, i);
//         }
//         std::vector<int> leaves;
//         ((RemoteServerStorage*)storage)->ReadBucketBatch(positions, bkts);

//         for(auto& bkt : bkts){
//             vector<Block> blocks = bkt.getBlocks();
//             for (Block& b: blocks) {
//                 if (b.index != -1) {
//                     // mstash[b.index] = Block(b);
//                     if(b.block_size == -1){
//                         assert(0);
//                     }
//                     mmstash[make_pair(position_map[b.index], b.index)] = Block(b);
//                 }
//             }
//         }
//     }

//     Block *targetBlock = NULL;
//  // int targetPos = 0;
//     for(auto& pair : mmstash){
//         if(pair.first.second == blockIndex){
//             targetBlock = &(pair.second);
//             // targetPos = i;
//             break;
//         }
//     }

//     if (op == Operation::WRITE) {
//         if (targetBlock == NULL) {
//             Block newBlock = Block(block_size ,position_map[blockIndex], blockIndex, newdata);
//             // mstash[newBlock.index] = newBlock;
//             mmstash[make_pair(position_map[newBlock.index], newBlock.index)] = newBlock;
//         } else {
//             for(int i =0; i < block_size; i++){
//                 targetBlock->data[i] = newdata[i];
//             }
//             // stash[targetPos] = Block(*targetBlock);
//         }
//     } 
//     else {
//         if(targetBlock == NULL){
//             data = NULL;
//             cout << "Block not found" << endl;
//             assert(0);
//         }
//         else {
//             for(int i = 0; i < block_size; i++){
//                 data[i] = targetBlock->data[i];
//             }
//         }
//     }

//     std::vector<int> Pxls;
//     std::vector<Bucket> bkts;

//     // Eviction steps: write to the same path that was read from.
//     for (int l = num_levels - 1; l >= 0; l--) {

//         vector<int> bid_evicted = vector<int>();
//         Bucket bucket = Bucket();
//         int Pxl = P(oldLeaf, l);
//         int counter = 0;

//         for (auto it = mmstash.begin(), next_it = it; it != mmstash.end(); it = next_it) {
//             ++next_it;
//             if (counter >= bucket_size) {
//                 break;
//             }
//             if (Pxl == P(position_map[it->first.second], l)) {
//                 bucket.addBlock(Block(it->second));

//                 mmstash.erase(it);
//                 counter++;
//             }
//         }

//         while(counter < bucket_size)
//         {
//             bucket.addBlock(Block(block_size)); //dummy block
//             counter++;
//         }
//         Pxls.push_back(Pxl);
//         bkts.push_back(bucket);

//     }

//     std::vector<int> leaves;
//     ((RemoteServerStorage*)storage)->WriteBucketBatch(Pxls, bkts);

//     return data;
    
}

int* OramReadPathEviction::access(Operation op, int blockIndex, int *newdata) {
    
    assert(0);
    return nullptr;

    // int *data = new int[block_size];
    // int oldLeaf = position_map[blockIndex];
    // position_map[blockIndex] = rand_gen->getRandomLeafWithBound(num_leaves);
    // for (int i = 0; i < num_levels; i++) {
    //     vector<Block> blocks = storage->ReadBucket(OramReadPathEviction::P(oldLeaf, i)).getBlocks();
    //     for (Block b: blocks) {
    //         if (b.index != -1) {
    //             // mstash[b.index] = Block(b);
    //             if(b.block_size == -1){
    //                 assert(0);
    //             }
    //             mmstash[make_pair(position_map[b.index], b.index)] = Block(b);
    //         }
    //     }

    // }

    // Block *targetBlock = NULL;
    // // int targetPos = 0;

    // for(auto& pair : mmstash){
    //     if(pair.first.second == blockIndex){
    //         targetBlock = &(pair.second);
    //         // targetPos = i;
    //         break;
    //     }
    // }


    // if (op == Operation::WRITE) {
    //     if (targetBlock == NULL) {
    //         Block newBlock = Block(block_size, position_map[blockIndex], blockIndex, newdata);
    //         // mstash[newBlock.index] = newBlock;
    //         mmstash[make_pair(position_map[newBlock.index], newBlock.index)] = newBlock;
    //     } else {
    //         for(int i =0; i < block_size; i++){
    //             targetBlock->data[i] = newdata[i];
    //         }
    //         // stash[targetPos] = Block(*targetBlock);
    //     }
    // } 
    // else {
    //     if(targetBlock == NULL){
    //         data = NULL;
    //     }
    //     else {
    //         for(int i = 0; i < block_size; i++){
    //             data[i] = targetBlock->data[i];
    //         }
    //     }
    // }


    // // Eviction steps: write to the same path that was read from.
    // for (int l = num_levels - 1; l >= 0; l--) {

    //     vector<int> bid_evicted = vector<int>();
    //     Bucket bucket = Bucket();
    //     int Pxl = P(oldLeaf, l);
    //     int counter = 0;

    //     for (auto it = mmstash.begin(), next_it = it; it != mmstash.end(); it = next_it) {
    //         ++next_it;
    //         if (counter >= bucket_size) {
    //             break;
    //         }
    //         if (Pxl == P(position_map[it->first.second], l)) {
    //             bucket.addBlock(Block(it->second));

    //             mmstash.erase(it);
    //             counter++;
    //         }
    //     }

    //     while(counter < bucket_size)
    //     {
    //         bucket.addBlock(Block(block_size)); //dummy block
    //         counter++;
    //     }
    //     storage->WriteBucket(Pxl, bucket);

    // }

    // return data;
    
}

std::pair<int, int> OramReadPathEviction::Q(int leaf, int level){
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

int OramReadPathEviction::P(int leaf, int level) {
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

int* OramReadPathEviction::getPositionMap() {
    return this->position_map.data();
}

void OramReadPathEviction::loadPositionMap(std::vector<int>& pmap) {
    this->position_map = std::vector<int>();
    for(int i = 0; i < pmap.size(); i++){
        this->position_map.push_back(pmap[i]);
    }
    // for(int i =0; i < this->num_blocks; i++){
    //     this->position_map[i] = pmap[i];
    // }
}

void OramReadPathEviction::loadPositionMapFromFile(const char* fname) {
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

vector<Block> OramReadPathEviction::getStash() {
    vector<Block> tmp;
    for (const auto& pair : this->mmstash) {
        tmp.push_back(Block(*(pair.second))); // Store the value in the vector
    }
    return tmp;
}
    
int OramReadPathEviction::getStashSize() {
    return (this->mmstash).size();
}
    
int OramReadPathEviction::getNumLeaves() {
    return this->num_leaves;

}

int OramReadPathEviction::getNumLevels() {
    return this->num_levels;

}

int OramReadPathEviction::getNumBlocks() {
    return this->num_blocks;

}

int OramReadPathEviction::getNumBuckets() {
    return this->num_buckets;

}

