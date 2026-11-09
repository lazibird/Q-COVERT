#ifndef FAISS_PRODUCT_QUANTIZER_H
#define FAISS_PRODUCT_QUANTIZER_H

#include "../MetricType.h"

#include <stdint.h>
#include <vector>
#include <cstddef>
#include <cassert>
#include <iostream>
#include <bitset>
#include <functional>


namespace faiss {

struct ProductQuantizer {
    size_t d;         ///< size of the input vectors
    size_t code_size; ///< bytes per indexed vector

    size_t M;     ///< number of subquantizers
    size_t nbits; ///< number of bits per quantization index

    // values derived from the above
    size_t dsub;  ///< dimensionality of each subvector
    size_t ksub;  ///< number of centroids for each subquantizer
    MetricType metric;

    /// Centroid table, size M * ksub * dsub.
    /// Layout: (M, ksub, dsub)
    std::vector<float> centroids;
    std::vector<uint8_t> codes;

    std::function<void(const std::vector<int>&, std::vector<std::vector<uint8_t>>&)> code_fetcher;
    std::function<void()> reset_code_oram_stats;
    std::function<void()> finish_code_oram_query;
    std::function<void(uint64_t&, uint64_t&, uint64_t&, uint64_t&, double&, double&)> read_code_oram_stats;

    /// compute derived values when d, M and nbits have been set
    void set_derived_values();

    /** Compute distance table for one vector.
     *
     * The distance table for x = [x_0 x_1 .. x_(M-1)] is a M * ksub
     * matrix that contains
     *
     *   dis_table (m, j) = || x_m - c_(m, j)||^2
     *   for m = 0..M-1 and j = 0 .. ksub - 1
     *
     * where c_(m, j) is the centroid no j of sub-quantizer m.
     *
     * @param x         input vector size d
     * @param dis_table output table, size M * ksub
     */
    void compute_distance_table(const float* x, float* dis_table) const;

    const float* get_centroids(size_t m, size_t i) const {
        return &centroids[(m * ksub + i) * dsub];
    }

    void print_stats();

};

struct PQDistanceComputer {
    ProductQuantizer* pq;
    std::vector<float> precomputed_table;
    MetricType metric;

    explicit PQDistanceComputer(ProductQuantizer* pq, MetricType metric)
        : pq(pq), metric(metric){
        precomputed_table.resize(pq->M * pq->ksub);
    }

    void set_query(const float* x) {
        pq->compute_distance_table(x, precomputed_table.data());
    }

    float operator()(const uint8_t* code) {
        float result = 0;
        for (size_t m = 0; m < pq->M; m++) {
            int code_offset = (m * pq->nbits) / 8;
            int bit_offset =  (m * pq->nbits) % 8;
            uint8_t mask = (1 << pq->nbits) - 1;
            uint8_t real_code = (code[code_offset] >> bit_offset) & mask;

            result += precomputed_table[m * pq->ksub + real_code];
            // if(result >= 1 || result <= -1){
            //     std::cout << result << " " << std::endl;
            //     std::cout << precomputed_table[m * pq->ksub + code[m]] << " " << std::endl;
            //     std::cout << precomputed_table.size() << " " << std::endl;
            //     std::cout << m * pq->ksub + code[m] << " " << std::endl;
            //     std::cout << static_cast<unsigned int>(m * pq->ksub + real_code) << " " << std::endl;
            //     // std::cout << std::bitset<8>(code[m]) << " " << std::endl;
            //     std::cout << m << " " << std::endl;
            //     assert(0);
            // }
        }
        return result;
    }


};

}

#endif
