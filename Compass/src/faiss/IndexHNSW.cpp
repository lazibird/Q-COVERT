/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include "IndexHNSW.h"

#include <omp.h>
#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <queue>
#include <unordered_set>
#include <map>
#include <stdio.h>
#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <cstdint>

#include "../node.h"

// #include <faiss/Index2Layer.h>
// #include <faiss/IndexFlat.h>
// #include <faiss/IndexIVFPQ.h>
#include "./impl/AuxIndexStructures.h"
#include "./impl/FaissAssert.h"
#include "./impl/DistanceComputer.h"
#include "./utils/Heap.h"
// #include "./utils/distances.h"
#include "./utils/random.h"
// #include "./utils/sorting.h"

// extern "C" {

// /* declare BLAS functions, see http://www.netlib.org/clapack/cblas/ */

// int sgemm_(
//         const char* transa,
//         const char* transb,
//         FINTEGER* m,
//         FINTEGER* n,
//         FINTEGER* k,
//         const float* alpha,
//         const float* a,
//         FINTEGER* lda,
//         const float* b,
//         FINTEGER* ldb,
//         float* beta,
//         float* c,
//         FINTEGER* ldc);
// }

namespace faiss {

using MinimaxHeap = HNSW::MinimaxHeap;
using storage_idx_t = HNSW::storage_idx_t;
using NodeDistFarther = HNSW::NodeDistFarther;

HNSWStats hnsw_stats;

IndexHNSW::IndexHNSW(int d, int M, MetricType metric)
        : d(d), hnsw(M), metric_type(metric){}

// IndexHNSW::IndexHNSW(LinearStorage* storage, int M)
//         : IndexHNSW(storage->d, storage->metric_type), hnsw(M), storage(storage) {}

IndexHNSW::~IndexHNSW() {
    // if (own_fields) {
    //     delete storage;
    // }
}

// void IndexHNSW::train(idx_t n, const float* x) {
//     // hnsw structure does not require training
//     // storage->train(n, x);
//     is_trained = true;
// }

void IndexHNSW::set_graph_cache(int lowest_cached_layer, BlockFetcher* bf, const char* fname){
    // std::cout << "Set graph cache: " << std::endl;
    gcache = new GraphCache(hnsw.max_level + 1, d, ntotal, lowest_cached_layer, bf, metric_type==METRIC_L2);

    uint64_t gsize = 0;

    {
        int fd = open(fname, O_RDONLY);
        if (fd == -1) {
            fprintf(stderr, "could not open %s\n", fname);
            perror("");
            abort();
        }

        // Get the file size using fstat
        struct stat st;
        if (fstat(fd, &st) == -1) {
            perror("Error getting file size");
            close(fd);
            abort();
        }
        size_t file_size = (size_t)st.st_size;

        unsigned char* buf = new unsigned char[file_size];

        size_t bytes_read = read(fd, buf, file_size);

        if (bytes_read != file_size) {
            perror("Error reading file");
            free(buf);
            close(fd);
            abort();
        }

        size_t per_on_size = (2 + OptNode::dim + OptNode::n_neighbors) * sizeof(int);

        assert(file_size % per_on_size == 0);

        for(size_t i = 0; i < file_size / per_on_size; i++){
            OptNode on = OptNode();
            memcpy(on.data.data(), buf + i * per_on_size, per_on_size);
            if(on.get_layer() >= lowest_cached_layer){
                gcache->add_local_cluster(on.get_id(), on.get_layer(), on);
                gsize += on.len*sizeof(int);
            }
        }
        
        delete[] buf;
    }

    // free the memory
    hnsw.neighbors = std::vector<storage_idx_t>();
    cout << "cached graph size: " << gsize << endl;
}

// SearchStats IndexHNSW::search(
//         idx_t n,
//         const float* x,
//         idx_t k,
//         float* distances,
//         idx_t* labels,
//         SearchParametersHNSW* params_in) {
//     FAISS_THROW_IF_NOT(k > 0);
//     SearchParametersHNSW* params = nullptr;

//     int efSearch = hnsw.efSearch;
//     if (params_in) {
//         // params = dynamic_cast<const SearchParametersHNSW*>(params_in);
//         // FAISS_THROW_IF_NOT_MSG(params, "params type invalid");
//         params = params_in;
//         efSearch = params->efSearch;
//     }

//     GenericDistanceComputer* dis = new GenericDistanceComputer(storage, metric_type);

//     SearchStats s_stats;

//     for(int i =0; i < hnsw.max_level; i++){
//         s_stats.nsteps_map[i] = {};
//         s_stats.nvisited_map[i] = {};
//     }

//     for (idx_t i = 0; i < n; i += 1) {
//         VisitedTable vt(ntotal);
//         idx_t* idxi = labels + i * k;
//         float* simi = distances + i * k;
//         // dis->set_query(x + i * d);
//         maxheap_heapify(k, simi, idxi);
//         HNSWStats stats = hnsw.search(*dis, k, idxi, simi, vt, params);
//         maxheap_reorder(k, simi, idxi);
//         // std::cout << "done searching " << i << std::endl;

//         for(int i = 0; i < stats.nsteps.size(); i++){
//             int idx = stats.nsteps.size() - 1 - i;
//             s_stats.nsteps_map[i].push_back(stats.nsteps[idx]);
//             s_stats.nvisited_map[i].push_back(stats.nvisited[idx]);
//         }

//         s_stats.principle_dim.insert(
//             s_stats.principle_dim.end(), 
//             stats.principle_dim.begin(), 
//             stats.principle_dim.end()
//         );
//     }

//     return s_stats;
// }

inline double interval(std::chrono::_V2::system_clock::time_point start){
    auto end = std::chrono::high_resolution_clock::now();
    auto interval = (end - start)/1e+9;
    return interval.count();
}

SearchStats IndexHNSW::oblivious_search(
        idx_t n,
        const float* x,
        idx_t k,
        float* distances,
        idx_t* labels,
        ProductQuantizer* pq,
        SearchParametersHNSW* params_in) {
    FAISS_THROW_IF_NOT(k > 0);
    SearchParametersHNSW* params = nullptr;

    int efSearch = hnsw.efSearch;
    if (params_in) {
        params = params_in;
        efSearch = params->efSearch;
    }

    PQDistanceComputer* pqdis = new PQDistanceComputer(pq, metric_type);

    SearchStats s_stats;

    for(int i =0; i < hnsw.max_level; i++){
        s_stats.nsteps_map[i] = {};
    }

    ::OramRing* oram_stats = nullptr;
    if (auto* obr = dynamic_cast<::ObliviousBlockFetcherRing*>(gcache->bf)) {
        oram_stats = obr->oram_for_stats();
    }

    double time_coords = 0;
    // double time_nb = 0;
    // double time_search = 0;

    double t_search = 0;
    double t_response = 0;
    double t_upper = 0;
    double t_fetch = 0;
    double t_last = 0; 

    for (idx_t i = 0; i < n; i += 1) {
        if (oram_stats) {
            oram_stats->reset_query_access_stats();
        }
        if (pq && pq->reset_code_oram_stats) {
            pq->reset_code_oram_stats();
        }

        auto t_0 = std::chrono::high_resolution_clock::now();
        VisitedTable vt(ntotal);
        idx_t* idxi = labels + i * k;
        float* simi = distances + i * k;
        gcache->set_query(x + i * d);
        // dis->set_query(x + i * d);
        pqdis->set_query(x + i * d);
        maxheap_heapify(k, simi, idxi);
        HNSWStats stats;
        // HNSWStats stats = hnsw.oblivious_search_preload_beam(*gcache, *dis, k, idxi, simi, vt, params);
        stats = hnsw.oblivious_search_preload_beam_cache_upper(*gcache, *pqdis, k, idxi, simi, vt, params);
        // HNSWStats stats = hnsw.oblivious_search_preload_beam_cache_upper_ablation(*gcache, *pqdis, k, idxi, simi, vt, params);
        maxheap_reorder(k, simi, idxi);

        // s_stats.stash_size_before_evict.push_back(gcache->bf->get_stash_size());

        auto reponse = interval(t_0);
        t_response += reponse;
        s_stats.perceived_latency.push_back(reponse);

        gcache->reset();
        if (pq && pq->finish_code_oram_query) {
            pq->finish_code_oram_query();
        }

        if (oram_stats) {
            s_stats.oram_bmasr_invocations_per_query.push_back(oram_stats->stat_bmasr_invocations);
            s_stats.oram_real_block_indices_per_query.push_back(oram_stats->stat_real_block_indices);
            s_stats.oram_pad_slots_per_query.push_back(oram_stats->stat_pad_slots);
            s_stats.oram_server_cipher_block_reads_per_query.push_back(oram_stats->stat_server_cipher_block_reads);
            s_stats.oram_evict_invocations_per_query.push_back(oram_stats->stat_evict_invocations);
            s_stats.oram_evict_effective_per_query.push_back(oram_stats->stat_evict_effective);
            s_stats.oram_early_reshuffle_invocations_per_query.push_back(oram_stats->stat_early_reshuffle_invocations);
            s_stats.oram_evict_time_sec_per_query.push_back(oram_stats->stat_evict_time_sec);
            s_stats.oram_bmasr_time_sec_per_query.push_back(oram_stats->stat_bmasr_time_sec);
        }
        if (pq && pq->read_code_oram_stats) {
            uint64_t pq_bmasr = 0;
            uint64_t pq_server_blocks = 0;
            uint64_t pq_early = 0;
            uint64_t pq_evict_effective = 0;
            double pq_bmasr_time = 0.0;
            double pq_evict_time = 0.0;
            pq->read_code_oram_stats(
                    pq_bmasr,
                    pq_server_blocks,
                    pq_early,
                    pq_evict_effective,
                    pq_bmasr_time,
                    pq_evict_time);
            s_stats.pq_oram_bmasr_invocations_per_query.push_back(pq_bmasr);
            s_stats.pq_oram_server_cipher_block_reads_per_query.push_back(pq_server_blocks);
            s_stats.pq_oram_early_reshuffle_invocations_per_query.push_back(pq_early);
            s_stats.pq_oram_evict_effective_per_query.push_back(pq_evict_effective);
            s_stats.pq_oram_bmasr_time_sec_per_query.push_back(pq_bmasr_time);
            s_stats.pq_oram_evict_time_sec_per_query.push_back(pq_evict_time);
        }

        // s_stats.stash_size_after_evict.push_back(gcache->bf->get_stash_size());

        auto search = interval(t_0);
        s_stats.full_latency.push_back(search);

        t_search += search;
        t_fetch += stats.oram_fetch;

        if((i+1) % 100 == 0){
            std::cout << "-> done: " <<  (i+1) << std::endl;
        }
    }


    // std::cout << "Avg. fetch latency: " <<  t_fetch / n << std::endl;

    return s_stats;
}
}
