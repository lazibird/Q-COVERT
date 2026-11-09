#pragma once

#include <vector>
#include <map>
#include <set>

#include "node.h"

/*
 * Graph storage and indexing 
 * 
 * Graph:
 * 
 * local_block_map: contains the block of nodes in the cached layers
 * temp_block_map: contains the block of nodes in the remote layers, 
 * 
 * 
 */

// Graph
// 
class GraphCache {
    public:

    int nlevel;
    int dim;
    int ntotal;
    int lowest_cached_layer;
    BlockFetcher* bf;
    bool isL2;
    float (* dis)(const float*, const float*, size_t d);

    std::map<block_id_t, OptNode> local_block_map;
    std::map<block_id_t, OptNode> temp_block_map;

    const float* q;

    void add_local_cluster(node_id_t p, int l, OptNode& on);

    // Load the cluster that contains node p
    void load_cluster(node_id_t p, int l);
    
    void load_clusters(std::vector<node_id_t> ps, std::vector<int> ls, int pad_l);

    bool contains(node_id_t p, int l);

    void set_query(const float* x);

    block_id_t get_block_id(node_id_t p, int l);

    float get_dis(node_id_t p, int l);

    vector<node_id_t> get_neighbors(node_id_t p, int l);

    void reset();

    void evict();

    GraphCache(int nlevel, int dim, int ntotal, int lowest_cached_layer, BlockFetcher* bf, bool isL2);

    ~GraphCache();

};