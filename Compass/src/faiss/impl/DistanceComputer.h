// /**
//  * Copyright (c) Facebook, Inc. and its affiliates.
//  *
//  * This source code is licensed under the MIT license found in the
//  * LICENSE file in the root directory of this source tree.
//  */

// #ifndef DISTANCOMPUTER_H
// #define DISTANCOMPUTER_H

// #include <vector>

// #include "../MetricType.h"
// #include "../storage/storage.h"
// namespace faiss {

// // using idx_t = int64_t;

// struct GenericDistanceComputer {
//     size_t d;
//     LinearStorage* storage;
//     std::vector<float> buf;
//     const float* q;
//     float (* dis)(const float*, const float*, size_t d);

//     GenericDistanceComputer(LinearStorage* storage, MetricType metric) : storage(storage) {
//         d = storage->d;
//         buf.resize(d * 2);
//         if(metric == METRIC_L2){
//             dis = fvec_L2sqrd;
//         } else if(metric == METRIC_INNER_PRODUCT){
//             cout << "NEG INNER PRODUCT" << endl;
//             dis = fvec_neg_inner_product;
//         } else{
//             assert(0);
//         }
//     }

//     float operator()(idx_t i) {
//         storage->reconstruct(i, buf.data());
//         return dis(q, buf.data(), d);
//     }

//     float symmetric_dis(idx_t i, idx_t j)  {
//         storage->reconstruct(i, buf.data());
//         storage->reconstruct(j, buf.data() + d);
//         return dis(buf.data() + d, buf.data(), d);
//     }

//     void set_query(const float* x)  {
//         q = x;
//     }

//     std::vector<double> get_query() {
//         std::vector<double> d_coord(q, q+d);
//         return d_coord;
//     }

//     std::vector<double> get_coord(idx_t i) {
//         std::vector<float> coord = storage->get(i);
//         std::vector<double> d_coord(coord.begin(), coord.end());
//         return d_coord;
//     }
// };

// }


// // namespace faiss {

// // /***********************************************************
// //  * The distance computer maintains a current query and computes
// //  * distances to elements in an index that supports random access.
// //  *
// //  * The DistanceComputer is not intended to be thread-safe (eg. because
// //  * it maintains counters) so the distance functions are not const,
// //  * instantiate one from each thread if needed.
// //  *
// //  * Note that the equivalent for IVF indexes is the InvertedListScanner,
// //  * that has additional methods to handle the inverted list context.
// //  ***********************************************************/
// // struct DistanceComputer {
// //     /// called before computing distances. Pointer x should remain valid
// //     /// while operator () is called
// //     virtual std::vector<double> get_query() = 0;

// //     virtual int32_t get_pca_nearest(int layer, int32_t i) = 0;

// //     virtual void set_query(const float* x) = 0;

// //     /// compute distance of vector i to current query
// //     virtual float operator()(idx_t i) = 0;

// //     /// compute distances of current query to 4 stored vectors.
// //     /// certain DistanceComputer implementations may benefit
// //     /// heavily from this.
// //     virtual void distances_batch_4(
// //             const idx_t idx0,
// //             const idx_t idx1,
// //             const idx_t idx2,
// //             const idx_t idx3,
// //             float& dis0,
// //             float& dis1,
// //             float& dis2,
// //             float& dis3) {
// //         // compute first, assign next
// //         const float d0 = this->operator()(idx0);
// //         const float d1 = this->operator()(idx1);
// //         const float d2 = this->operator()(idx2);
// //         const float d3 = this->operator()(idx3);
// //         dis0 = d0;
// //         dis1 = d1;
// //         dis2 = d2;
// //         dis3 = d3;
// //     }

// //     /// compute distance between two stored vectors
// //     virtual float symmetric_dis(idx_t i, idx_t j) = 0;

// //     virtual std::vector<double> get_coord(idx_t i) = 0;

// //     virtual ~DistanceComputer() {}
// // };

// // /*************************************************************
// //  * Specialized version of the DistanceComputer when we know that codes are
// //  * laid out in a flat index.
// //  */
// // struct FlatCodesDistanceComputer : DistanceComputer {
// //     const uint8_t* codes;
// //     size_t code_size;

// //     FlatCodesDistanceComputer(const uint8_t* codes, size_t code_size)
// //             : codes(codes), code_size(code_size) {}

// //     FlatCodesDistanceComputer() : codes(nullptr), code_size(0) {}

// //     float operator()(idx_t i) override {
// //         return distance_to_code(codes + i * code_size);
// //     }

// //     /// compute distance of current query to an encoded vector
// //     virtual float distance_to_code(const uint8_t* code) = 0;

// //     virtual ~FlatCodesDistanceComputer() {}
// // };

// // } // namespace faiss

// #endif
