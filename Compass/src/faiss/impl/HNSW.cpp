/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include "HNSW.h"

#include <string>
// #include <pca.h>
// #include <armadillo>

// #include "AuxIndexStructures.h"
#include "DistanceComputer.h"
// #include <faiss/impl/IDSelector.h>
#include "../utils/prefetch.h"
#include <set>

// #include <faiss/impl/platform_macros.h>

namespace faiss {

double dvec_L2sqr(std::vector<double> &x, std::vector<double> &y, size_t d) {
    size_t i;
    double res = 0;
    for (i = 0; i < d; i++) {
        double tmp = x[i] - y[i];
        res += tmp * tmp;
    }
    return res;
}

/**************************************************************
 * HNSW structure implementation
 **************************************************************/

// int HNSW::nb_neighbors(int layer_no) const {
//     return cum_nneighbor_per_level[layer_no + 1] -
//             cum_nneighbor_per_level[layer_no];
// }

// void HNSW::set_nb_neighbors(int level_no, int n) {
//     FAISS_THROW_IF_NOT(levels.size() == 0);
//     int cur_n = nb_neighbors(level_no);
//     for (int i = level_no + 1; i < cum_nneighbor_per_level.size(); i++) {
//         cum_nneighbor_per_level[i] += n - cur_n;
//     }
// }

int HNSW::cum_nb_neighbors(int layer_no) const {
    return cum_nneighbor_per_level[layer_no];
}

void HNSW::neighbor_range(idx_t no, int layer_no, size_t* begin, size_t* end)
        const {
    size_t o = offsets[no];
    *begin = o + cum_nb_neighbors(layer_no);
    *end = o + cum_nb_neighbors(layer_no + 1);
}

HNSW::HNSW(int M) : rng(12345) {
    // set_default_probas(M, 1.0 / log(M));
    offsets.push_back(0);
}

// int HNSW::random_level() {
//     double f = rng.rand_float();
//     // could be a bit faster with bissection
//     for (int level = 0; level < assign_probas.size(); level++) {
//         if (f < assign_probas[level]) {
//             return level;
//         }
//         f -= assign_probas[level];
//     }
//     // happens with exponentially low probability
//     return assign_probas.size() - 1;
// }

// void HNSW::set_default_probas(int M, float levelMult) {
//     int nn = 0;
//     cum_nneighbor_per_level.push_back(0);
//     for (int level = 0;; level++) {
//         float proba = exp(-level / levelMult) * (1 - exp(-1 / levelMult));
//         if (proba < 1e-9)
//             break;
//         assign_probas.push_back(proba);
//         nn += level == 0 ? M * 2 : M;
//         cum_nneighbor_per_level.push_back(nn);
//     }
// }

// void HNSW::clear_neighbor_tables(int level) {
//     for (int i = 0; i < levels.size(); i++) {
//         size_t begin, end;
//         neighbor_range(i, level, &begin, &end);
//         for (size_t j = begin; j < end; j++) {
//             neighbors[j] = -1;
//         }
//     }
// }

// void HNSW::reset() {
//     max_level = -1;
//     entry_point = -1;
//     offsets.clear();
//     offsets.push_back(0);
//     levels.clear();
//     neighbors.clear();
// }

// void HNSW::print_neighbor_stats(int level) const {
//     FAISS_THROW_IF_NOT(level < cum_nneighbor_per_level.size());
//     printf("stats on level %d, max %d neighbors per vertex:\n",
//            level,
//            nb_neighbors(level));
//     size_t tot_neigh = 0, tot_common = 0, tot_reciprocal = 0, n_node = 0;
// #pragma omp parallel for reduction(+: tot_neigh) reduction(+: tot_common) \
//   reduction(+: tot_reciprocal) reduction(+: n_node)
//     for (int i = 0; i < levels.size(); i++) {
//         if (levels[i] > level) {
//             n_node++;
//             size_t begin, end;
//             neighbor_range(i, level, &begin, &end);
//             std::unordered_set<int> neighset;
//             for (size_t j = begin; j < end; j++) {
//                 if (neighbors[j] < 0)
//                     break;
//                 neighset.insert(neighbors[j]);
//             }
//             int n_neigh = neighset.size();
//             int n_common = 0;
//             int n_reciprocal = 0;
//             for (size_t j = begin; j < end; j++) {
//                 storage_idx_t i2 = neighbors[j];
//                 if (i2 < 0)
//                     break;
//                 FAISS_ASSERT(i2 != i);
//                 size_t begin2, end2;
//                 neighbor_range(i2, level, &begin2, &end2);
//                 for (size_t j2 = begin2; j2 < end2; j2++) {
//                     storage_idx_t i3 = neighbors[j2];
//                     if (i3 < 0)
//                         break;
//                     if (i3 == i) {
//                         n_reciprocal++;
//                         continue;
//                     }
//                     if (neighset.count(i3)) {
//                         neighset.erase(i3);
//                         n_common++;
//                     }
//                 }
//             }
//             tot_neigh += n_neigh;
//             tot_common += n_common;
//             tot_reciprocal += n_reciprocal;
//         }
//     }
//     float normalizer = n_node;
//     printf("   nb of nodes at that level %zd\n", n_node);
//     printf("   neighbors per node: %.2f (%zd)\n",
//            tot_neigh / normalizer,
//            tot_neigh);
//     printf("   nb of reciprocal neighbors: %.2f\n",
//            tot_reciprocal / normalizer);
//     printf("   nb of neighbors that are also neighbor-of-neighbors: %.2f (%zd)\n",
//            tot_common / normalizer,
//            tot_common);
// }


// namespace {

using storage_idx_t = HNSW::storage_idx_t;
using NodeDistCloser = HNSW::NodeDistCloser;
using NodeDistFarther = HNSW::NodeDistFarther;

/**************************************************************
 * MinimaxHeap
 **************************************************************/

void HNSW::MinimaxHeap::push(storage_idx_t i, float v) {
    if (k == n) {
        if (v >= dis[0])
            return;
        if (ids[0] != -1) {
            --nvalid;
        }
        faiss::heap_pop<HC>(k--, dis.data(), ids.data());
    }
    faiss::heap_push<HC>(++k, dis.data(), ids.data(), v, i);
    ++nvalid;
}

float HNSW::MinimaxHeap::max() const {
    return dis[0];
}

int HNSW::MinimaxHeap::size() const {
    return nvalid;
}

void HNSW::MinimaxHeap::clear() {
    nvalid = k = 0;
}

// baseline non-vectorized version
int HNSW::MinimaxHeap::pop_min(float* vmin_out) {
    assert(k > 0);
    // returns min. This is an O(n) operation
    int i = k - 1;
    while (i >= 0) {
        if (ids[i] != -1) {
            break;
        }
        i--;
    }
    if (i == -1) {
        return -1;
    }
    int imin = i;
    float vmin = dis[i];
    i--;
    while (i >= 0) {
        if (ids[i] != -1 && dis[i] < vmin) {
            vmin = dis[i];
            imin = i;
        }
        i--;
    }
    if (vmin_out) {
        *vmin_out = vmin;
    }
    int ret = ids[imin];
    ids[imin] = -1;
    --nvalid;

    return ret;
}

int HNSW::MinimaxHeap::count_below(float thresh) {
    int n_below = 0;
    for (int i = 0; i < k; i++) {
        if (dis[i] < thresh) {
            n_below++;
        }
    }

    return n_below;
}

} // namespace faiss
