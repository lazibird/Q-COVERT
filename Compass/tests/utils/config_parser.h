#pragma once
#include <string>

using namespace std;

struct Metadata {
    bool integrity;
    int M;
    int dim;

    int base_size;
    int block_size;

    int dummy_size;
    int real_bucket_size;
    int evict_rate;
    int num_levels;
    int oram_cached_levels;

    int ef_search;
    int ef_spec;
    int ef_neighbor;
    int ef_lowest_cached_layer;
    int ef_upper_steps;

    string index_path;
    string pq_path;
    string base_path;
    string query_path;
    string gt_path;
    string buckets_path;
    string pos_map_path;
    string block_map_path;
    string metadata_path;
    string graph_cache_path;
    string hash_path;
};

int parseJson(string config_path, Metadata &md, string dataset);
inline std::string compass_default_config() { return std::string(COMPASS_SOURCE_DIR) + "/tests/config/config_ring.json"; }
