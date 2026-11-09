#include "./graph.h"
#include <iostream>

float fvec_neg_inner_product(const float* x, const float* y, size_t d) {
    float res = 0.F;
    for (size_t i = 0; i != d; ++i) {
        res += x[i] * y[i];
    }
    return -res;
}


float fvec_L2sqrd(const float* x, const float* y, size_t d) {
    size_t i;
    float res = 0;
    for (i = 0; i < d; i++) {
        const float tmp = x[i] - y[i];
        res += tmp * tmp;
    }
    return res;
}

GraphCache::GraphCache(int nlevel, int dim, int ntotal, int lowest_cached_layer, BlockFetcher* bf, bool isL2) 
    : nlevel(nlevel), dim(dim), ntotal(ntotal), lowest_cached_layer(lowest_cached_layer), bf(bf), isL2(isL2){
        if(isL2){
            dis = fvec_L2sqrd;
        } else{
            dis = fvec_neg_inner_product;
        }
    }

void GraphCache::load_cluster(node_id_t p, int l){

    assert(0);

    // OptNode* on = bf->get_block(p, l);

    // node_id_t local_offset = 0;

    // node_id_t nid = on->get_id();
    // node_id_t real_nid = nid*nlevel + l;
    // id_to_offset[real_nid] = local_offset;
    // local_offset++;
    // id_to_block[real_nid] = block_offset;

    // block_map[block_offset] = on;

    // block_offset++;
}

void GraphCache::load_clusters(std::vector<node_id_t> ps, std::vector<int> ls, int pad_l){
    std::vector<OptNode> ons = bf->get_blocks(ps, ls, pad_l);
    for(auto on : ons){
        node_id_t nid = on.get_id();
        node_id_t real_nid = nid*nlevel + on.get_layer();
        temp_block_map[real_nid] = on;
        // std::cout << "temp block add: " << nid << " " << on.get_layer() << std::endl;
    }
}

void GraphCache::add_local_cluster(node_id_t p, int l, OptNode& on){
    node_id_t real_nid = p*nlevel + l;
    local_block_map[real_nid] = on;
}

bool GraphCache::contains(node_id_t p, int l){
    node_id_t real_p = p*nlevel + l;
    if(l < lowest_cached_layer){
        return temp_block_map.find(real_p) != temp_block_map.end();
    } else{
        return local_block_map.find(real_p) != local_block_map.end();
    }
}

void GraphCache::set_query(const float* x){
    q = x;
}

block_id_t GraphCache::get_block_id(node_id_t p, int l){
    // std::cout << "Get offset" << std::endl;
    node_id_t real_p = p*nlevel + l;
    return real_p;
    //  std::cout << "Get offset done" << std::endl;
}


float GraphCache::get_dis(node_id_t p, int l){
    float res = 0;
    node_id_t real_p = p*nlevel + l;
    float* y = NULL;
    if(l < lowest_cached_layer){
        y = temp_block_map[real_p].coord();
    } else{
        y = local_block_map[real_p].coord();
    }
    return dis(q, y, dim);
}

vector<node_id_t> GraphCache::get_neighbors(node_id_t p, int l){
    // std::cout << "Get neighbors: " << p << " l: " <<  l << std::endl;
    node_id_t real_p = get_block_id(p, l);
    if(l < lowest_cached_layer){
        auto it = temp_block_map.find(real_p);
        if(it != temp_block_map.end()){
            // std::cout << "Found in tmp block" << std::endl;
            return it->second.get_neighbors();
        } else{
            // The node is not loaded. 
            // Try upper layer's cache
            // std::cout << "upper layer caching... " << l + 1 << std::endl;
            // assert(0);
            node_id_t last_real_p = get_block_id(p, l + 1);
            if(l + 1 < lowest_cached_layer){
                // std::cout << "upper layer caching 111... " << l + 1 << std::endl;
                return temp_block_map.at(last_real_p).get_next_layer_neighbors();
            } else{
                // std::cout << "upper layer caching222... " << l + 1 << std::endl;
                return local_block_map.at(last_real_p).get_next_layer_neighbors();
            }
        }

    } else{
        // std::cout << "upper layer caching333... " << l + 1 << std::endl;
        return local_block_map.at(real_p).get_neighbors();
    }
}

void GraphCache::evict(){
    bf->evict();
}

void GraphCache::reset(){
    bf->evict();
    temp_block_map.clear();
}