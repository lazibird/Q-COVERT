#include "ArgMapping.h"
#include "net_io_channel.h"

// stl
#include <iostream>
#include <iomanip>
#include <cmath>
#include <chrono>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <set>
#include <fstream>
#include <numeric>
#include <random>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <sys/stat.h>
#include <sys/types.h>

// oram
// #include "RemoteServerStorage.h"
// #include "RemoteServerRing.h"
// #include "RemoteRing.h"
#include "RandForOramInterface.h"
#include "RandomForOram.h"
#include "OramRing.h"
#include "evp.h"

// graph
#include "../../src/cluster.h"
#include "../../src/faiss/impl/HNSW.h"
#include "../../src/faiss/impl/ProductQuantizer.h"
#include "../../src/faiss/utils/io.h"
#include "../../src/faiss/index_io.h"

// json
#include "utils/config_parser.h"

using namespace std;


double elapsed() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}


// ground truth labels @gt (gt_size NNs per query), results @I (nq * k).
// Prints only R@k (standard recall@k, 0..1) and returns that value.
float compute_recall(faiss::idx_t* gt, int gt_size, faiss::idx_t* I, int nq, int k) {
    const int eval_k = std::min(k, gt_size);
    long hits = 0;
    for (int i = 0; i < nq; i++) { // loop over all queries
        faiss::idx_t* row = gt + i * gt_size;
        std::set<faiss::idx_t> gt_nns(row, row + eval_k);
        for (int j = 0; j < k; j++) { // iterate over returned nn results
            if (gt_nns.count(I[i * k + j]) != 0) {
                hits++;
            }
        }
    }
    const float recall = hits / (float)(nq * eval_k);
    printf("* Base HNSW accuracy relative to exact search:\n");
    printf("\tR@%d = %.4f\n", k, recall);
    return recall;
}

inline void tprint(string s, double t0){
    cout << "[" << std::fixed << std::setprecision(3) << (elapsed() - t0) * 1000.0 << " ms] " << s;
}


int party = 0;
int port = 8000;
string address = "127.0.0.1";
string dataset = "";
int n = -1;

int efspec = -1;
int efn = -1;
int ef_search_o = -1;
int ef_upper_steps_o = -1;
/** Retrieve Top-k neighbors from search (-1 → use 10). */
int topk_o = -1;
bool batching = true;
bool lazy_evict = true;
string f_latency = "";
string f_accuracy = "";
string f_comm = "";
string f_csv = "";
string net_profile_arg = "none";
string pq_oram_path_arg = "";
int use_pq_oram = 1;
int simulate_server_metadata = 1;
int pq_codes_per_block_arg = -1;
string pq_oram_state_path_arg = "";
int use_pm_oram = 0;
int pm_entries_per_block_arg = 32;
string pm_oram_state_path_arg = "";
int pq_metadata_client = 0;

struct NetworkProfile {
    string name;
    double bandwidth_bps;
    double rtt_ms;
};

NetworkProfile parse_network_profile(const string& raw_profile) {
    string profile = raw_profile;
    std::transform(profile.begin(), profile.end(), profile.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    if (profile == "none" || profile == "no" || profile == "0" || profile == "1") {
        return {"none", 0.0, 0.0};
    }
    if (profile == "fast" || profile == "2") {
        return {"fast", 3e9, 1.0};
    }
    if (profile == "slow" || profile == "3") {
        return {"slow", 400e6, 80.0};
    }

    cerr << "Unknown net profile: " << raw_profile
         << " (use none|fast|slow or 1|2|3)" << endl;
    exit(1);
}

string format_csv_double(double value) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6) << value;
    return oss.str();
}

string csv_escape(const string& value) {
    bool needs_quotes = false;
    for (char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needs_quotes = true;
            break;
        }
    }
    if (!needs_quotes) {
        return value;
    }

    string escaped = "\"";
    for (char c : value) {
        if (c == '"') {
            escaped += "\"\"";
        } else {
            escaped += c;
        }
    }
    escaped += "\"";
    return escaped;
}

vector<string> parse_csv_line(const string& line) {
    vector<string> cells;
    string cell;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"' && i + 1 < line.size() && line[i + 1] == '"') {
                cell += '"';
                ++i;
            } else if (c == '"') {
                in_quotes = false;
            } else {
                cell += c;
            }
        } else {
            if (c == '"') {
                in_quotes = true;
            } else if (c == ',') {
                cells.push_back(cell);
                cell.clear();
            } else {
                cell += c;
            }
        }
    }
    cells.push_back(cell);
    return cells;
}

bool ensure_parent_dirs(const string& path) {
    size_t parent_end = path.find_last_of('/');
    if (parent_end == string::npos) {
        return true;
    }

    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != string::npos && pos <= parent_end) {
        string dir = path.substr(0, pos);
        if (dir.empty()) {
            continue;
        }
        if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
            perror(("mkdir " + dir).c_str());
            return false;
        }
    }
    return true;
}

bool append_csv_column(const string& path, const vector<pair<string, string>>& rows) {
    if (!ensure_parent_dirs(path)) {
        return false;
    }

    vector<vector<string>> existing;
    {
        std::ifstream in(path);
        string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                existing.push_back(parse_csv_line(line));
            }
        }
    }

    size_t col_count = 1;
    for (const auto& row : existing) {
        col_count = std::max(col_count, row.size());
    }

    vector<vector<string>> output;
    output.reserve(rows.size());

    for (const auto& item : rows) {
        vector<string> row;
        for (const auto& existing_row : existing) {
            if (!existing_row.empty() && existing_row[0] == item.first) {
                row = existing_row;
                break;
            }
        }
        if (row.empty()) {
            row.push_back(item.first);
        } else {
            row[0] = item.first;
        }
        if (row.size() < col_count) {
            row.resize(col_count);
        }
        row.push_back(item.second);
        output.push_back(row);
    }

    const string tmp_path = path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::trunc);
        if (!out) {
            perror(("open csv temp " + tmp_path).c_str());
            return false;
        }
        for (const auto& row : output) {
            for (size_t i = 0; i < row.size(); ++i) {
                if (i > 0) {
                    out << ",";
                }
                out << csv_escape(row[i]);
            }
            out << "\n";
        }
        out.close();
        if (!out) {
            perror(("write csv temp " + tmp_path).c_str());
            std::remove(tmp_path.c_str());
            return false;
        }
    }

    if (std::rename(tmp_path.c_str(), path.c_str()) != 0) {
        perror(("rename csv temp " + tmp_path + " -> " + path).c_str());
        std::remove(tmp_path.c_str());
        return false;
    }
    return true;
}

uint64_t file_size_bytes(const string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_size);
}

uint64_t file_mtime_sec(const string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(st.st_mtime);
}

double bytes_to_mb(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

string path_parent(const string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == string::npos) {
        return ".";
    }
    if (slash == 0) {
        return "/";
    }
    return path.substr(0, slash);
}

string path_stem(const string& path) {
    size_t slash = path.find_last_of('/');
    string name = slash == string::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of('.');
    if (dot != string::npos) {
        name = name.substr(0, dot);
    }
    return name.empty() ? "pq" : name;
}

uint64_t ring_oram_ciphertext_bytes(const RingOramConfig& config) {
    return static_cast<uint64_t>(config.num_buckets)
            * static_cast<uint64_t>(config.bucket_size)
            * static_cast<uint64_t>(SBucket::getCipherSize());
}

uint64_t ring_oram_metadata_block_ids_bytes(const RingOramConfig& config) {
    return sizeof(int)
            + static_cast<uint64_t>(config.num_buckets)
            * static_cast<uint64_t>(config.bucket_size)
            * sizeof(int);
}

uint64_t pq_oram_state_header_field_count() {
    return 17;
}

uint64_t pq_oram_state_header_bytes() {
    return 16 + pq_oram_state_header_field_count() * sizeof(uint64_t);
}

uint64_t pq_oram_state_file_bytes(const RingOramConfig& config) {
    return pq_oram_state_header_bytes() + ring_oram_ciphertext_bytes(config);
}

uint64_t pm_oram_state_header_field_count() {
    return 16;
}

uint64_t pm_oram_state_header_bytes() {
    return 16 + pm_oram_state_header_field_count() * sizeof(uint64_t);
}

uint64_t pm_oram_state_file_bytes(const RingOramConfig& config) {
    return pm_oram_state_header_bytes() + ring_oram_ciphertext_bytes(config);
}

string default_pq_oram_state_path(
        const string& pq_index_path,
        const RingOramConfig& config,
        int code_size,
        int codes_per_block,
        int num_vectors) {
    std::ostringstream oss;
    oss << path_parent(pq_index_path)
        << "/result/"
        << path_stem(pq_index_path)
        << ".pq_oram"
        << ".bs" << config.block_size
        << ".code" << code_size
        << ".cpb" << codes_per_block
        << ".n" << num_vectors
        << ".z" << config.real_bucket_size
        << ".d" << config.dummy_size
        << ".l" << config.num_levels
        << ".cl" << config.cached_levels
        << ".bin";
    return oss.str();
}

string default_pm_oram_state_path(
        const Metadata& md,
        const RingOramConfig& config,
        int entries_per_oram_block,
        int num_position_entries) {
    std::ostringstream oss;
    oss << path_parent(md.base_path)
        << "/result/graph_position_map.pm_oram"
        << ".bs" << config.block_size
        << ".epb" << entries_per_oram_block
        << ".n" << num_position_entries
        << ".z" << config.real_bucket_size
        << ".d" << config.dummy_size
        << ".l" << config.num_levels
        << ".cl" << config.cached_levels
        << ".bin";
    return oss.str();
}

struct PqOramLayout {
    int num_vectors;
    int code_size;
    int codes_per_oram_block;
    int num_code_blocks;
    int num_levels;
    vector<int> position_map;
    vector<int> metadata_block_ids;
};

struct PmOramLayout {
    int num_position_entries = 0;
    int entries_per_oram_block = 0;
    int num_position_blocks = 0;
    int num_levels = 0;
    vector<int> position_map;
    vector<int> metadata_block_ids;
};

struct StorageProfile {
    uint64_t graph_position_map_bytes = 0;
    uint64_t pm_position_map_bytes = 0;
    uint64_t pq_position_map_bytes = 0;
    uint64_t graph_cache_bytes = 0;
    uint64_t pq_centroids_bytes = 0;
    uint64_t client_total_bytes = 0;

    uint64_t graph_oram_state_bytes = 0;
    uint64_t graph_oram_metadata_bytes = 0;
    uint64_t graph_oram_hash_bytes = 0;
    uint64_t pm_oram_state_bytes = 0;
    uint64_t pm_oram_metadata_bytes = 0;
    uint64_t pq_oram_state_bytes = 0;
    uint64_t pq_oram_metadata_bytes = 0;
    uint64_t server_total_bytes = 0;
};

StorageProfile build_storage_profile(
        const Metadata& md,
        const faiss::ProductQuantizer* pq,
        const RingOramConfig* pq_config,
        const PqOramLayout& pq_layout,
        const string& pq_oram_state_path,
        bool use_pq,
        const RingOramConfig* pm_config,
        const PmOramLayout& pm_layout,
        const string& pm_oram_state_path,
        bool use_pm) {
    StorageProfile profile;

    profile.graph_position_map_bytes = file_size_bytes(md.pos_map_path);
    profile.pm_position_map_bytes = use_pm
            ? static_cast<uint64_t>(pm_layout.position_map.size()) * sizeof(int)
            : 0;
    profile.pq_position_map_bytes = use_pq
            ? static_cast<uint64_t>(pq_layout.position_map.size()) * sizeof(int)
            : 0;
    profile.graph_cache_bytes = file_size_bytes(md.graph_cache_path);
    profile.pq_centroids_bytes = pq
            ? static_cast<uint64_t>(pq->centroids.size()) * sizeof(float)
            : 0;
    profile.client_total_bytes =
            (use_pm ? 0 : profile.graph_position_map_bytes)
            + profile.pm_position_map_bytes
            + profile.pq_position_map_bytes
            + profile.graph_cache_bytes
            + profile.pq_centroids_bytes;

    profile.graph_oram_state_bytes = file_size_bytes(md.buckets_path);
    profile.graph_oram_metadata_bytes = simulate_server_metadata
            ? file_size_bytes(md.metadata_path)
            : 0;
    profile.graph_oram_hash_bytes = md.integrity ? file_size_bytes(md.hash_path) : 0;
    if (use_pm && pm_config) {
        uint64_t persisted_size = file_size_bytes(pm_oram_state_path);
        if (persisted_size == 0) {
            persisted_size = pm_oram_state_file_bytes(*pm_config);
        }
        profile.pm_oram_state_bytes = persisted_size;
        profile.pm_oram_metadata_bytes = simulate_server_metadata
                ? ring_oram_metadata_block_ids_bytes(*pm_config)
                : 0;
    }
    if (use_pq && pq_config) {
        uint64_t persisted_size = file_size_bytes(pq_oram_state_path);
        if (persisted_size == 0) {
            persisted_size = pq_oram_state_file_bytes(*pq_config);
        }
        profile.pq_oram_state_bytes = persisted_size;
        if (pq_metadata_client) {
            profile.pq_oram_metadata_bytes = ring_oram_metadata_block_ids_bytes(*pq_config);
            profile.client_total_bytes += profile.pq_oram_metadata_bytes;
        } else {
            profile.pq_oram_metadata_bytes = simulate_server_metadata
                    ? ring_oram_metadata_block_ids_bytes(*pq_config)
                    : 0;
        }
    }
    profile.server_total_bytes =
            profile.graph_oram_state_bytes
            + profile.graph_oram_metadata_bytes
            + profile.graph_oram_hash_bytes
            + profile.pm_oram_state_bytes
            + profile.pm_oram_metadata_bytes
            + profile.pq_oram_state_bytes
            + (pq_metadata_client ? 0 : profile.pq_oram_metadata_bytes);

    return profile;
}

void print_storage_profile(
        const StorageProfile& profile,
        const string& pq_oram_state_path,
        const string& pm_oram_state_path,
        bool use_pm) {
    std::cout << "Client storage breakdown (simulated):" << std::endl;
    if (use_pm) {
        std::cout << "  Graph ORAM position map: remote via PM ORAM, excluded from client total"
                  << " (" << bytes_to_mb(profile.graph_position_map_bytes)
                  << " MB original)" << std::endl;
        std::cout << "  PM ORAM position map: " << bytes_to_mb(profile.pm_position_map_bytes)
                  << " MB (" << profile.pm_position_map_bytes << " bytes)" << std::endl;
    } else {
        std::cout << "  Graph ORAM position map: " << bytes_to_mb(profile.graph_position_map_bytes)
                  << " MB (" << profile.graph_position_map_bytes << " bytes)" << std::endl;
    }
    std::cout << "  PQ ORAM position map: " << bytes_to_mb(profile.pq_position_map_bytes)
              << " MB (" << profile.pq_position_map_bytes << " bytes)" << std::endl;
    std::cout << "  graph_cache: " << bytes_to_mb(profile.graph_cache_bytes)
              << " MB (" << profile.graph_cache_bytes << " bytes)" << std::endl;
    std::cout << "  PQ centroids: " << bytes_to_mb(profile.pq_centroids_bytes)
              << " MB (" << profile.pq_centroids_bytes << " bytes)" << std::endl;
    if (pq_metadata_client) {
        std::cout << "  PQ ORAM metadata (client-held): " << bytes_to_mb(profile.pq_oram_metadata_bytes)
                  << " MB (" << profile.pq_oram_metadata_bytes << " bytes)" << std::endl;
    }
    std::cout << "  Client storage total: " << bytes_to_mb(profile.client_total_bytes)
              << " MB (" << profile.client_total_bytes << " bytes)"
              << (pq_metadata_client
                          ? " [excludes graph/PM ORAM metadata, PQ codes, block_mapping]"
                          : " [excludes Ring ORAM metadata, PQ codes, block_mapping]")
              << std::endl;

    std::cout << "Server storage breakdown (simulated):" << std::endl;
    std::cout << "  Graph ORAM server state: " << bytes_to_mb(profile.graph_oram_state_bytes)
              << " MB (" << profile.graph_oram_state_bytes << " bytes)" << std::endl;
    std::cout << "  Graph ORAM metadata: " << bytes_to_mb(profile.graph_oram_metadata_bytes)
              << " MB (" << profile.graph_oram_metadata_bytes << " bytes)" << std::endl;
    if (profile.graph_oram_hash_bytes > 0) {
        std::cout << "  Graph ORAM integrity hashes: " << bytes_to_mb(profile.graph_oram_hash_bytes)
                  << " MB (" << profile.graph_oram_hash_bytes << " bytes)" << std::endl;
    }
    std::cout << "  PM ORAM server state: " << bytes_to_mb(profile.pm_oram_state_bytes)
              << " MB (" << profile.pm_oram_state_bytes << " bytes)"
              << (pm_oram_state_path.empty() ? "" : (" [" + pm_oram_state_path + "]"))
              << std::endl;
    std::cout << "  PM ORAM metadata: " << bytes_to_mb(profile.pm_oram_metadata_bytes)
              << " MB (" << profile.pm_oram_metadata_bytes << " bytes)" << std::endl;
    std::cout << "  PQ ORAM server state: " << bytes_to_mb(profile.pq_oram_state_bytes)
              << " MB (" << profile.pq_oram_state_bytes << " bytes)"
              << (pq_oram_state_path.empty() ? "" : (" [" + pq_oram_state_path + "]"))
              << std::endl;
    if (pq_metadata_client) {
        std::cout << "  PQ ORAM metadata: client-held, excluded from server total" << std::endl;
    } else {
        std::cout << "  PQ ORAM metadata: " << bytes_to_mb(profile.pq_oram_metadata_bytes)
                  << " MB (" << profile.pq_oram_metadata_bytes << " bytes)" << std::endl;
    }
    std::cout << "  Server storage total: " << bytes_to_mb(profile.server_total_bytes)
              << " MB (" << profile.server_total_bytes << " bytes)" << std::endl;
}

class DeterministicRandForOram : public RandForOramInterface {
public:
    uint64_t state;
    int bound;

    explicit DeterministicRandForOram(uint64_t seed = 0x9e3779b97f4a7c15ULL)
            : state(seed), bound(1) {}

    int getRandomLeafWithBound(int b) override {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return (int)((state >> 32) % (uint64_t)b);
    }

    int getRandomLeaf() override {
        return getRandomLeafWithBound(bound);
    }

    void setBound(int num_leaves) override {
        bound = num_leaves;
    }
};

int ceil_div_int(int a, int b) {
    return (a + b - 1) / b;
}

int compute_ring_levels_for_blocks(int num_blocks, int real_bucket_size) {
    int levels = 2;
    while (((1 << (levels - 1)) * real_bucket_size) < num_blocks) {
        levels++;
    }
    return levels;
}

int ring_bucket_position(int leaf, int level, int num_levels) {
    return (1 << level) - 1 + (leaf >> (num_levels - level - 1));
}

int read_position_map_entry_count(const string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        perror(("open position map " + path).c_str());
        return 0;
    }
    int count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(int));
    return in ? count : 0;
}

vector<int> read_position_map_entries(const string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        perror(("open position map " + path).c_str());
        return {};
    }
    int count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(int));
    if (!in || count < 0) {
        return {};
    }
    vector<int> entries((size_t)count);
    in.read(reinterpret_cast<char*>(entries.data()), (size_t)count * sizeof(int));
    if (!in) {
        return {};
    }
    return entries;
}

PqOramLayout build_pq_oram_layout(
        int num_vectors,
        int code_size,
        const RingOramConfig& config,
        int codes_per_oram_block) {
    PqOramLayout layout;
    layout.num_vectors = num_vectors;
    layout.code_size = code_size;
    const int payload_bytes = config.block_size * (int)sizeof(int);
    assert(code_size <= payload_bytes);
    assert(codes_per_oram_block > 0);
    assert((size_t)codes_per_oram_block * (size_t)code_size <= (size_t)payload_bytes);
    layout.codes_per_oram_block = codes_per_oram_block;
    layout.num_code_blocks = ceil_div_int(num_vectors, layout.codes_per_oram_block);
    layout.num_levels = config.num_levels;
    layout.position_map.assign(layout.num_code_blocks, 0);
    layout.metadata_block_ids.assign(config.num_buckets * config.bucket_size, -1);

    vector<int> slots(config.num_leaves * config.real_bucket_size);
    std::iota(slots.begin(), slots.end(), 0);
    std::mt19937 rng(0x5eed1234);
    std::shuffle(slots.begin(), slots.end(), rng);

    for (int block_id = 0; block_id < layout.num_code_blocks; block_id++) {
        int slot = slots[block_id];
        int leaf = slot / config.real_bucket_size;
        assert(leaf < config.num_leaves);
        int bucket = ring_bucket_position(leaf, layout.num_levels - 1, layout.num_levels);
        int offset = slot % config.real_bucket_size;
        layout.position_map[block_id] = leaf;
        layout.metadata_block_ids[bucket * config.bucket_size + offset] = block_id;
    }
    return layout;
}

PmOramLayout build_pm_oram_layout(
        int num_position_entries,
        const RingOramConfig& config,
        int entries_per_oram_block) {
    PmOramLayout layout;
    layout.num_position_entries = num_position_entries;
    layout.entries_per_oram_block = entries_per_oram_block;
    layout.num_position_blocks = ceil_div_int(num_position_entries, entries_per_oram_block);
    layout.num_levels = config.num_levels;
    layout.position_map.assign(layout.num_position_blocks, 0);
    layout.metadata_block_ids.assign(config.num_buckets * config.bucket_size, -1);

    vector<int> slots(config.num_leaves * config.real_bucket_size);
    std::iota(slots.begin(), slots.end(), 0);
    std::mt19937 rng(0x6d5a56b9);
    std::shuffle(slots.begin(), slots.end(), rng);

    for (int block_id = 0; block_id < layout.num_position_blocks; block_id++) {
        int slot = slots[block_id];
        int leaf = slot / config.real_bucket_size;
        assert(leaf < config.num_leaves);
        int bucket = ring_bucket_position(leaf, layout.num_levels - 1, layout.num_levels);
        int offset = slot % config.real_bucket_size;
        layout.position_map[block_id] = leaf;
        layout.metadata_block_ids[bucket * config.bucket_size + offset] = block_id;
    }
    return layout;
}

vector<unsigned char> build_pq_oram_ciphertexts(
        const faiss::ProductQuantizer* pq,
        const RingOramConfig& config,
        const PqOramLayout& layout) {
    const size_t slot_count = (size_t)config.num_buckets * (size_t)config.bucket_size;
    const size_t cipher_size = (size_t)SBucket::getCipherSize();
    vector<unsigned char> ciphertexts(slot_count * cipher_size);
    const int per_block_size = (1 + config.block_size) * (int)sizeof(int);

    for (size_t slot = 0; slot < slot_count; slot++) {
        Block block(config.block_size);
        int block_id = layout.metadata_block_ids[slot];
        if (block_id >= 0) {
            block.index = block_id;
            unsigned char* packed = reinterpret_cast<unsigned char*>(block.data.data());
            const size_t packed_bytes = (size_t)config.block_size * sizeof(int);
            memset(packed, 0, packed_bytes);

            const int first_vector = block_id * layout.codes_per_oram_block;
            const int count = std::min(layout.codes_per_oram_block, layout.num_vectors - first_vector);
            if (count > 0) {
                memcpy(
                        packed,
                        pq->codes.data() + (size_t)first_vector * (size_t)layout.code_size,
                        (size_t)count * (size_t)layout.code_size);
            }
        }

        unsigned char* plaintext = new unsigned char[cipher_size];
        block.to_ptr(plaintext);
        int ctx_len = encrypt_wrapper(
                plaintext,
                (size_t)per_block_size * (size_t)Bucket::getMaxSize(),
                ciphertexts.data() + slot * cipher_size);
        assert(ctx_len == SBucket::getCipherSize());
        delete[] plaintext;
    }
    return ciphertexts;
}

vector<unsigned char> build_pm_oram_ciphertexts(
        const vector<int>& graph_position_entries,
        const RingOramConfig& config,
        const PmOramLayout& layout) {
    const size_t slot_count = (size_t)config.num_buckets * (size_t)config.bucket_size;
    const size_t cipher_size = (size_t)SBucket::getCipherSize();
    vector<unsigned char> ciphertexts(slot_count * cipher_size);
    const int per_block_size = (1 + config.block_size) * (int)sizeof(int);

    for (size_t slot = 0; slot < slot_count; slot++) {
        Block block(config.block_size);
        int block_id = layout.metadata_block_ids[slot];
        if (block_id >= 0) {
            block.index = block_id;
            std::fill(block.data.begin(), block.data.end(), 0);
            const int first_entry = block_id * layout.entries_per_oram_block;
            const int count = std::min(
                    layout.entries_per_oram_block,
                    layout.num_position_entries - first_entry);
            for (int i = 0; i < count; i++) {
                block.data[i] = graph_position_entries[first_entry + i];
            }
        }

        unsigned char* plaintext = new unsigned char[cipher_size];
        block.to_ptr(plaintext);
        int ctx_len = encrypt_wrapper(
                plaintext,
                (size_t)per_block_size * (size_t)Bucket::getMaxSize(),
                ciphertexts.data() + slot * cipher_size);
        assert(ctx_len == SBucket::getCipherSize());
        delete[] plaintext;
    }
    return ciphertexts;
}

template<typename T>
bool read_binary_scalar(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

template<typename T>
void write_binary_scalar(std::ofstream& out, T value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

bool read_pq_oram_state(
        const string& path,
        const RingOramConfig& config,
        const PqOramLayout& layout,
        uint64_t pq_file_size,
        uint64_t pq_file_mtime,
        vector<unsigned char>& ciphertexts) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    char magic[16];
    in.read(magic, sizeof(magic));
    const char expected_magic[16] = {
            'P','Q','O','R','A','M','S','T','A','T','E','1','\0','\0','\0','\0'};
    if (!in || memcmp(magic, expected_magic, sizeof(magic)) != 0) {
        return false;
    }

    vector<uint64_t> fields(pq_oram_state_header_field_count());
    for (uint64_t& field : fields) {
        if (!read_binary_scalar(in, field)) {
            return false;
        }
    }

    const uint64_t expected_ciphertext_bytes = ring_oram_ciphertext_bytes(config);
    vector<uint64_t> expected = {
            1,
            static_cast<uint64_t>(config.block_size),
            static_cast<uint64_t>(config.real_bucket_size),
            static_cast<uint64_t>(config.dummy_size),
            static_cast<uint64_t>(config.evict_rate),
            static_cast<uint64_t>(config.num_blocks),
            static_cast<uint64_t>(config.num_levels),
            static_cast<uint64_t>(config.cached_levels),
            static_cast<uint64_t>(config.bucket_size),
            static_cast<uint64_t>(config.num_buckets),
            static_cast<uint64_t>(SBucket::getCipherSize()),
            static_cast<uint64_t>(layout.num_vectors),
            static_cast<uint64_t>(layout.code_size),
            static_cast<uint64_t>(layout.codes_per_oram_block),
            pq_file_size,
            pq_file_mtime,
            expected_ciphertext_bytes};
    if (fields != expected) {
        return false;
    }

    ciphertexts.resize(expected_ciphertext_bytes);
    in.read(reinterpret_cast<char*>(ciphertexts.data()), ciphertexts.size());
    return static_cast<bool>(in);
}

void write_pq_oram_state(
        const string& path,
        const RingOramConfig& config,
        const PqOramLayout& layout,
        uint64_t pq_file_size,
        uint64_t pq_file_mtime,
        const vector<unsigned char>& ciphertexts) {
    ensure_parent_dirs(path);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        perror(("open pq oram state " + path).c_str());
        return;
    }

    const char magic[16] = {
            'P','Q','O','R','A','M','S','T','A','T','E','1','\0','\0','\0','\0'};
    out.write(magic, sizeof(magic));
    vector<uint64_t> fields = {
            1,
            static_cast<uint64_t>(config.block_size),
            static_cast<uint64_t>(config.real_bucket_size),
            static_cast<uint64_t>(config.dummy_size),
            static_cast<uint64_t>(config.evict_rate),
            static_cast<uint64_t>(config.num_blocks),
            static_cast<uint64_t>(config.num_levels),
            static_cast<uint64_t>(config.cached_levels),
            static_cast<uint64_t>(config.bucket_size),
            static_cast<uint64_t>(config.num_buckets),
            static_cast<uint64_t>(SBucket::getCipherSize()),
            static_cast<uint64_t>(layout.num_vectors),
            static_cast<uint64_t>(layout.code_size),
            static_cast<uint64_t>(layout.codes_per_oram_block),
            pq_file_size,
            pq_file_mtime,
            static_cast<uint64_t>(ciphertexts.size())};
    for (uint64_t field : fields) {
        write_binary_scalar(out, field);
    }
    out.write(reinterpret_cast<const char*>(ciphertexts.data()), ciphertexts.size());
}

bool read_pm_oram_state(
        const string& path,
        const RingOramConfig& config,
        const PmOramLayout& layout,
        uint64_t pos_map_file_size,
        uint64_t pos_map_file_mtime,
        vector<unsigned char>& ciphertexts) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    char magic[16];
    in.read(magic, sizeof(magic));
    const char expected_magic[16] = {
            'P','M','O','R','A','M','S','T','A','T','E','1','\0','\0','\0','\0'};
    if (!in || memcmp(magic, expected_magic, sizeof(magic)) != 0) {
        return false;
    }

    vector<uint64_t> fields(pm_oram_state_header_field_count());
    for (uint64_t& field : fields) {
        if (!read_binary_scalar(in, field)) {
            return false;
        }
    }

    const uint64_t expected_ciphertext_bytes = ring_oram_ciphertext_bytes(config);
    vector<uint64_t> expected = {
            1,
            static_cast<uint64_t>(config.block_size),
            static_cast<uint64_t>(config.real_bucket_size),
            static_cast<uint64_t>(config.dummy_size),
            static_cast<uint64_t>(config.evict_rate),
            static_cast<uint64_t>(config.num_blocks),
            static_cast<uint64_t>(config.num_levels),
            static_cast<uint64_t>(config.cached_levels),
            static_cast<uint64_t>(config.bucket_size),
            static_cast<uint64_t>(config.num_buckets),
            static_cast<uint64_t>(SBucket::getCipherSize()),
            static_cast<uint64_t>(layout.num_position_entries),
            static_cast<uint64_t>(layout.entries_per_oram_block),
            pos_map_file_size,
            pos_map_file_mtime,
            expected_ciphertext_bytes};
    if (fields != expected) {
        return false;
    }

    ciphertexts.resize(expected_ciphertext_bytes);
    in.read(reinterpret_cast<char*>(ciphertexts.data()), ciphertexts.size());
    return static_cast<bool>(in);
}

void write_pm_oram_state(
        const string& path,
        const RingOramConfig& config,
        const PmOramLayout& layout,
        uint64_t pos_map_file_size,
        uint64_t pos_map_file_mtime,
        const vector<unsigned char>& ciphertexts) {
    ensure_parent_dirs(path);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        perror(("open pm oram state " + path).c_str());
        return;
    }

    const char magic[16] = {
            'P','M','O','R','A','M','S','T','A','T','E','1','\0','\0','\0','\0'};
    out.write(magic, sizeof(magic));
    vector<uint64_t> fields = {
            1,
            static_cast<uint64_t>(config.block_size),
            static_cast<uint64_t>(config.real_bucket_size),
            static_cast<uint64_t>(config.dummy_size),
            static_cast<uint64_t>(config.evict_rate),
            static_cast<uint64_t>(config.num_blocks),
            static_cast<uint64_t>(config.num_levels),
            static_cast<uint64_t>(config.cached_levels),
            static_cast<uint64_t>(config.bucket_size),
            static_cast<uint64_t>(config.num_buckets),
            static_cast<uint64_t>(SBucket::getCipherSize()),
            static_cast<uint64_t>(layout.num_position_entries),
            static_cast<uint64_t>(layout.entries_per_oram_block),
            pos_map_file_size,
            pos_map_file_mtime,
            static_cast<uint64_t>(ciphertexts.size())};
    for (uint64_t field : fields) {
        write_binary_scalar(out, field);
    }
    out.write(reinterpret_cast<const char*>(ciphertexts.data()), ciphertexts.size());
}

class PQCodeOramClient {
public:
    OramRing* oram;
    int block_size;
    int code_size;
    int codes_per_oram_block;
    int max_blocks_per_access;

    PQCodeOramClient(
            OramRing* oram,
            int block_size,
            int code_size,
            int codes_per_oram_block,
            int dummy_size)
            : oram(oram),
              block_size(block_size),
              code_size(code_size),
              codes_per_oram_block(codes_per_oram_block),
              max_blocks_per_access(std::max(1, dummy_size - 1)) {}

    void fetch_codes(const vector<int>& ids, vector<vector<uint8_t>>& out) {
        out.assign(ids.size(), vector<uint8_t>((size_t)code_size));
        if (ids.empty()) {
            return;
        }

        vector<int> unique_blocks;
        unordered_map<int, int> block_to_unique_index;
        for (int id : ids) {
            int block_id = id / codes_per_oram_block;
            if (block_to_unique_index.find(block_id) == block_to_unique_index.end()) {
                int pos = (int)unique_blocks.size();
                block_to_unique_index[block_id] = pos;
                unique_blocks.push_back(block_id);
            }
        }

        unordered_map<int, vector<int>> fetched_blocks;
        fetched_blocks.reserve(unique_blocks.size());
        for (size_t begin = 0; begin < unique_blocks.size(); begin += (size_t)max_blocks_per_access) {
            const size_t end = std::min(
                    begin + (size_t)max_blocks_per_access,
                    unique_blocks.size());
            vector<int> chunk(unique_blocks.begin() + begin, unique_blocks.begin() + end);
            vector<OramInterface::Operation> ops(
                    chunk.size(),
                    OramInterface::Operation::READ);
            vector<int*> newdata(chunk.size(), nullptr);
            vector<int*> blocks = oram->access(
                    ops,
                    chunk,
                    newdata,
                    (int)chunk.size());

            for (size_t i = 0; i < chunk.size(); i++) {
                fetched_blocks.emplace(chunk[i], vector<int>(blocks[i], blocks[i] + block_size));
                delete[] blocks[i];
            }
        }

        for (size_t i = 0; i < ids.size(); i++) {
            int block_id = ids[i] / codes_per_oram_block;
            int offset_in_block = ids[i] % codes_per_oram_block;
            auto block_it = fetched_blocks.find(block_id);
            assert(block_it != fetched_blocks.end());
            unsigned char* packed = reinterpret_cast<unsigned char*>(block_it->second.data());
            memcpy(
                    out[i].data(),
                    packed + (size_t)offset_in_block * (size_t)code_size,
                    (size_t)code_size);
        }
    }
};

struct OramStatsSnapshot {
    uint64_t bmasr_invocations = 0;
    uint64_t real_block_indices = 0;
    uint64_t pad_slots = 0;
    uint64_t server_cipher_block_reads = 0;
    uint64_t evict_invocations = 0;
    uint64_t evict_effective = 0;
    uint64_t early_reshuffle_invocations = 0;
    double evict_time_sec = 0.0;
    double bmasr_time_sec = 0.0;
};

OramStatsSnapshot snapshot_oram_stats(const OramRing* oram) {
    OramStatsSnapshot s;
    s.bmasr_invocations = oram->stat_bmasr_invocations;
    s.real_block_indices = oram->stat_real_block_indices;
    s.pad_slots = oram->stat_pad_slots;
    s.server_cipher_block_reads = oram->stat_server_cipher_block_reads;
    s.evict_invocations = oram->stat_evict_invocations;
    s.evict_effective = oram->stat_evict_effective;
    s.early_reshuffle_invocations = oram->stat_early_reshuffle_invocations;
    s.evict_time_sec = oram->stat_evict_time_sec;
    s.bmasr_time_sec = oram->stat_bmasr_time_sec;
    return s;
}

void add_oram_stats_delta(
        OramRing* dst,
        const OramStatsSnapshot& before,
        const OramStatsSnapshot& after) {
    dst->stat_bmasr_invocations += after.bmasr_invocations - before.bmasr_invocations;
    dst->stat_real_block_indices += after.real_block_indices - before.real_block_indices;
    dst->stat_pad_slots += after.pad_slots - before.pad_slots;
    dst->stat_server_cipher_block_reads +=
            after.server_cipher_block_reads - before.server_cipher_block_reads;
    dst->stat_evict_invocations += after.evict_invocations - before.evict_invocations;
    dst->stat_evict_effective += after.evict_effective - before.evict_effective;
    dst->stat_early_reshuffle_invocations +=
            after.early_reshuffle_invocations - before.early_reshuffle_invocations;
    dst->stat_evict_time_sec += after.evict_time_sec - before.evict_time_sec;
    dst->stat_bmasr_time_sec += after.bmasr_time_sec - before.bmasr_time_sec;
}

class PositionMapOramClient {
public:
    OramRing* graph_oram;
    OramRing* pm_oram;
    int block_size;
    int entries_per_oram_block;
    int max_blocks_per_access;

    PositionMapOramClient(
            OramRing* graph_oram,
            OramRing* pm_oram,
            int block_size,
            int entries_per_oram_block,
            int dummy_size)
            : graph_oram(graph_oram),
              pm_oram(pm_oram),
              block_size(block_size),
              entries_per_oram_block(entries_per_oram_block),
              max_blocks_per_access(std::max(1, dummy_size - 1)) {}

    void access_positions(
            const vector<int>& block_indices,
            const vector<int>& old_leaves,
            const vector<int>& new_leaves) {
        if (block_indices.empty()) {
            return;
        }
        assert(block_indices.size() == old_leaves.size());
        assert(block_indices.size() == new_leaves.size());

        vector<int> unique_pm_blocks;
        unordered_map<int, vector<pair<int, int>>> updates_by_pm_block;
        unordered_map<int, int> seen;
        for (size_t i = 0; i < block_indices.size(); i++) {
            const int graph_block_id = block_indices[i];
            assert(graph_block_id >= 0);
            assert((size_t)graph_block_id < graph_oram->position_map.size());
            assert(graph_oram->position_map[graph_block_id] == old_leaves[i]);

            const int pm_block_id = graph_block_id / entries_per_oram_block;
            const int offset = graph_block_id % entries_per_oram_block;
            updates_by_pm_block[pm_block_id].push_back({offset, new_leaves[i]});
            if (seen.find(pm_block_id) == seen.end()) {
                seen[pm_block_id] = (int)unique_pm_blocks.size();
                unique_pm_blocks.push_back(pm_block_id);
            }
        }

        const OramStatsSnapshot before = snapshot_oram_stats(pm_oram);
        for (size_t begin = 0; begin < unique_pm_blocks.size(); begin += (size_t)max_blocks_per_access) {
            const size_t end = std::min(
                    begin + (size_t)max_blocks_per_access,
                    unique_pm_blocks.size());
            vector<int> chunk(unique_pm_blocks.begin() + begin, unique_pm_blocks.begin() + end);
            vector<vector<int>> write_buffers(chunk.size(), vector<int>(block_size, 0));

            for (size_t ci = 0; ci < chunk.size(); ci++) {
                const int pm_block_id = chunk[ci];
                const int first_entry = pm_block_id * entries_per_oram_block;
                const int count = std::min(
                        entries_per_oram_block,
                        (int)graph_oram->position_map.size() - first_entry);
                for (int j = 0; j < count; j++) {
                    write_buffers[ci][j] = graph_oram->position_map[first_entry + j];
                }
                for (const auto& update : updates_by_pm_block[pm_block_id]) {
                    write_buffers[ci][update.first] = update.second;
                }
            }

            vector<OramInterface::Operation> ops(
                    chunk.size(),
                    OramInterface::Operation::WRITE);
            vector<int*> newdata;
            newdata.reserve(write_buffers.size());
            for (auto& buffer : write_buffers) {
                newdata.push_back(buffer.data());
            }
            vector<int*> old_blocks = pm_oram->access(ops, chunk, newdata, (int)chunk.size());
            for (int* block : old_blocks) {
                delete[] block;
            }
        }
        const OramStatsSnapshot after = snapshot_oram_stats(pm_oram);
        add_oram_stats_delta(graph_oram, before, after);
    }
};

int main(int argc, char **argv) {
    std::string config_path = compass_default_config();
    ArgMapping amap;
    amap.arg("c", config_path, "Configuration JSON (default: tests/config/config_ring.json)");
    amap.arg("r", party, "Role of party: Server = 1; Client = 2");
    amap.arg("p", port, "Port Number");
    amap.arg("d", dataset, "Dataset: [sift, deep1M, sift10M, sift100M, trip, msmarco, laion]");
    amap.arg("n", n, "# queries");
    amap.arg("ip", address, "IP Address of server");
    amap.arg("efspec", efspec, "Size of speculation set");
    amap.arg("efn", efn, "Size of directional filter");
    amap.arg("ef_search", ef_search_o, "Override ef_search (-1: use config)");
    amap.arg("ef_upper_steps", ef_upper_steps_o, "Override ef_upper_steps (-1: use config)");
    amap.arg("topk", topk_o, "Top-k neighbors to retrieve (-1: 10)");
    amap.arg("batch", batching, "Disable batching");
    amap.arg("lazy", lazy_evict, "Disable lazy eviction");
    amap.arg("f_latency", f_latency, "Save latency");
    amap.arg("f_accuracy", f_accuracy, "Save accuracy");
    amap.arg("f_comm", f_comm, "Save communication");
    amap.arg("f_csv", f_csv, "Append summary metrics to CSV");
    amap.arg("net", net_profile_arg, "Network profile for estimated latency: none|fast|slow or 1|2|3");
    amap.arg("pq_oram", use_pq_oram, "Store PQ codes in a second Ring ORAM (0/1)");
    amap.arg("server_metadata", simulate_server_metadata, "Simulate ORAM metadata as server-side state (0/1)");
    amap.arg("pq_oram_path", pq_oram_path_arg, "PQ index used for remote PQ-code ORAM");
    amap.arg("pq_codes_per_block", pq_codes_per_block_arg, "PQ codes per PQ-ORAM block (-1: auto-fit)");
    amap.arg("pq_oram_state_path", pq_oram_state_path_arg, "Persisted PQ ORAM server-state path (empty: derive from PQ path/config)");
    amap.arg("pm_oram", use_pm_oram, "Store Graph ORAM position map in a recursive PM Ring ORAM (0/1)");
    amap.arg("pm_entries_per_block", pm_entries_per_block_arg, "Graph position-map entries per PM-ORAM block");
    amap.arg("pm_oram_state_path", pm_oram_state_path_arg, "Persisted PM ORAM server-state path (empty: derive from dataset/config)");
    amap.arg("pq_metadata_client", pq_metadata_client, "Count PQ ORAM metadata as client-side storage (0/1)");
    amap.parse(argc, argv);

    const NetworkProfile net_profile = parse_network_profile(net_profile_arg);
    const size_t k = (topk_o >= 1) ? (size_t)topk_o : 10;

    cout << ">>> Setting up..." << endl;
    cout << "-> Role: " << party << endl;
    cout << "-> Address: " << address << endl;
    cout << "-> Port: " << port << endl;
    cout << "<<<" << endl << endl;

    bool isServer = party == 1;
    NetIO* io = new NetIO(isServer ? nullptr : address.c_str(), port);
    NetIO* bf_io = new NetIO(isServer ? nullptr : address.c_str(), port+1);
    NetIO* pq_io = nullptr;
    if (use_pq_oram) {
        pq_io = new NetIO(isServer ? nullptr : address.c_str(), port+2);
    }
    NetIO* pm_io = nullptr;
    if (use_pm_oram) {
        pm_io = new NetIO(isServer ? nullptr : address.c_str(), port+3);
    }

    Metadata md;
    
    cout << "-> Parsing json..." << endl;
    if(parseJson(config_path, md, dataset) != 0) {
        cerr << "Early exit..." << endl;
        return 0;
    }

     // ORAM for graph
    struct RingOramConfig config(
        md.block_size, 
        md.real_bucket_size, 
        md.dummy_size, 
        md.evict_rate, 
        md.base_size,
        md.num_levels,
        md.oram_cached_levels
    );

    OptNode::n_neighbors = 2*md.M;
    OptNode::dim = md.dim;

    Bucket::setMaxSize(1);
    SBucket::setCipherSize(compute_ctx_len((1 + md.block_size)*sizeof(int)));

    const string pq_index_path = pq_oram_path_arg.empty() ? md.pq_path : pq_oram_path_arg;
    faiss::ProductQuantizer* pq_for_oram = nullptr;
    RingOramConfig* pq_config = nullptr;
    PqOramLayout pq_layout;
    RemoteRing* pq_remote_storage = nullptr;
    string pq_oram_state_path;
    uint64_t pq_index_file_size = 0;
    uint64_t pq_index_file_mtime = 0;
    if (use_pq_oram) {
        pq_for_oram = faiss::read_pq(pq_index_path.c_str(), 0);
        pq_index_file_size = file_size_bytes(pq_index_path);
        pq_index_file_mtime = file_mtime_sec(pq_index_path);
        const int pq_num_vectors = (int)(pq_for_oram->codes.size() / pq_for_oram->code_size);
        const int pq_code_size = (int)pq_for_oram->code_size;
        const int pq_payload_bytes = md.block_size * (int)sizeof(int);
        if (pq_code_size > pq_payload_bytes) {
            cerr << "PQ code size " << pq_code_size
                 << " exceeds ORAM block payload " << pq_payload_bytes
                 << " bytes; cannot store one PQ code per ORAM block." << endl;
            return 1;
        }
        const int max_pq_codes_per_block = pq_payload_bytes / pq_code_size;
        const int pq_codes_per_block = pq_codes_per_block_arg > 0
                ? pq_codes_per_block_arg
                : max_pq_codes_per_block;
        if (pq_codes_per_block > max_pq_codes_per_block) {
            cerr << "pq_codes_per_block=" << pq_codes_per_block
                 << " exceeds max " << max_pq_codes_per_block
                 << " for block payload " << pq_payload_bytes
                 << " bytes and PQ code size " << pq_code_size << " bytes." << endl;
            return 1;
        }
        const int pq_num_code_blocks = ceil_div_int(pq_num_vectors, pq_codes_per_block);
        const int pq_num_levels = compute_ring_levels_for_blocks(pq_num_code_blocks, md.real_bucket_size);
        const int pq_cached_levels = std::min(md.oram_cached_levels, pq_num_levels - 1);
        pq_config = new RingOramConfig(
            md.block_size,
            md.real_bucket_size,
            md.dummy_size,
            md.evict_rate,
            pq_num_code_blocks,
            pq_num_levels,
            pq_cached_levels
        );
        pq_layout = build_pq_oram_layout(pq_num_vectors, pq_code_size, *pq_config, pq_codes_per_block);
        pq_remote_storage = new RemoteRing(pq_io, *pq_config, isServer, true, false);
        pq_oram_state_path = pq_oram_state_path_arg.empty()
                ? default_pq_oram_state_path(
                        pq_index_path,
                        *pq_config,
                        pq_code_size,
                        pq_layout.codes_per_oram_block,
                        pq_num_vectors)
                : pq_oram_state_path_arg;
        std::cout << "-> PQ ORAM: path=" << pq_index_path
                  << " code_size=" << pq_code_size
                  << " codes/block=" << pq_layout.codes_per_oram_block
                  << " max_codes/block=" << max_pq_codes_per_block
                  << " code_blocks=" << pq_layout.num_code_blocks
                  << " cached_levels=" << pq_cached_levels
                  << " state_path=" << pq_oram_state_path
                  << std::endl;
    }

    RingOramConfig* pm_config = nullptr;
    PmOramLayout pm_layout;
    RemoteRing* pm_remote_storage = nullptr;
    string pm_oram_state_path;
    uint64_t graph_pos_map_file_size = 0;
    uint64_t graph_pos_map_file_mtime = 0;
    if (use_pm_oram) {
        if (pm_entries_per_block_arg <= 0 || pm_entries_per_block_arg > md.block_size) {
            cerr << "pm_entries_per_block=" << pm_entries_per_block_arg
                 << " must be in [1, " << md.block_size << "] because the PM ORAM uses the graph ORAM block size."
                 << endl;
            return 1;
        }
        const int num_position_entries = read_position_map_entry_count(md.pos_map_path);
        if (num_position_entries <= 0) {
            cerr << "Could not read Graph ORAM position map entry count from "
                 << md.pos_map_path << endl;
            return 1;
        }
        graph_pos_map_file_size = file_size_bytes(md.pos_map_path);
        graph_pos_map_file_mtime = file_mtime_sec(md.pos_map_path);
        const int pm_num_blocks = ceil_div_int(num_position_entries, pm_entries_per_block_arg);
        const int pm_num_levels = compute_ring_levels_for_blocks(pm_num_blocks, md.real_bucket_size);
        const int pm_cached_levels = std::min(md.oram_cached_levels, pm_num_levels - 1);
        pm_config = new RingOramConfig(
                md.block_size,
                md.real_bucket_size,
                md.dummy_size,
                md.evict_rate,
                pm_num_blocks,
                pm_num_levels,
                pm_cached_levels);
        pm_layout = build_pm_oram_layout(
                num_position_entries,
                *pm_config,
                pm_entries_per_block_arg);
        pm_remote_storage = new RemoteRing(pm_io, *pm_config, isServer, true, false);
        pm_oram_state_path = pm_oram_state_path_arg.empty()
                ? default_pm_oram_state_path(
                        md,
                        *pm_config,
                        pm_layout.entries_per_oram_block,
                        pm_layout.num_position_entries)
                : pm_oram_state_path_arg;
        std::cout << "-> PM ORAM: graph_position_entries=" << pm_layout.num_position_entries
                  << " entries/block=" << pm_layout.entries_per_oram_block
                  << " position_blocks=" << pm_layout.num_position_blocks
                  << " cached_levels=" << pm_cached_levels
                  << " state_path=" << pm_oram_state_path
                  << std::endl;
    }

    // cout << "block_size: " << md.block_size << endl;
    // cout << "bucket_size: " << md.bucket_size << endl;
    // cout << "sbucket_size: " << SBucket::getCipherSize() << endl;

    // RemoteServerStorage* rss = new RemoteServerStorage(config.block_size, io, isServer, config.num_levels, md.integrity, config);

    RandForOramInterface* random = new RandomForOram();

    bool in_memory = true;
    RemoteRing* remote_storage = new RemoteRing(io, config, isServer, in_memory, md.integrity);

    long comm;
    long round;

    double t0 = elapsed();

    if(isServer){
        // Config remote storage server
        tprint("Starting remote server... \n", t0);
        
        bool in_memory = true;

        tprint("Faster initialization...\n", t0);

        remote_storage->load_server_state(md.buckets_path.c_str());
        if (use_pm_oram) {
            vector<unsigned char> pm_ciphertexts;
            if (read_pm_oram_state(
                    pm_oram_state_path,
                    *pm_config,
                    pm_layout,
                    graph_pos_map_file_size,
                    graph_pos_map_file_mtime,
                    pm_ciphertexts)) {
                tprint("Loading persisted PM ORAM server state: " + pm_oram_state_path + "\n", t0);
            } else {
                tprint("Building PM ORAM server state from Graph position map...\n", t0);
                vector<int> graph_position_entries = read_position_map_entries(md.pos_map_path);
                if ((int)graph_position_entries.size() != pm_layout.num_position_entries) {
                    cerr << "Failed to read complete Graph ORAM position map: "
                         << md.pos_map_path << endl;
                    return 1;
                }
                pm_ciphertexts = build_pm_oram_ciphertexts(
                        graph_position_entries,
                        *pm_config,
                        pm_layout);
                graph_position_entries.clear();
                graph_position_entries.shrink_to_fit();
                write_pm_oram_state(
                        pm_oram_state_path,
                        *pm_config,
                        pm_layout,
                        graph_pos_map_file_size,
                        graph_pos_map_file_mtime,
                        pm_ciphertexts);
                tprint("Persisted PM ORAM server state: " + pm_oram_state_path + "\n", t0);
            }
            pm_remote_storage->load_server_state_from_ciphertexts(pm_ciphertexts);
        }
        if (use_pq_oram) {
            vector<unsigned char> pq_ciphertexts;
            if (read_pq_oram_state(
                    pq_oram_state_path,
                    *pq_config,
                    pq_layout,
                    pq_index_file_size,
                    pq_index_file_mtime,
                    pq_ciphertexts)) {
                tprint("Loading persisted PQ-code ORAM server state: " + pq_oram_state_path + "\n", t0);
            } else {
                tprint("Building PQ-code ORAM server state...\n", t0);
                pq_ciphertexts = build_pq_oram_ciphertexts(pq_for_oram, *pq_config, pq_layout);
                write_pq_oram_state(
                        pq_oram_state_path,
                        *pq_config,
                        pq_layout,
                        pq_index_file_size,
                        pq_index_file_mtime,
                        pq_ciphertexts);
                tprint("Persisted PQ-code ORAM server state: " + pq_oram_state_path + "\n", t0);
            }
            pq_remote_storage->load_server_state_from_ciphertexts(pq_ciphertexts);
            pq_for_oram->codes.clear();
            pq_for_oram->codes.shrink_to_fit();
        }
        
        bool ready = true;
        bf_io->send_data(&ready, sizeof(bool));

        if(md.integrity){
            remote_storage->load_server_hash(md.hash_path.c_str());
            remote_storage->sync_roots();
        }

        tprint("Run server...\n", t0);

        comm = io->counter;
        round = io->num_rounds;
        long pq_comm = use_pq_oram ? pq_io->counter : 0;
        long pq_round = use_pq_oram ? pq_io->num_rounds : 0;
        long pm_comm = use_pm_oram ? pm_io->counter : 0;
        long pm_round = use_pm_oram ? pm_io->num_rounds : 0;
        
        std::thread pq_server_thread;
        if (use_pq_oram) {
            pq_server_thread = std::thread([&]() {
                pq_remote_storage->run_server();
            });
        }
        std::thread pm_server_thread;
        if (use_pm_oram) {
            pm_server_thread = std::thread([&]() {
                pm_remote_storage->run_server();
            });
        }
		remote_storage->run_server();
        if (use_pq_oram && pq_server_thread.joinable()) {
            pq_server_thread.join();
        }
        if (use_pm_oram && pm_server_thread.joinable()) {
            pm_server_thread.join();
        }

        {
            long server_send_bytes = (io->counter - comm)
                    + (use_pq_oram ? (pq_io->counter - pq_comm) : 0)
                    + (use_pm_oram ? (pm_io->counter - pm_comm) : 0);
            long server_send_rounds = (io->num_rounds - round)
                    + (use_pq_oram ? (pq_io->num_rounds - pq_round) : 0)
                    + (use_pm_oram ? (pm_io->num_rounds - pm_round) : 0);
            io->send_data(&server_send_bytes, sizeof(long));
            io->send_data(&server_send_rounds, sizeof(long));
        }
        
    } else{
        // Client
        tprint("Initializing ORAM... \n", t0);

        OramRing* graph_oram = new OramRing(
            remote_storage, 
            random, 
            config,
            md.num_levels,
            md.oram_cached_levels,
            batching,
            lazy_evict
        );
        OramInterface* oram = graph_oram;
        DeterministicRandForOram* pm_random = nullptr;
        OramRing* pm_position_oram = nullptr;
        PositionMapOramClient* pm_position_client = nullptr;
        if (use_pm_oram) {
            pm_random = new DeterministicRandForOram(0x7f4a7c159e3779b9ULL);
            pm_position_oram = new OramRing(
                    pm_remote_storage,
                    pm_random,
                    *pm_config,
                    pm_config->num_levels,
                    pm_config->cached_levels,
                    batching,
                    false);
            pm_position_oram->loadPositionMap(pm_layout.position_map);
            pm_position_oram->loadMetaDataFromBlockIds(pm_layout.metadata_block_ids);
            pm_position_client = new PositionMapOramClient(
                    graph_oram,
                    pm_position_oram,
                    md.block_size,
                    pm_layout.entries_per_oram_block,
                    pm_config->dummy_size);
        }
        DeterministicRandForOram* pq_random = nullptr;
        OramRing* pq_code_oram = nullptr;
        PQCodeOramClient* pq_code_client = nullptr;
        if (use_pq_oram) {
            pq_random = new DeterministicRandForOram();
            pq_code_oram = new OramRing(
                    pq_remote_storage,
                    pq_random,
                    *pq_config,
                    pq_config->num_levels,
                    pq_config->cached_levels,
                    batching,
                    lazy_evict);
            pq_code_oram->loadPositionMap(pq_layout.position_map);
            pq_code_oram->loadMetaDataFromBlockIds(pq_layout.metadata_block_ids);
            pq_code_client = new PQCodeOramClient(
                    pq_code_oram,
                    md.block_size,
                    pq_layout.code_size,
                    pq_layout.codes_per_oram_block,
                    pq_config->dummy_size);
        }

        tprint("Loading ORAM metadata\n", t0);
        ((OramRing*)oram)->loadMetaDataFromFile(md.metadata_path.c_str());

        tprint("Loading prebuilt index\n", t0);

        faiss::IndexHNSW* index = faiss::read_index(md.index_path.c_str(), 0);

        tprint("Loading pq index\n", t0);

        faiss::ProductQuantizer* pq = use_pq_oram
                ? pq_for_oram
                : faiss::read_pq(md.pq_path.c_str(), 0);
        pq->print_stats();
        if (use_pq_oram) {
            pq->code_fetcher = [pq_code_client](const vector<int>& ids, vector<vector<uint8_t>>& out) {
                pq_code_client->fetch_codes(ids, out);
            };
            pq->reset_code_oram_stats = [pq_code_oram]() {
                pq_code_oram->reset_query_access_stats();
            };
            pq->finish_code_oram_query = [pq_code_oram]() {
                pq_code_oram->evict_and_write_back();
            };
            pq->read_code_oram_stats = [pq_code_oram](
                    uint64_t& bmasr,
                    uint64_t& server_blocks,
                    uint64_t& early,
                    uint64_t& evict_effective,
                    double& bmasr_time,
                    double& evict_time) {
                bmasr = pq_code_oram->stat_bmasr_invocations;
                server_blocks = pq_code_oram->stat_server_cipher_block_reads;
                early = pq_code_oram->stat_early_reshuffle_invocations;
                evict_effective = pq_code_oram->stat_evict_effective;
                bmasr_time = pq_code_oram->stat_bmasr_time_sec;
                evict_time = pq_code_oram->stat_evict_time_sec;
            };
            pq->codes.clear();
            pq->codes.shrink_to_fit();
        }

        StorageProfile storage_profile;

        tprint("Clustering\n", t0);

        ObliviousBlockFetcherRing* obf = new ObliviousBlockFetcherRing(
            md.base_size, 
            index->hnsw.max_level + 1 ,
            oram
        );

        // bool tmp;
        // io->recv_data(&tmp, 1);        

        // Read client states
        {
            obf->read_block_mapping(md.block_map_path.c_str());

            // stash
            graph_oram->loadPositionMapFromFile(md.pos_map_path.c_str());
            if (use_pm_oram) {
                graph_oram->set_position_map_batch_hook(
                        [pm_position_client](
                                const vector<int>& block_indices,
                                const vector<int>& old_leaves,
                                const vector<int>& new_leaves) {
                            pm_position_client->access_positions(block_indices, old_leaves, new_leaves);
                        });
            }
            
            bool ready;
            bf_io->recv_data(&ready, sizeof(bool));

            if(md.integrity){
                remote_storage->sync_roots();
            }

            // Graph cache
            index->set_graph_cache(md.ef_lowest_cached_layer, obf, md.graph_cache_path.c_str());

            // ORAM cache
            graph_oram->init_cache_top();
        }

        storage_profile = build_storage_profile(
                md,
                pq,
                pq_config,
                pq_layout,
                pq_oram_state_path,
                use_pq_oram,
                pm_config,
                pm_layout,
                pm_oram_state_path,
                use_pm_oram);
        print_storage_profile(storage_profile, pq_oram_state_path, pm_oram_state_path, use_pm_oram);
        if (use_pm_oram) {
            std::cout << "PM ORAM recursion enabled: each Graph ORAM batch access triggers recursive PM ORAM access; "
                      << "Graph counters include the PM ORAM delta." << std::endl;
        }

        tprint("Loading queries\n", t0);
        size_t nq;
        float* xq;

        {
            size_t d2;
            xq = fvecs_read(md.query_path.c_str(), &d2, &nq);
            n = n == -1 ? nq : n;
            assert(md.dim == d2 || !"query does not have same dimension as train set");
            assert(n <= nq || !"# queries larger than query set");
        }

        tprint("Loading GT\n", t0);

        faiss::idx_t* gt; // nq * gt_size matrix of ground-truth nearest-neighbors
        size_t gt_size;

        {

            tprint("Loading ground truth for " + to_string(nq) + "queries\n", t0);

            // load ground-truth and convert int to long
            size_t nq2;
            int* gt_int = ivecs_read(md.gt_path.c_str(), &gt_size, &nq2);
            assert(nq2 == nq || !"incorrect nb of ground truth entries");
            // assert(k == k1 || !"incorrect k");

            gt = new faiss::idx_t[gt_size * nq];
            for (int i = 0; i < gt_size * nq; i++) {
                gt[i] = gt_int[i];
            }
            delete[] gt_int;
        }

        // bool tmp;
        // io->recv_data(&tmp, 1);

        nq = n;

        comm = io->counter;
        round = io->num_rounds;
        long pq_comm = use_pq_oram ? pq_io->counter : 0;
        long pq_round = use_pq_oram ? pq_io->num_rounds : 0;
        long pm_comm = use_pm_oram ? pm_io->counter : 0;
        long pm_round = use_pm_oram ? pm_io->num_rounds : 0;

        faiss::SearchParametersHNSW* params = new faiss::SearchParametersHNSW();
        params->efSpec = md.ef_spec;
        params->efNeighbor = md.ef_neighbor;
        params->efLayer = md.ef_lowest_cached_layer;
        params->efUpperSteps = md.ef_upper_steps;
        params->efSearch = md.ef_search;

        if(efn != -1){
            params->efNeighbor = efn;
        }

        if(efspec != -1){
            params->efSpec = efspec;
        }

        if(ef_search_o != -1){
            params->efSearch = ef_search_o;
        }

        if(ef_upper_steps_o != -1){
            params->efUpperSteps = ef_upper_steps_o;
        }
        
        // output buffers
        faiss::idx_t* I = new faiss::idx_t[nq * k];
        float* D = new float[nq * k];

        tprint("Searching with topk=" + to_string(k) + " efSearch: " + to_string(params->efSearch) + "\n", t0);

        double t1 = elapsed();

        faiss::SearchStats s_stats = index->oblivious_search(
            nq,
            xq,
            k,
            D,
            I,
            pq,
            params
        );


        unsigned long total = 0;
        for(int nrt : s_stats.nsteps_map[0]){
            total += nrt;
        }

        printf("Since t0: %.3f ms | Search: %.3f ms\n", (elapsed() - t0) * 1000.0, (elapsed() - t1) * 1000.0);

        const string graph_oram_label = use_pm_oram
                ? "[Graph ORAM + PM recursion]"
                : "[Graph ORAM]";
        if (!s_stats.oram_bmasr_invocations_per_query.empty()) {
            uint64_t sum_bma = 0, sum_real = 0, sum_pad = 0, sum_serv = 0;
            uint64_t sum_evict = 0, sum_evict_eff = 0, sum_early = 0;
            double sum_evict_t = 0.0, sum_bmasr_t = 0.0;
            for (size_t qi = 0; qi < s_stats.oram_bmasr_invocations_per_query.size(); ++qi) {
                sum_bma += s_stats.oram_bmasr_invocations_per_query[qi];
                sum_real += s_stats.oram_real_block_indices_per_query[qi];
                sum_pad += s_stats.oram_pad_slots_per_query[qi];
                sum_serv += s_stats.oram_server_cipher_block_reads_per_query[qi];
                sum_evict += s_stats.oram_evict_invocations_per_query[qi];
                sum_evict_eff += s_stats.oram_evict_effective_per_query[qi];
                sum_early += s_stats.oram_early_reshuffle_invocations_per_query[qi];
                sum_evict_t += s_stats.oram_evict_time_sec_per_query[qi];
                sum_bmasr_t += s_stats.oram_bmasr_time_sec_per_query[qi];
            }
            double inv = 1.0 / (double)nq;
            std::cout << graph_oram_label << " batch_multi_access_swap_ro calls / query (avg): " << (sum_bma * inv)
                      << "  (total " << sum_bma << ")" << std::endl;
            std::cout << graph_oram_label << " real logical block indices read / query (avg): " << (sum_real * inv)
                      << "  (total " << sum_real << ")" << std::endl;
            std::cout << graph_oram_label << " padded batch slots (incl. dummies) / query (avg): " << (sum_pad * inv)
                      << "  (total " << sum_pad << ")" << std::endl;
            std::cout << graph_oram_label << " ciphertext block reads from server / query (avg): " << (sum_serv * inv)
                      << "  (total " << sum_serv << ")" << std::endl;
            std::cout << graph_oram_label << " evict_and_write_back calls / query (avg): " << (sum_evict * inv)
                      << "  (total " << sum_evict << "; effective num_paths>0: " << sum_evict_eff << ")"
                      << std::endl;
            std::cout << graph_oram_label << " early_reshuffle calls / query (avg): " << (sum_early * inv)
                      << "  (total " << sum_early << ")" << std::endl;
            std::cout << graph_oram_label << " data RTT / query (avg): "
                      << ((sum_bma + 2 * sum_early + 2 * sum_evict_eff) * inv)
                      << "  (batch + 2*early_reshuffle + 2*effective_evict)" << std::endl;
            if (sum_evict > 0) {
                std::cout << graph_oram_label << " avg wall time per evict_and_write_back (all calls): "
                          << (sum_evict_t / (double)sum_evict) * 1000.0 << " ms" << std::endl;
            }
            if (sum_evict_eff > 0) {
                std::cout << graph_oram_label << " avg wall time per effective evict (num_paths>0 only): "
                          << (sum_evict_t / (double)sum_evict_eff) * 1000.0 << " ms" << std::endl;
            }
            if (sum_bma > 0) {
                std::cout << graph_oram_label << " avg wall time per batch_multi_access_swap_ro call: "
                          << (sum_bmasr_t / (double)sum_bma) * 1000.0 << " ms" << std::endl;
            }
        }
        if (!s_stats.pq_oram_bmasr_invocations_per_query.empty()) {
            const uint64_t sum_bma = std::accumulate(
                    s_stats.pq_oram_bmasr_invocations_per_query.begin(),
                    s_stats.pq_oram_bmasr_invocations_per_query.end(),
                    uint64_t{0});
            const uint64_t sum_serv = std::accumulate(
                    s_stats.pq_oram_server_cipher_block_reads_per_query.begin(),
                    s_stats.pq_oram_server_cipher_block_reads_per_query.end(),
                    uint64_t{0});
            const uint64_t sum_early = std::accumulate(
                    s_stats.pq_oram_early_reshuffle_invocations_per_query.begin(),
                    s_stats.pq_oram_early_reshuffle_invocations_per_query.end(),
                    uint64_t{0});
            const uint64_t sum_evict_eff = std::accumulate(
                    s_stats.pq_oram_evict_effective_per_query.begin(),
                    s_stats.pq_oram_evict_effective_per_query.end(),
                    uint64_t{0});
            const double inv = 1.0 / (double)nq;
            std::cout << "[PQ ORAM] batch_multi_access_swap_ro calls / query (avg): "
                      << (sum_bma * inv) << "  (total " << sum_bma << ")" << std::endl;
            std::cout << "[PQ ORAM] ciphertext block reads from server / query (avg): "
                      << (sum_serv * inv) << "  (total " << sum_serv << ")" << std::endl;
            std::cout << "[PQ ORAM] early_reshuffle calls / query (avg): "
                      << (sum_early * inv) << "  (total " << sum_early << ")" << std::endl;
            std::cout << "[PQ ORAM] effective evict calls / query (avg): "
                      << (sum_evict_eff * inv) << "  (total " << sum_evict_eff << ")" << std::endl;
            std::cout << "[PQ ORAM] data RTT / query (avg): "
                      << ((sum_bma + 2 * sum_early + 2 * sum_evict_eff) * inv)
                      << "  (batch + 2*early_reshuffle + 2*effective_evict)" << std::endl;
        }

        printf("[%.3f ms] Computing average round trip: %ld\n", (elapsed() - t0) * 1000.0, total / nq);

        printf("[%.3f ms] Compute recalls\n", (elapsed() - t0) * 1000.0);

        const float recall_at_k = compute_recall(
            gt, 
            gt_size,
            I,
            nq,
            k
        );

        int* result = new int[nq * k];

        for(int i = 0; i < nq * k; i++){
            result[i] = I[i];
        }

        if(f_accuracy != ""){
            // write result if search over the whole query set
            // string result_path = dataset + ".ivecs";
            ivecs_write(
                f_accuracy.c_str(), 
                result, 
                k,
                nq
            );
        }

        if(f_latency != ""){
            // string perceived_latency_path = "perceived_latency_" + dataset + ".bin";
            // string full_latency_path = "full_latency_" + dataset + ".bin";

            vector<float> latency = s_stats.perceived_latency;
            latency.insert(latency.end(), s_stats.full_latency.begin(), s_stats.full_latency.end());

            fvecs_write(
                f_latency.c_str(),
                latency.data(),
                latency.size(),
                1
            );
        }

        double perceived_latency_sum = std::accumulate(s_stats.perceived_latency.begin(), s_stats.perceived_latency.end(), 0.0);
        double avg_perceived_latency = perceived_latency_sum / n;

        double full_latency_sum = std::accumulate(s_stats.full_latency.begin(), s_stats.full_latency.end(), 0.0);
        double avg_full_latency = full_latency_sum / n;

        std::cout << "Avg. perceived latency: " << (avg_perceived_latency * 1000.0) << " ms" << std::endl;
        std::cout << "Avg. full latency: " << (avg_full_latency * 1000.0) << " ms" << std::endl;

        const int final_ef_search = params->efSearch;

        delete params;

        delete[] result;
        delete[] I;
        delete[] D;
        delete[] gt;
        

        remote_storage->close_server();
        if (use_pq_oram) {
            pq_remote_storage->close_server();
        }
        if (use_pm_oram) {
            pm_remote_storage->close_server();
        }

        long server_send_bytes = 0;
        long server_send_rounds = 0;
        io->recv_data(&server_send_bytes, sizeof(long));
        io->recv_data(&server_send_rounds, sizeof(long));
        const long client_send_bytes = (io->counter - comm)
                + (use_pq_oram ? (pq_io->counter - pq_comm) : 0)
                + (use_pm_oram ? (pm_io->counter - pm_comm) : 0);
        const long client_send_rounds = (io->num_rounds - round)
                + (use_pq_oram ? (pq_io->num_rounds - pq_round) : 0)
                + (use_pm_oram ? (pm_io->num_rounds - pm_round) : 0);
        const long total_send_bytes = client_send_bytes + server_send_bytes;
        const long total_rounds = client_send_rounds + server_send_rounds;
        const double total_mb =
                static_cast<double>(total_send_bytes) / (1024.0 * 1024.0);
        std::cout << "Total communication: " << std::fixed << std::setprecision(3) << total_mb
                  << " MB (client send " << client_send_bytes << " + server send " << server_send_bytes
                  << " bytes)" << std::endl;
        std::cout << "Round (sum): " << total_rounds << std::endl;

        const double query_count = static_cast<double>(nq);
        const double avg_communication_rounds_per_query =
                static_cast<double>(total_rounds) / query_count;
        const uint64_t graph_bma_count = std::accumulate(
                s_stats.oram_bmasr_invocations_per_query.begin(),
                s_stats.oram_bmasr_invocations_per_query.end(),
                uint64_t{0});
        const uint64_t graph_early_reshuffle_count = std::accumulate(
                s_stats.oram_early_reshuffle_invocations_per_query.begin(),
                s_stats.oram_early_reshuffle_invocations_per_query.end(),
                uint64_t{0});
        const uint64_t graph_effective_evict_count = std::accumulate(
                s_stats.oram_evict_effective_per_query.begin(),
                s_stats.oram_evict_effective_per_query.end(),
                uint64_t{0});
        const uint64_t pq_bma_count = std::accumulate(
                s_stats.pq_oram_bmasr_invocations_per_query.begin(),
                s_stats.pq_oram_bmasr_invocations_per_query.end(),
                uint64_t{0});
        const uint64_t pq_early_reshuffle_count = std::accumulate(
                s_stats.pq_oram_early_reshuffle_invocations_per_query.begin(),
                s_stats.pq_oram_early_reshuffle_invocations_per_query.end(),
                uint64_t{0});
        const uint64_t pq_effective_evict_count = std::accumulate(
                s_stats.pq_oram_evict_effective_per_query.begin(),
                s_stats.pq_oram_evict_effective_per_query.end(),
                uint64_t{0});
        const uint64_t semantic_bma_count = graph_bma_count + pq_bma_count;
        const uint64_t semantic_early_reshuffle_count =
                graph_early_reshuffle_count + pq_early_reshuffle_count;
        const uint64_t semantic_effective_evict_count =
                graph_effective_evict_count + pq_effective_evict_count;
        const uint64_t semantic_data_rtt_count =
                semantic_bma_count
                + 2 * semantic_early_reshuffle_count
                + 2 * semantic_effective_evict_count;
        const uint64_t metadata_rtt_count = simulate_server_metadata
                ? (semantic_bma_count
                   + semantic_early_reshuffle_count
                   + semantic_effective_evict_count)
                : 0;
        const double estimated_rtt_count =
                static_cast<double>(semantic_data_rtt_count + metadata_rtt_count);
        const double estimated_rtt_per_query = estimated_rtt_count / query_count;
        const double estimated_rtt_delay_ms = estimated_rtt_count * net_profile.rtt_ms;
        const double estimated_transfer_delay_ms =
                net_profile.bandwidth_bps > 0.0
                        ? static_cast<double>(total_send_bytes) * 8.0 / net_profile.bandwidth_bps * 1000.0
                        : 0.0;
        const double estimated_network_delay_ms =
                estimated_rtt_delay_ms + estimated_transfer_delay_ms;
        const double estimated_network_delay_per_query_ms =
                estimated_network_delay_ms / query_count;

        std::cout << "Network profile: " << net_profile.name;
        if (net_profile.name == "fast") {
            std::cout << " (3 Gbps, 1 ms RTT)";
        } else if (net_profile.name == "slow") {
            std::cout << " (400 Mbps, 80 ms RTT)";
        } else {
            std::cout << " (no added network delay)";
        }
        std::cout << std::endl;
        std::cout << "Avg. communication rounds / query (Round(sum)/n): "
                  << avg_communication_rounds_per_query << std::endl;
        std::cout << "RTT count (ORAM semantic): " << estimated_rtt_count
                  << " total, " << estimated_rtt_per_query << " / query" << std::endl;
        std::cout << "RTT components: data_batch_access=" << semantic_bma_count
                  << ", data_early_reshuffle*2=" << (2 * semantic_early_reshuffle_count)
                  << ", data_effective_evict*2=" << (2 * semantic_effective_evict_count)
                  << ", metadata_extra=" << metadata_rtt_count
                  << " (" << (use_pm_oram ? "graph+pm" : "graph")
                  << " access/early/evict=" << graph_bma_count << "/"
                  << graph_early_reshuffle_count << "/" << graph_effective_evict_count
                  << ", pq access/early/evict=" << pq_bma_count << "/"
                  << pq_early_reshuffle_count << "/" << pq_effective_evict_count << ")"
                  << std::endl;
        std::cout << "Estimated network delay: " << estimated_network_delay_ms
                  << " ms total, " << estimated_network_delay_per_query_ms
                  << " ms / query (RTT " << estimated_rtt_delay_ms
                  << " ms + transfer " << estimated_transfer_delay_ms << " ms)" << std::endl;
        std::cout << "Avg. perceived latency + estimated network: "
                  << (avg_perceived_latency * 1000.0 + estimated_network_delay_per_query_ms)
                  << " ms" << std::endl;
        std::cout << "Avg. full latency + estimated network: "
                  << (avg_full_latency * 1000.0 + estimated_network_delay_per_query_ms)
                  << " ms" << std::endl;

        if(f_csv != ""){
            const uint64_t graph_server_block_count = std::accumulate(
                    s_stats.oram_server_cipher_block_reads_per_query.begin(),
                    s_stats.oram_server_cipher_block_reads_per_query.end(),
                    uint64_t{0});
            const uint64_t pq_server_block_count = std::accumulate(
                    s_stats.pq_oram_server_cipher_block_reads_per_query.begin(),
                    s_stats.pq_oram_server_cipher_block_reads_per_query.end(),
                    uint64_t{0});
            const uint64_t semantic_server_block_count =
                    graph_server_block_count + pq_server_block_count;
            const double graph_bmasr_time_sec = std::accumulate(
                    s_stats.oram_bmasr_time_sec_per_query.begin(),
                    s_stats.oram_bmasr_time_sec_per_query.end(),
                    0.0);
            const double pq_bmasr_time_sec = std::accumulate(
                    s_stats.pq_oram_bmasr_time_sec_per_query.begin(),
                    s_stats.pq_oram_bmasr_time_sec_per_query.end(),
                    0.0);
            const double semantic_bmasr_time_sec =
                    graph_bmasr_time_sec + pq_bmasr_time_sec;
            const double graph_evict_time_sec = std::accumulate(
                    s_stats.oram_evict_time_sec_per_query.begin(),
                    s_stats.oram_evict_time_sec_per_query.end(),
                    0.0);
            const double pq_evict_time_sec = std::accumulate(
                    s_stats.pq_oram_evict_time_sec_per_query.begin(),
                    s_stats.pq_oram_evict_time_sec_per_query.end(),
                    0.0);
            const double semantic_evict_time_sec =
                    graph_evict_time_sec + pq_evict_time_sec;

            const double avg_query_time_ms =
                    avg_full_latency * 1000.0 + estimated_network_delay_per_query_ms;
            const double avg_oram_blocks_per_query =
                    static_cast<double>(semantic_server_block_count) / query_count;
            const double avg_oram_accesses_per_query =
                    static_cast<double>(semantic_bma_count) / query_count;
            const double network_delay_per_semantic_rtt_ms =
                    estimated_rtt_count > 0.0
                            ? estimated_network_delay_ms / estimated_rtt_count
                            : 0.0;
            const double access_network_rtt_units =
                    1.0 + (simulate_server_metadata ? 1.0 : 0.0);
            const double evict_network_rtt_units =
                    2.0 + (simulate_server_metadata ? 1.0 : 0.0);
            const double avg_oram_access_time_ms =
                    semantic_bma_count > 0
                            ? semantic_bmasr_time_sec / static_cast<double>(semantic_bma_count) * 1000.0
                                      + access_network_rtt_units * network_delay_per_semantic_rtt_ms
                            : 0.0;
            const double avg_effective_evicts_per_query =
                    static_cast<double>(semantic_effective_evict_count) / query_count;
            const double avg_effective_evict_time_ms =
                    semantic_effective_evict_count > 0
                            ? semantic_evict_time_sec / static_cast<double>(semantic_effective_evict_count) * 1000.0
                                      + evict_network_rtt_units * network_delay_per_semantic_rtt_ms
                            : 0.0;
            const double avg_communication_mb_per_query = total_mb / query_count;

            vector<pair<string, string>> csv_rows = {
                {"EF_SEARCH", std::to_string(final_ef_search)},
                {"R@" + std::to_string(k) + " recall", format_csv_double(recall_at_k)},
                {"Mean query latency/ms", format_csv_double(avg_query_time_ms)},
                {"Mean ORAM blocks/query", format_csv_double(avg_oram_blocks_per_query)},
                {"ORAM access calls/query", format_csv_double(avg_oram_accesses_per_query)},
                {"ORAM time/access call/ms", format_csv_double(avg_oram_access_time_ms)},
                {"ORAM evictions/query", format_csv_double(avg_effective_evicts_per_query)},
                {"ORAM time/eviction/ms", format_csv_double(avg_effective_evict_time_ms)},
                {"Communication/query/MB", format_csv_double(avg_communication_mb_per_query)},
                {"Total round trips/query", format_csv_double(estimated_rtt_per_query)},
                {"Total client storage/MB", format_csv_double(bytes_to_mb(storage_profile.client_total_bytes))},
                {"Total server storage/MB", format_csv_double(bytes_to_mb(storage_profile.server_total_bytes))}
            };
            if (!append_csv_column(f_csv, csv_rows)) {
                std::cerr << "Failed to append CSV result: " << f_csv << std::endl;
                return 1;
            }
            std::cout << "CSV result appended: " << f_csv << std::endl;
        }

        if(f_comm != ""){
            FILE* file = fopen(f_comm.c_str(), "w");
            if (file != nullptr) {
                fprintf(file, "%ld %ld\n", total_send_bytes, total_rounds);
                fclose(file);
            } else {
                perror("Error opening file");
            }
        }
    }

    return 0;
}
