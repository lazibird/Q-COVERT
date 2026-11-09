//
//
//
#include "Bucket.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cassert>
#include <string.h>

using namespace std;

namespace double_oram {

bool Bucket::is_init = false;
int Bucket::max_size = -1;
bool SBucket::sbucket_is_init = false;
int SBucket::cipher_size = -1;

Bucket::Bucket(){
    if(!is_init){
        throw new runtime_error("Please set bucket size before creating a bucket");
    }
    blocks = vector<Block>();
}

//Copy constructor
Bucket::Bucket(Bucket *other){
    if(other == NULL){
        throw new runtime_error("the other bucket is not malloced.");
    }
    for(int i = 0; i < max_size; i++){
        blocks.push_back(Block(other->blocks[i]));
    }
}

//Get block object with matching index
Block Bucket::getBlockByIndex(int index) {
    Block *copy_block = NULL;
    for(Block b: blocks){
        if(b.index == index){
            copy_block = new Block(b);
            break;
        }
    }
    return *copy_block;
}

void Bucket::addBlock(Block new_blk){
    if(blocks.size() < max_size){
        Block toAdd = Block(new_blk);
        blocks.push_back(toAdd);
    } else{
        assert(0);
    }

}

bool Bucket::tryAddBlock(Block new_blk){
    if(blocks.size() < max_size){
        Block toAdd = Block(new_blk);
        blocks.push_back(toAdd);
        return true;
    } else{
        return false;
    }

}

void Bucket::padDummy(int block_size){
    for(int i = blocks.size(); i < max_size; i++){
        blocks.push_back(Block(block_size));
    }
    assert(blocks.size() == max_size);
}

int Bucket::getSize(){
    return blocks.size();
}

bool Bucket::removeBlock(Block rm_blk){
    bool removed = false;
    for(int i = 0; i < blocks.size(); i++){
        if(blocks[i].index == rm_blk.index){
            blocks.erase(blocks.begin() + i);
            removed = true;
            break;
        }
    }
    return removed;
}

bool Bucket::replaceBlock(int index, Block& new_blk){
    bool replaced = false;
    for(int i = 0; i < blocks.size(); i++){
        if(blocks[i].index == index){
            blocks[i] = Block(new_blk);
            replaced = true;
            break;
        }
    }
    return replaced;
}

// Return a shallow copy.
vector<Block> Bucket::getBlocks() const{
    return this->blocks;
}

void Bucket::setMaxSize(int maximumSize){
    if(is_init == true){
        throw new runtime_error("Max Bucket Size was already set");
    }
    max_size = maximumSize;
    is_init = true;
}

int Bucket::getMaxSize() {
    return max_size;
}

void Bucket::resetState(){
    is_init = false;
}

void Bucket::printBlocks() {
    for (Block b: blocks) {
        b.printBlock();
    }
}

bool Bucket::operator==(const Bucket& other) const{
    vector<Block> b1 = other.getBlocks();
    for(int i = 0; i < blocks.size(); i++){
        if(b1[i].index != blocks[i].index){
            return false;
        }
    }
    return true;
}

SBucket::SBucket(){
    if(!sbucket_is_init){
        throw new runtime_error("Please set bucket size before creating a bucket");
    }
    data = new unsigned char[cipher_size];
    hash = new uint8_t[32]; // sha256
}

SBucket::SBucket(bool integrity){
    if(!sbucket_is_init){
        throw new runtime_error("Please set bucket size before creating a bucket");
    }
    data = new unsigned char[cipher_size];
    hash = nullptr;
    if(integrity){
        hash = new uint8_t[32]; // sha256
    }
}

SBucket::~SBucket(){
    delete[] data;
    if(hash){
        delete[] hash;
    }
}

void SBucket::hash_to_ptr(unsigned char* ptr){
    memcpy(ptr, hash, 32*sizeof(uint8_t));
}

void SBucket::hash_from_ptr(unsigned char* ptr){
    memcpy(hash, ptr, 32*sizeof(uint8_t));
}

void SBucket::data_to_ptr(unsigned char* ptr){
    memcpy(ptr, data, cipher_size);
}

void SBucket::data_xor_to_ptr(unsigned char* ptr){
    for(size_t j = 0; j < cipher_size; j++){
        ptr[j] = data[j] ^ ptr[j];
    }
}

void SBucket::data_xor_from_ptr(unsigned char* ptr){
    for(size_t j = 0; j < cipher_size; j++){
        data[j] = data[j] ^ ptr[j];
    }
}

void SBucket::data_from_ptr(unsigned char* ptr){
    memcpy(data, ptr, cipher_size);
}

// void SBucket::to_io(NetIO* io){
//     io->send_data(data, cipher_size);
//     io->send_data(hash, 32*sizeof(uint8_t));
// }

// void SBucket::from_io(NetIO* io){
//     io->recv_data(data, cipher_size);
//     io->recv_data(hash, 32*sizeof(uint8_t));
// }

void SBucket::setCipherSize(int maximumSize){
    if(sbucket_is_init == true){
        throw new runtime_error("Max Bucket Size was already set");
    }
    cipher_size = maximumSize;
    sbucket_is_init = true;
}

int SBucket::getCipherSize() {
    return cipher_size;
}

int SBucket::getSBucketSize(){
    return cipher_size + 32;
}

void SBucket::resetState(){
    sbucket_is_init = false;
}

} // namespace double_oram


