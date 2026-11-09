//
//
//
#include "Block.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cassert>

using namespace std;

Block::Block(){//dummy index
    this->index = -1;
    this->block_size = -1;
    // this->data = nullptr;
    // assert(0);
}

Block::Block(const Block& other){
    this->block_size = other.block_size;
    this->index = other.index;
    this->data = other.data;

    // if(other.data != nullptr){
    //     this->data = new int[block_size];
        
    //     for (int i = 0; i < block_size; i++){
    //         this->data[i] = other.data[i];
    //     }
    // }
}

Block::Block(int block_size){//dummy index
    this->index = -1;
    this->block_size = block_size;
    this->data = vector<int>(block_size);
}

Block::Block(int block_size, int leaf_id, int index, int data[]) : block_size(block_size), index(index)
{
    this->data = vector<int>(block_size);
    for (int i = 0; i < block_size; i++){
        this->data[i] = data[i];
    }
}

Block::~Block()
{
    // delete[] data;
    //dtor
}

void Block::printBlock(){
	string data_holder = "";
	for (int i = 0; i<block_size; i++) {
		data_holder += to_string(this->data[i]);
		data_holder += " ";
	}
	cout << "index: " << to_string(this->index) << " data: " << data_holder << endl;
}

void Block::printBlockInt(){
	string data_holder = "";
	for (int i = 0; i<block_size; i++) {
		data_holder += to_string(this->data[i]);
		data_holder += " ";
	}
	cout << "index: " << to_string(this->index) << " data: " << data_holder << endl;

    for (int i = 0; i<block_size; i++) {
		cout << this->data[i] << " ";
	}

    cout << endl;
}

// void Block::to_io(NetIO* io){
//     io->send_data(&index, sizeof(index));
//     io->send_data(data.data(), block_size*sizeof(int));
// }

// void Block::from_io(NetIO* io){
//     io->recv_data(&index, sizeof(index));
//     io->recv_data(data.data(), block_size*sizeof(int));
// }

void Block::to_ptr(int* ptr){
    ptr[0] = index;
    for(int i = 0; i < block_size; i++){
        ptr[1 + i] = data[i];
    }
}

void Block::from_ptr(int* ptr){
    index = ptr[0];
    for(int i = 0; i < block_size; i++){
        data[i] = ptr[1 + i];
    }
}

void Block::to_ptr(unsigned char* ptr){
    return to_ptr((int *) ptr);
}

void Block::from_ptr(unsigned char* ptr){
    return from_ptr((int *) ptr);
}