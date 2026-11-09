//
//
//

#ifndef DOUBLE_ORAM_BLOCK_H
#define DOUBLE_ORAM_BLOCK_H

#include <algorithm>
#include <iostream>
#include <vector>

using namespace std;

namespace double_oram {

class Block {

public:
    int block_size;
    int index;

    // TODO: optimize data copy here
    vector<int> data;

    Block();
    Block(const Block& other);
    Block(int block_size);
    Block(int block_size, int leaf_id, int index, int data[]);
    
    void printBlock();
    void printBlockInt();
    virtual ~Block();

    // void to_io(NetIO* io);
    // void from_io(NetIO* io);

    void to_ptr(int* ptr);
    void from_ptr(int* ptr);
    void to_ptr(unsigned char* ptr);
    void from_ptr(unsigned char* ptr);

};

} // namespace double_oram

#endif //DOUBLE_ORAM_BLOCK_H
