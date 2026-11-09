//
//
//

#ifndef DOUBLE_ORAM_RANDOM_H
#define DOUBLE_ORAM_RANDOM_H
#include <cmath>
#include <stdlib.h>
#include <vector>
#include <random>
#include "RandForOramInterface.h"
#include "duthomhas/csprng.hpp"

using namespace std;

namespace double_oram {

class RandomForOram : public RandForOramInterface {
	public:
		static bool is_initialized;
		static int bound;
		vector<int> rand_history;
		RandomForOram();
		void RandomForOramMT();
		void RandomForOramLCG();
		int getRandomLeafWithBound(int b);
		int getRandomLeaf();
		int getRandomLeafMT();
		int getRandomLeafLCG();
		void setBound(int totalNumOfLeaves);
		void resetState();
		void clearHistory();
		vector<int> getHistory();
		linear_congruential_engine<unsigned long, 25214903917, 11, 281474976710656> rnd_generator;
		std::mt19937 mt_generator; 
		long seed = 0L;
		double_oram_duthomhas::csprng rng;
};


//static int RandomForOram::bound = -1; 

} // namespace double_oram


#endif 
