//
//
//

#ifndef PORAM_BUCKET_H
#define PORAM_BUCKET_H


#include "Block.h"
#include <vector>
#include <stdexcept>
#include <cstdint>

class Bucket {

public:
    Bucket();
    Bucket(Bucket *other);
    Block getBlockByIndex(int index);
    void addBlock(Block new_blk);
    bool tryAddBlock(Block new_blk);
    void padDummy(int block_size);
    int getSize();
    bool removeBlock(Block rm_blk);
    bool replaceBlock(int index, Block& new_blk);
    vector<Block> getBlocks() const;
    static void setMaxSize(int maximumSize);
    static void resetState();
    static int getMaxSize();
    void printBlocks();
    bool operator==(const Bucket& other) const;

    void addRRBlock(Block block);
    Block getRRBlock();
    void printRRBlock();

private:
    static bool is_init; //should be initially false
    static int max_size; //should be initially -1
    vector<Block> blocks;

    Block rr_block;
};

// Bucket used on server side
class SBucket {
public:
    SBucket();
    SBucket(bool integrity);

    static void setCipherSize(int maximumSize);
    static void resetState();
    static int getCipherSize();
    static int getSBucketSize();

    // void to_io(NetIO* io);
    // void from_io(NetIO* io);

    void data_to_ptr(unsigned char* ptr);
    void data_xor_to_ptr(unsigned char* ptr);
    void data_from_ptr(unsigned char* ptr);
    void data_xor_from_ptr(unsigned char* ptr);

    void hash_to_ptr(unsigned char* ptr);
    void hash_from_ptr(unsigned char* ptr);

    ~SBucket();

    unsigned char* data;
    uint8_t* hash; // sha256


private:
    static bool sbucket_is_init; //should be initially false
    static int cipher_size; //should be initially -1
};

#endif //PORAM_BUCKET_H
