#ifndef PCONFIG_PARSER_H
#define PCONFIG_PARSER_H

#include <string>

using namespace std;

struct Metadata {
    bool integrity;
    int R;
    int dim;

    int base_size;
    int block_size;

    int dummy_size;
    int real_bucket_size;
    int evict_rate;
    int num_levels;
    int oram_cached_levels;
    int large_number;
    bool use_oram;
    bool debug;
    long num_queries;

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
    string index_path_prefix;
    string data_type;

    // low-client-storage ORAM path (optional; empty/0 disables)
    bool low_client_storage = false;
    bool recursive_posmap = true;
    int pq_bytes = 0;
    string pq_oram_buckets_path;
    string pq_oram_pos_map_path;
    string pq_oram_metadata_path;
    string pq_oram_hash_path;
    int pq_oram_block_size_bytes = 0;
    int pq_oram_num_levels = 0;
    int pq_oram_num_blocks = 0;
    string posmap_oram_buckets_path;
    string posmap_oram_pos_map_path;
    string posmap_oram_metadata_path;
    string posmap_oram_hash_path;
    int posmap_oram_block_size_bytes = 0;
    int posmap_oram_num_levels = 0;
    int posmap_oram_num_blocks = 0;
};

int parseJson(string config_path, Metadata &md, string dataset);
bool overwrite_json_nested_field(const std::string &filename, const std::string &dataset, const std::string &field, int new_value);

bool overwrite_json_nested_field_bool(const std::string &filename, const std::string &dataset, const std::string &field, bool new_value);
#endif
