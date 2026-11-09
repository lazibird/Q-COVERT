/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#pragma once

#include <vector>
#include <sys/time.h>

#include "impl/HNSW.h"
#include "Index.h"
// #include "storage/storage.h"
// #include <faiss/utils/utils.h>

// double getmillisecs() {
//     struct timeval tv;
//     gettimeofday(&tv, nullptr);
//     return tv.tv_sec * 1e3 + tv.tv_usec * 1e-3;
// }

namespace faiss {

struct IndexHNSW;

// struct ReconstructFromNeighbors {
//     typedef HNSW::storage_idx_t storage_idx_t;

//     const IndexHNSW& index;
//     size_t M;   // number of neighbors
//     size_t k;   // number of codebook entries
//     size_t nsq; // number of subvectors
//     size_t code_size;
//     int k_reorder; // nb to reorder. -1 = all

//     std::vector<float> codebook; // size nsq * k * (M + 1)

//     std::vector<uint8_t> codes; // size ntotal * code_size
//     size_t ntotal;
//     size_t d, dsub; // derived values

//     explicit ReconstructFromNeighbors(
//             const IndexHNSW& index,
//             size_t k = 256,
//             size_t nsq = 1);

//     /// codes must be added in the correct order and the IndexHNSW
//     /// must be populated and sorted
//     void add_codes(size_t n, const float* x);

//     size_t compute_distances(
//             size_t n,
//             const idx_t* shortlist,
//             const float* query,
//             float* distances) const;

//     /// called by add_codes
//     void estimate_code(const float* x, storage_idx_t i, uint8_t* code) const;

//     /// called by compute_distances
//     void reconstruct(storage_idx_t i, float* x, float* tmp) const;

//     void reconstruct_n(storage_idx_t n0, storage_idx_t ni, float* x) const;

//     /// get the M+1 -by-d table for neighbor coordinates for vector i
//     void get_neighbor_table(storage_idx_t i, float* out) const;
// };

/** The HNSW index is a normal random-access index with a HNSW
 * link structure built on top */

struct IndexHNSW {
    typedef HNSW::storage_idx_t storage_idx_t;

    using component_t = float;
    using distance_t = float;

    int d;        ///< vector dimension
    idx_t ntotal; ///< total nb of indexed vectors
    bool verbose; ///< verbosity level

    /// set if the Index does not require training, or if training is
    /// done already
    bool is_trained;

    /// type of metric this index uses for search
    MetricType metric_type;
    float metric_arg; ///< argument of the metric type

    // the link strcuture
    HNSW hnsw;

    // graph cache
    GraphCache* gcache;

    // the sequential storage
    bool own_fields = false;
    // LinearStorage* storage = nullptr;

//     ReconstructFromNeighbors* reconstruct_from_neighbors = nullptr;

    explicit IndexHNSW(int d = 0, int M = 32, MetricType metric = METRIC_L2);
    // explicit IndexHNSW(LinearStorage* storage, int M = 32);

    ~IndexHNSW();

    // void add(idx_t n, const float* x) override;

    // /// Trains the storage if needed
    // void train(idx_t n, const float* x) override;

    void set_graph_cache(int lowest_cached_layer, BlockFetcher* bf, const char* fname);

    /// entry point for search
    // SearchStats search(
    //         idx_t n,
    //         const float* x,
    //         idx_t k,
    //         float* distances,
    //         idx_t* labels,
    //         SearchParametersHNSW* params = nullptr);

    SearchStats oblivious_search(
            idx_t n,
            const float* x,
            idx_t k,
            float* distances,
            idx_t* labels,
            ProductQuantizer* pq,
            SearchParametersHNSW* params = nullptr);

    // void reconstruct(idx_t key, float* recons) const override;

    // void reset() override;

    // void shrink_level_0_neighbors(int size);

    // /** Perform search only on level 0, given the starting points for
    //  * each vertex.
    //  *
    //  * @param search_type 1:perform one search per nprobe, 2: enqueue
    //  *                    all entry points
    //  */
    // void search_level_0(
    //         idx_t n,
    //         const float* x,
    //         idx_t k,
    //         const storage_idx_t* nearest,
    //         const float* nearest_d,
    //         float* distances,
    //         idx_t* labels,
    //         int nprobe = 1,
    //         int search_type = 1) const;

    // /// alternative graph building
    // void init_level_0_from_knngraph(int k, const float* D, const idx_t* I);

    // /// alternative graph building
    // void init_level_0_from_entry_points(
    //         int npt,
    //         const storage_idx_t* points,
    //         const storage_idx_t* nearests);

    // // reorder links from nearest to farthest
    // void reorder_links();

    // void link_singletons();

//     void permute_entries(const idx_t* perm);
};

// /** Flat index topped with with a HNSW structure to access elements
//  *  more efficiently.
//  */

// struct IndexHNSWFlat : IndexHNSW {
//     IndexHNSWFlat();
//     IndexHNSWFlat(int d, int M, MetricType metric = METRIC_L2);
// };

// /** PQ index topped with with a HNSW structure to access elements
//  *  more efficiently.
//  */
// struct IndexHNSWPQ : IndexHNSW {
//     IndexHNSWPQ();
//     IndexHNSWPQ(int d, int pq_m, int M, int pq_nbits = 8);
//     void train(idx_t n, const float* x) override;
// };

// /** SQ index topped with with a HNSW structure to access elements
//  *  more efficiently.
//  */
// struct IndexHNSWSQ : IndexHNSW {
//     IndexHNSWSQ();
//     IndexHNSWSQ(
//             int d,
//             ScalarQuantizer::QuantizerType qtype,
//             int M,
//             MetricType metric = METRIC_L2);
// };

// /** 2-level code structure with fast random access
//  */
// struct IndexHNSW2Level : IndexHNSW {
//     IndexHNSW2Level();
//     IndexHNSW2Level(Index* quantizer, size_t nlist, int m_pq, int M);

//     void flip_to_ivf();

//     /// entry point for search
//     void search(
//             idx_t n,
//             const float* x,
//             idx_t k,
//             float* distances,
//             idx_t* labels,
//             const SearchParameters* params = nullptr) const override;
// };

} // namespace faiss
