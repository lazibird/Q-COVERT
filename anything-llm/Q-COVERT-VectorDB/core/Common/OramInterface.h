//
//
//

#ifndef DOUBLE_ORAM_INTERFACE_H
#define DOUBLE_ORAM_INTERFACE_H
#include "Block.h"
#include <cstdint>
#include <vector>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace double_oram {

struct PairComparator {
    bool operator()(const std::pair<int, int>& lhs, const std::pair<int, int>& rhs) const {
        // Compare the pairs based on custom criteria
        // For example, compare by first element, and if they are equal, compare by second element
        return (lhs.first < rhs.first) || (lhs.first == rhs.first && lhs.second < rhs.second);
    }
};

static constexpr uint32_t kRingMetadataCacheMagic = 0x524c4631U; // "RLF1"


struct OramConfig{
    int num_blocks;
    int bucket_size;
    int block_size;

    int num_levels;
    int num_buckets;
    int num_leaves;

    static int ceil_log2_int(int n) {
        if (n <= 0) {
            throw std::invalid_argument("num_blocks must be positive");
        }
        int result = 0;
        int value = 1;
        while (value < n) {
            value <<= 1;
            result++;
        }
        return result;
    }

    OramConfig(int block_size, int bucket_size, int num_blocks)
        : num_blocks(num_blocks), bucket_size(bucket_size), block_size(block_size){

        num_levels = ceil_log2_int(num_blocks);
        num_buckets = pow(2, num_levels)-1;
        num_leaves = pow(2, num_levels-1);

        std::cout << "> Oram Config: " << std::endl;
        std::cout << " -> num_levels: " << num_levels << std::endl;
        std::cout << " -> num_buckets: " << num_buckets << std::endl;
        std::cout << " -> real_blocks: " << num_blocks << std::endl;
        std::cout << " -> total_block_capacity: " << static_cast<long long>(num_buckets) * bucket_size << std::endl;
        std::cout << " -> num_leaves: " << num_leaves << std::endl;

    }

};

struct RingOramConfig{
    int num_blocks;
    int block_size;

    int real_bucket_size;
    int dummy_size;
    int evict_rate;
    int bucket_size;

    int num_levels;
    int num_buckets;
    int num_leaves;

    int cached_levels;

    RingOramConfig(int block_size, int real_bucket_size, int dummy_size, int evict_rate, int num_blocks, int num_levels, int cached_levels)
        : num_blocks(num_blocks), 
        dummy_size(dummy_size),
        real_bucket_size(real_bucket_size),
        evict_rate(evict_rate),
        num_levels(num_levels),
        block_size(block_size),
        cached_levels(cached_levels)
        {

        std::cout << "> Ring Oram Config: " << std::endl;
        std::cout << " -> logical_block_limit: " << num_blocks << std::endl;
        std::cout << " -> evict_rate: " << evict_rate << std::endl;
        std::cout << " -> dummy_size: " << dummy_size << std::endl;
        std::cout << " -> real_bucket_size: " << real_bucket_size << std::endl;

        bucket_size = dummy_size + real_bucket_size;
        
        // num_levels = ceil(log10(2 * num_blocks / evict_rate) / log10(2)) - 1;
        num_buckets = pow(2, num_levels)-1;
        num_leaves = pow(2, num_levels-1);

        std::cout << " -> num_levels: " << num_levels << std::endl;
        std::cout << " -> num_buckets: " << num_buckets << std::endl;
        std::cout << " -> real_block_capacity: " << static_cast<long long>(num_buckets) * real_bucket_size << std::endl;
        std::cout << " -> total_block_capacity: " << static_cast<long long>(num_buckets) * bucket_size << std::endl;
        std::cout << " -> num_leaves: " << num_leaves << std::endl;

    }

};

class OramInterface {
public:
    enum Operation {READ,WRITE};
    virtual int P(int leaf, int level) { return 0; };
    virtual int getNumLeaves() { return 0; };
    virtual int getNumLevels() { return 0; };
    virtual int getNumBlocks() { return 0; };
    virtual int getNumBuckets() { return 0; };
};


} // double_oram 命名空间

#endif //DOUBLE_ORAM_INTERFACE_H
