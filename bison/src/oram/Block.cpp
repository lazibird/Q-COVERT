//
//
//
#include "Block.h"
#include <iostream>
#include <string>
#include <sstream>
#include <cassert>
#include <cstring>

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
        // this->data[i] = data[i];
        memcpy(this->data.data() + i , data + i , sizeof(int));
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
        int32_t temp;
        memcpy(&temp, this->data.data() + i, sizeof(int32_t));
        // cout << temp << " ";
		// data_holder += to_string(this->data[i]);
        data_holder += to_string(temp);
		data_holder += " ";
	}
	cout << "\nindex: " << to_string(this->index) << " data: " << data_holder << endl;
}

void Block::printBlockInt(){
	string data_holder = "";
	for (int i = 0; i<block_size; i++) {
		data_holder += to_string(this->data[i]);
		data_holder += " ";
	}
	cout << "index: " << to_string(this->index) << " data: " << data_holder << endl;

    cout << endl;
}

void Block::printBlockByte(){
    unsigned char* temp_data = reinterpret_cast<unsigned char*>(data.data());

    int temp_bs = block_size*sizeof(int);
	string data_holder = "";
	for (int i = 0; i< temp_bs; i++) {
		data_holder += to_string(temp_data[i]);
		data_holder += " ";
	}
	cout << "index: " << to_string(this->index) << " data: " << data_holder << endl;

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
        // ptr[1 + i] = data[i];
        memcpy(ptr + 1 + i, data.data() + i, sizeof(int));
    }
}

void Block::from_ptr(int* ptr){
    index = ptr[0];
    for(int i = 0; i < block_size; i++){
        // data[i] = ptr[1 + i];
        memcpy(data.data() + i, ptr + 1 + i , sizeof(int));
    }
}

void Block::to_ptr(unsigned char* ptr){
    return to_ptr((int *) ptr);
}

void Block::from_ptr(unsigned char* ptr){

    // memcpy(&index, ptr, sizeof(int));
    // memcpy(data.data(), ptr + sizeof(int), block_size * sizeof(int));

    return from_ptr((int *) ptr);
}