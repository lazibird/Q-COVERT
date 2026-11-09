//
//
//

#include "RandomForOram.h"
#include <iostream>

namespace double_oram {

RandomForOram::RandomForOram() {
	this->rng(long());
	this->rand_history = vector<int>();
}

int RandomForOram::getRandomLeafWithBound(int b) {
	
	long next = this->rng();
	int randVal = next % b;
	if (randVal < 0) {
		randVal += b;
	}
	//this->rand_history.push_back(randVal);
	return randVal;
}


int RandomForOram::getRandomLeaf() {
	
	long next = this->rng();
	int randVal = next % this->bound;
	if (randVal < 0) {
		randVal += this->bound;
	}
	//this->rand_history.push_back(randVal);
	return randVal;
}

void RandomForOram::RandomForOramMT() {
	std::random_device seed1;
	mt_generator.seed(seed1());

	this->rand_history = vector<int>();
}


int RandomForOram::getRandomLeafMT() {
	long next = mt_generator();
	int randVal = next % this->bound;
	this->rand_history.push_back(randVal);
	return randVal;
}


void RandomForOram::RandomForOramLCG() {
	linear_congruential_engine<unsigned long, 0x5DEECE66DL, 11, 281474976710656> rnd_generator;
	rnd_generator.seed(0L);
	this->seed = (seed ^ 0x5DEECE66DL) & ((1L << 48) - 1);
	this->rand_history = vector<int>();
}


int RandomForOram::getRandomLeafLCG() {
	long next = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1);//rnd_generator();
	this->seed = next;
	int bits = next >> (48 - 31);
	int randVal = 0;
	if ((this->bound & -this->bound) == this->bound) {  // i.e., bound is a power of 2
    	randVal = (int)((this->bound * (long)bits) >> 31);
    	return randVal;
    }
	int val = bits % this->bound;
	while (bits - val + (bound-1) < 0) {
		next = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1);//rnd_generator();
		this->seed = next;
		bits = next >> (48 - 31);
		val = bits % this->bound;
	}
	randVal = val;
	this->rand_history.push_back(randVal);
	return randVal;
}
void RandomForOram::setBound(int totalNumOfLeaves) {
	this->bound = totalNumOfLeaves;
}

void RandomForOram::resetState() {
	this->bound = -1;
	this->rand_history.clear();
}
void RandomForOram::clearHistory() {
	this->rand_history = vector<int>();
}
vector<int> RandomForOram::getHistory() {
	return this->rand_history;
}

} // double_oram 命名空间
