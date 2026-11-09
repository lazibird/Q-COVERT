#include "HNSW.h"

#include <string>
// #include <armadillo>
#include <chrono>

#include "AuxIndexStructures.h"
#include "DistanceComputer.h"
// #include <faiss/impl/IDSelector.h>
#include "../utils/prefetch.h"
#include <set>
#include <sys/time.h>

namespace faiss {

using MinimaxHeap = HNSW::MinimaxHeap;
using Node = HNSW::Node;

double elapsed() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

inline double interval(std::chrono::_V2::system_clock::time_point start){
    auto end = std::chrono::high_resolution_clock::now();
    auto interval = (end - start)/1e+9;
    return interval.count();
}

void distance_rank(
    PQDistanceComputer& pqdis,
    std::vector<int>& nb_list,
    std::map<int, uint8_t*> code_map) {

    std::vector<float> scores;

    for (int v1 : nb_list) {
        // const uint8_t* code = pqdis.pq->codes.data() + v1*pqdis.pq->M;
        uint8_t* code = code_map[v1];
        // for(int i = 0; i < pqdis.pq->M; i++){
        //     if(code[i] != code_test[i]){
        //         assert(0);
        //     }
        // }
        scores.push_back(pqdis(code));
    }

    std::vector<std::pair<float, int>> AB;
    for (size_t i = 0; i < nb_list.size(); ++i) {
        AB.push_back(std::make_pair(scores[i], nb_list[i]));
    }

    std::sort(AB.begin(), AB.end());

    for (size_t i = 0; i < AB.size(); ++i) {
        nb_list[i] = AB[i].second;
    }
}

void distance_rank_local(
    PQDistanceComputer& pqdis,
    std::vector<int>& nb_list) {

    std::vector<float> scores;
    std::vector<std::vector<uint8_t>> fetched_codes;
    if (pqdis.pq->code_fetcher) {
        pqdis.pq->code_fetcher(nb_list, fetched_codes);
        assert(fetched_codes.size() == nb_list.size());
    }

    // cout << "code size: " << pqdis.pq->codes.size();
    // cout << "distance: ";

    for (size_t i = 0; i < nb_list.size(); i++) {
        int v1 = nb_list[i];
        const uint8_t* code = pqdis.pq->code_fetcher
                ? fetched_codes[i].data()
                : pqdis.pq->codes.data() + v1*pqdis.pq->code_size;
        // uint8_t* code = code_map[v1];
        // for(int i = 0; i < pqdis.pq->M; i++){
        //     if(code[i] != code_test[i]){
        //         assert(0);
        //     }
        // }
        scores.push_back(pqdis(code));
        
        // cout << pqdis(code) << " ";
    }

    // cout << endl;

    std::vector<std::pair<float, int>> AB;
    for (size_t i = 0; i < nb_list.size(); ++i) {
        AB.push_back(std::make_pair(scores[i], nb_list[i]));
    }

    std::sort(AB.begin(), AB.end());

    for (size_t i = 0; i < AB.size(); ++i) {
        nb_list[i] = AB[i].second;
    }
}

HNSWStats HNSW::oblivious_search_preload_beam_cache_upper(
        GraphCache& gcache,
        PQDistanceComputer& pqdis,
        int k,
        idx_t* I,
        float* D,
        VisitedTable& vt,
        const SearchParametersHNSW* params) const {

    // Search Params
    int efspec = params->efSpec;
    int efn = params->efNeighbor;

    bool nb_filter = false;
    if(efn > 0){
        nb_filter = true;
    }

    // stats
    int fetch_cnt = 0;
    HNSWStats stats;

    if (entry_point == -1) {
        return stats;
    }

    int l = max_level;

    storage_idx_t nearest = entry_point;
    float d_nearest = gcache.get_dis(nearest, l);
    
    int upper_fetch_count = 0;
    for(; l >= 1; l--){
        if(l >= params->efLayer){
            // cached
            for(;;){
                // std::cout << "l: " << l << " nearest: " << nearest << std::endl;
                storage_idx_t prev_nearest = nearest;
                for (storage_idx_t v : gcache.get_neighbors(nearest, l)) {
                    if (v < 0)
                        break;
                    float dis = gcache.get_dis(v, l);
                    if (dis < d_nearest) {
                        nearest = v;
                        d_nearest = dis;
                    }
                }

                if (nearest == prev_nearest) {
                    break;
                }
            }
        } else{
            for(;;){
                if(upper_fetch_count >= params->efUpperSteps){
                    break;
                }

                // std::cout << "-> Upper ORAM in level: " << l << std::endl;
                storage_idx_t prev_nearest = nearest;

                // neighbor list
                std::vector<int> nb_list;
                for (storage_idx_t v : gcache.get_neighbors(nearest, l)) {
                    if (v < 0)
                        break;
                    nb_list.push_back(v);
                }

                // neighbor filtering
                if(nb_filter){
                    distance_rank_local(pqdis, nb_list);
                    if(nb_list.size() > efn){
                        nb_list.resize(efn);
                    }
                }

                // A corner case:
                if(!gcache.contains(nearest, l)){
                    nb_list[nb_list.size()-1] = nearest;
                }
                

                // oram fetch
                auto t_fetch = std::chrono::high_resolution_clock::now();
                // CHECK Later !!!!
                gcache.load_clusters(nb_list, std::vector<int>(nb_list.size(), l), efn);
                stats.oram_fetch += interval(t_fetch);

                // distance compute
                for(size_t v : nb_list){
                    float d = gcache.get_dis(v, l);
                    if (d < d_nearest) {
                        nearest = v;
                        d_nearest = d;
                    }
                }

                upper_fetch_count++;
                if (nearest == prev_nearest) {
                    // continue if reach second last layer and 
                    // upper_fecth_count is less than expected
                    if(l != 1 || upper_fetch_count >= params->efUpperSteps){
                        break;
                    }
                }
            }
            
        }
    }

    int efSearch = params ? params->efSearch : this->efSearch;

    // Last level B-ish FS
    int nres = 0;
    int nstep = 0;
    int ef = std::max(efSearch, k);
    MinimaxHeap candidates(ef);
    candidates.push(nearest, d_nearest);
    vt.set(nearest);
    maxheap_push(++nres, D, I, d_nearest, nearest);

    // gcache.load_clusters({nearest}, {0}, 1);

    // Load entry point
    for(int nstep = 0; nstep < (efSearch / efspec) + 1; nstep++){

        std::vector<int> v_beam;
        int cnt = 0;
        while(candidates.size() > 0){
            float d0 = 0;
            int v0 = candidates.pop_min(&d0);
            v_beam.push_back(v0);

            cnt ++;
            if(cnt == efspec){
                break;
            }
        }

        fetch_cnt ++;

        std::vector<std::vector<int>> nb_cache; 
        std::set<int> nb_set;

        {
            for(auto v0 : v_beam){
                std::vector<int> nb;
                for (storage_idx_t v1 : gcache.get_neighbors(v0, l)) {
                    if (v1 < 0)
                        break;
                    if (vt.get(v1)) {
                        continue;
                    }
                    nb_set.insert(v1);
                    nb.push_back(v1);
                }
                nb_cache.push_back(nb);
            }
        }

        std::vector<int> nb_list(nb_set.begin(), nb_set.end());

        if(nb_filter){
            distance_rank_local(pqdis, nb_list);
            int nb_size;
            if(nstep == 0){
                // first step in last layer only has one candidate
                nb_size = efn;
            } else{
                nb_size = efspec * efn;
            }
            if(nb_list.size() > nb_size){
                nb_list.resize(nb_size);
            } 

            auto t_fetch = std::chrono::high_resolution_clock::now();
            gcache.load_clusters(nb_list, std::vector<int>(nb_list.size(), l), nb_size);
            stats.oram_fetch += interval(t_fetch);
        } else{
            assert(0);
            auto t_fetch = std::chrono::high_resolution_clock::now();
            gcache.load_clusters(nb_list, std::vector<int>(nb_list.size(), l), nb_list.size());
            stats.oram_fetch += interval(t_fetch);
        }

        {
                for(node_id_t v1 : nb_list){
                    vt.set(v1);

                    float d = gcache.get_dis(v1, l);
                    
                    if (nres < k) {
                        maxheap_push(++nres, D, I, d, v1);
                    } else if (d < D[0]) {
                        maxheap_replace_top(nres, D, I, d, v1);
                    }

                    candidates.push(v1, d);
                }
        }
    }

    // std::cout << "-> Done with fetch_cnt:  " << fetch_cnt << std::endl;

    return stats;
}


HNSWStats HNSW::oblivious_search_preload_beam_cache_upper_ablation(
        GraphCache& gcache,
        PQDistanceComputer& pqdis,
        int k,
        idx_t* I,
        float* D,
        VisitedTable& vt,
        const SearchParametersHNSW* params) const {

    // Search Params
    int efspec = params->efSpec;
    int efn = params->efNeighbor;

    bool nb_filter = false;
    if(efn > 0){
        nb_filter = true;
    }

    // stats
    int fetch_cnt = 0;
    HNSWStats stats;

    if (entry_point == -1) {
        return stats;
    }

    int l = max_level;

    storage_idx_t nearest = entry_point;
    float d_nearest = gcache.get_dis(nearest, l);
    
    int upper_fetch_count = 0;
    for(; l >= 1; l--){
        if(l >= params->efLayer){
            // cached
            for(;;){
                // std::cout << "l: " << l << " nearest: " << nearest << std::endl;
                storage_idx_t prev_nearest = nearest;
                for (storage_idx_t v : gcache.get_neighbors(nearest, l)) {
                    if (v < 0)
                        break;
                    float dis = gcache.get_dis(v, l);
                    if (dis < d_nearest) {
                        nearest = v;
                        d_nearest = dis;
                    }
                }

                if (nearest == prev_nearest) {
                    break;
                }
            }
        } else{
            for(;;){
                if(upper_fetch_count >= params->efUpperSteps){
                    break;
                }

                // std::cout << "-> Upper ORAM in level: " << l << std::endl;
                storage_idx_t prev_nearest = nearest;

                // neighbor list
                std::vector<int> nb_list;
                for (storage_idx_t v : gcache.get_neighbors(nearest, l)) {
                    if (v < 0)
                        break;
                    nb_list.push_back(v);
                }

                // neighbor filtering
                if(nb_filter){
                    distance_rank_local(pqdis, nb_list);
                    // if(nb_list.size() > efn){
                    //     nb_list.resize(efn);
                    // }
                    nb_list.resize(efn);
                }

                // A corner case:
                if(!gcache.contains(nearest, l)){
                    nb_list[nb_list.size()-1] = nearest;
                }
                

                // oram fetch
                auto t_fetch = std::chrono::high_resolution_clock::now();
                // gcache.load_clusters(nb_list, std::vector<int>(nb_list.size(), l), nb_list.size());
                for(auto nb : nb_list){
                    gcache.load_clusters({nb}, {l}, 1);
                    gcache.evict();
                }
                stats.oram_fetch += interval(t_fetch);

                // distance compute
                for(size_t v : nb_list){
                    float d = gcache.get_dis(v, l);
                    if (d < d_nearest) {
                        nearest = v;
                        d_nearest = d;
                    }
                }

                upper_fetch_count++;
                if (nearest == prev_nearest) {
                    // continue if reach second last layer and 
                    // upper_fecth_count is less than expected
                    if(l != 1 || upper_fetch_count >= params->efUpperSteps){
                        break;
                    }
                }
            }
            
        }
    }

    int efSearch = params ? params->efSearch : this->efSearch;

    // Last level B-ish FS
    int nres = 0;
    int nstep = 0;
    int ef = std::max(efSearch, k);
    MinimaxHeap candidates(ef);
    candidates.push(nearest, d_nearest);
    vt.set(nearest);
    maxheap_push(++nres, D, I, d_nearest, nearest);

    // Load entry point
    for(int nstep = 0; nstep < (efSearch / efspec) + 1; nstep++){

        std::vector<int> v_beam;
        int cnt = 0;
        while(candidates.size() > 0){
            float d0 = 0;
            int v0 = candidates.pop_min(&d0);
            v_beam.push_back(v0);

            cnt ++;
            if(cnt == efspec){
                break;
            }
        }

        fetch_cnt ++;

        std::vector<std::vector<int>> nb_cache; 
        std::set<int> nb_set;

        {
            for(auto v0 : v_beam){
                std::vector<int> nb;
                for (storage_idx_t v1 : gcache.get_neighbors(v0, l)) {
                    if (v1 < 0)
                        break;
                    if (vt.get(v1)) {
                        continue;
                    }
                    nb_set.insert(v1);
                    nb.push_back(v1);
                }
                nb_cache.push_back(nb);
            }
        }

        std::vector<int> nb_list(nb_set.begin(), nb_set.end());

        if(nb_filter){
            distance_rank_local(pqdis, nb_list);
            int nb_size;
            if(nstep == 0){
                // first step in last layer only has one candidate
                nb_size = efn;
            } else{
                nb_size = efspec * efn;
            }
            // if(nb_list.size() > nb_size){
            //     nb_list.resize(nb_size);
            // } 
            nb_list.resize(nb_size);
        }

        {
            auto t_fetch = std::chrono::high_resolution_clock::now();
            // gcache.load_clusters(nb_list, std::vector<int>(nb_list.size(), l), nb_list.size());
            for(auto nb : nb_list){
                gcache.load_clusters({nb}, {l}, 1);
                gcache.evict();
            }
            stats.oram_fetch += interval(t_fetch);
        }

        {
                for(node_id_t v1 : nb_list){
                    vt.set(v1);

                    float d = gcache.get_dis(v1, l);
                    
                    if (nres < k) {
                        maxheap_push(++nres, D, I, d, v1);
                    } else if (d < D[0]) {
                        maxheap_replace_top(nres, D, I, d, v1);
                    }

                    candidates.push(v1, d);
                }
        }
    }

    // std::cout << "-> Done with fetch_cnt:  " << fetch_cnt << std::endl;

    return stats;
}

}
