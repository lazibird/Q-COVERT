#include "repository_paths.h"
#include "Block.h"
#include "Bucket.h"
#include "QuORAM.h"
#include "DoubleORAM.h"
#include "RandomForOram.h"
#include "RemoteRing.h"
#include "FuORAM.h"
#include "double_oram_adapter.h"
#include "evp.h"
#include "ivf_rabitq_index.h"
#include "matrix.h"
#include "oram_block_builders.h"
#include "private_ivf_rabitq.h"
#include "simple_json.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_set>
#include <vector>

#ifndef DIM
#define DIM 128
#endif

#ifndef BB
#define BB 128
#endif

#ifndef B_QUERY
#define B_QUERY 4
#endif

using namespace double_oram;

namespace {

constexpr uint32_t kPathsPerCluster = 2;
constexpr int kClusterCachedLevels = 4;
constexpr int kRingCachedLevels = 4;

using Index = IvfRabitQIndex<DIM, BB, B_QUERY>;
using ApproxLayout = ApproxOramBlockLayout<DIM, BB>;
using ExactLayout = ExactOramBlockLayout<DIM, BB>;
using RawExactLayout = RawUint8ExactOramBlockLayout<DIM, BB>;

struct Args {
    repository_paths::DatasetFiles files;
    std::string dataset = "sift";
    std::string source;
    std::string index_path;
    uint32_t clusters = 0;
    uint32_t initial_clusters = 0;
    uint32_t tau = 0;
    uint32_t k = 10;
    uint32_t nprobe = 100;
    std::vector<uint32_t> nprobes;
    std::vector<uint32_t> k2_values;
    uint32_t k2 = 0;
    uint32_t nqueries = 0;
    uint32_t path_batch_clusters = 4;
    int ring_z = 32;
    int ring_s = 58;
    int ring_a = 46;
    uint32_t seed = 20260623;
    std::string net_sim = "none";
    double net_bandwidth_mbps = -1.0;
    double net_rtt_ms = -1.0;
    bool net_no_sleep = true;
    std::string metadata_storage = "server";
    std::string exact_payload = "auto";
};

struct Totals {
    double query_time_sec = 0.0;
    double cluster_access_time_sec = 0.0;
    double cluster_evict_time_sec = 0.0;
    double cluster_evict_effective_time_sec = 0.0;
    double ring_access_time_sec = 0.0;
    double ring_evict_time_sec = 0.0;
    double ring_evict_effective_time_sec = 0.0;
    double ann_centroid_select_time_sec = 0.0;
    double ann_path_read_time_sec = 0.0;
    double ann_approx_scan_time_sec = 0.0;
    double ann_candidate_select_time_sec = 0.0;
    double ann_ring_read_time_sec = 0.0;
    double ann_exact_compute_time_sec = 0.0;
    double ann_path_finish_time_sec = 0.0;
    double ann_ring_finish_time_sec = 0.0;
    double recall = 0.0;
    uint64_t cluster_accesses = 0;
    uint64_t cluster_evict_calls = 0;
    uint64_t cluster_evicts = 0;
    uint64_t ring_accesses = 0;
    uint64_t ring_evict_calls = 0;
    uint64_t ring_evicts = 0;
    uint64_t cluster_comm_bytes = 0;
    uint64_t ring_comm_bytes = 0;
    uint64_t cluster_access_net_round_trips = 0;
    uint64_t ring_access_net_round_trips = 0;
    uint64_t oram_evict_net_round_trips = 0;
    uint64_t approximate_distance_count = 0;
    uint64_t exact_distance_count = 0;
    uint64_t max_cluster_stash_blocks_after_evict = 0;
    uint64_t max_ring_stash_blocks_after_evict = 0;
};

struct CachePaths {
    std::string directory;
    std::string prefix;
    std::string cluster_server;
    std::string cluster_metadata;
    std::string ring_server;
    std::string ring_metadata;
};

std::string sanitize_for_filename(std::string value);
std::string lower_ascii(std::string value);
bool use_raw_uint8_exact_payload(const Args& args);
ExactPayloadFormat exact_payload_format_for_args(const Args& args);
size_t exact_payload_bytes_for_args(const Args& args);
std::string exact_payload_format_tag(const Args& args);
bool progress_line_active = false;
std::size_t progress_line_width = 0;

void finish_progress_line() {
    if (progress_line_active) {
        std::cout << std::endl;
        progress_line_active = false;
        progress_line_width = 0;
    }
}

void log_step(const std::string& message) {
    finish_progress_line();
    std::cout << "[status] " << message << std::endl;
}

void log_progress(const std::string& message, bool done = false) {
    const std::string line = "[status] " + message;
    std::cout << '\r' << line;
    if (progress_line_width > line.size()) {
        std::cout << std::string(progress_line_width - line.size(), ' ');
    }
    std::cout << std::flush;

    progress_line_active = !done;
    progress_line_width = done ? 0 : line.size();
    if (done) {
        std::cout << std::endl;
    }
}

void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--dataset sift] [--source ../data/sift]"
              << " [--clusters dataset-default] [--index path]"
              << " [--k 10] [--nprobe 100] [--nprobes 20,50,100] [--k2 60,150,250]"
              << " [--path-batch-clusters 0=nprobe] [--tau value]"
              << " [--nqueries 0] [--ring-z 32] [--ring-s 58] [--ring-a 46]"
              << " [--seed 20260623]"
              << " [--net-sim none|fast|slow|custom]"
              << " [--net-bandwidth-mbps value] [--net-rtt-ms value] [--net-no-sleep]"
              << " [--metadata-storage server|client]"
              << " [--exact-payload auto|raw_float32|raw-uint8]"
              << " [--config config.json] [--no-config]\n";
}

uint32_t default_clusters_for_dataset(std::string dataset) {
    std::transform(dataset.begin(), dataset.end(), dataset.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    if (dataset == "sift1b") {
        return 1u << 16;
    }
    if (dataset == "sift100m") {
        return 1u << 16;
    }
    if (dataset == "sift10m") {
        return 4096;
    }
    if (dataset == "deep1m") {
        return 4096;
    }
    if (dataset == "siftsmall") {
        return 48;
    }
    return 4096;
}

bool path_exists_for_arg_parse(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in.good();
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string assignment_tag_for_paths(const std::string& assignment) {
    return "assign" + sanitize_for_filename(assignment);
}

std::string assignment_manifest_path(
        const std::string& source,
        const std::string& dataset,
        uint32_t requested_clusters) {
    return source + dataset + "_assignment_" + std::to_string(requested_clusters) + ".txt";
}

uint32_t read_manifest_clusters(
        const std::string& source,
        const std::string& dataset,
        uint32_t requested_clusters) {
    std::ifstream in(assignment_manifest_path(source, dataset, requested_clusters));
    if (!in) {
        return 0;
    }
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream iss(line);
        std::string key;
        uint32_t value = 0;
        if (iss >> key >> value && key == "actual_clusters") {
            return value;
        }
    }
    return 0;
}

std::string index_path_for_clusters(
        const std::string& source,
        uint32_t clusters) {
    return source + "ivfrabitq" + std::to_string(clusters) +
           "_B" + std::to_string(BB) + ".index";
}

uint32_t resolve_index_clusters(
        const std::string& source,
        const std::string& dataset,
        uint32_t requested_clusters) {
    const uint32_t manifest_clusters =
        read_manifest_clusters(source, dataset, requested_clusters);
    if (manifest_clusters != 0 &&
            path_exists_for_arg_parse(index_path_for_clusters(source, manifest_clusters))) {
        return manifest_clusters;
    }
    if (path_exists_for_arg_parse(index_path_for_clusters(source, requested_clusters))) {
        return requested_clusters;
    }

    std::vector<uint32_t> candidates;
    DIR* dir = opendir(source.c_str());
    if (dir != nullptr) {
        const std::string prefix = "ivfrabitq";
        const std::string suffix = "_B" + std::to_string(BB) + ".index";
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (!starts_with(name, prefix) || !ends_with(name, suffix)) {
                continue;
            }
            const std::string cluster_text =
                name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
            if (cluster_text.empty() ||
                    !std::all_of(cluster_text.begin(), cluster_text.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
                continue;
            }
            candidates.push_back(static_cast<uint32_t>(std::stoul(cluster_text)));
        }
        closedir(dir);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1) {
        return candidates.front();
    }
    if (!candidates.empty()) {
        throw std::runtime_error("multiple matching index cluster counts found; pass the actual --clusters value");
    }
    return requested_clusters;
}

std::vector<uint32_t> parse_uint32_list(std::string text) {
    for (char& ch : text) {
        if (ch == ',' || ch == ';' || ch == ':') {
            ch = ' ';
        }
    }
    std::istringstream iss(text);
    std::vector<uint32_t> values;
    std::string token;
    while (iss >> token) {
        size_t consumed = 0;
        const unsigned long parsed = std::stoul(token, &consumed);
        if (consumed != token.size() ||
                parsed == 0 ||
                parsed > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("invalid positive uint32 list value: " + token);
        }
        values.push_back(static_cast<uint32_t>(parsed));
    }
    if (values.empty()) {
        throw std::invalid_argument("nprobe list cannot be empty");
    }
    return values;
}

std::string join_uint32_values(const std::vector<uint32_t>& values) {
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); i++) {
        if (i != 0) {
            oss << ",";
        }
        oss << values[i];
    }
    return oss.str();
}

std::string normalize_config_key(std::string key) {
    while (key.size() >= 2 && key[0] == '-' && key[1] == '-') {
        key.erase(key.begin(), key.begin() + 2);
    }
    for (char& ch : key) {
        if (ch == '-') {
            ch = '_';
        }
    }
    return key;
}

std::string config_string(const simple_json::Value& value, const std::string& path) {
    return simple_json::as_string(value, path);
}

uint32_t config_uint32(const simple_json::Value& value, const std::string& path) {
    const double number = simple_json::as_number(value, path);
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
            std::floor(number) != number) {
        throw std::runtime_error("config value must be a non-negative uint32: " + path);
    }
    return static_cast<uint32_t>(number);
}

int config_int(const simple_json::Value& value, const std::string& path) {
    const double number = simple_json::as_number(value, path);
    if (number < static_cast<double>(std::numeric_limits<int>::min()) ||
            number > static_cast<double>(std::numeric_limits<int>::max()) ||
            std::floor(number) != number) {
        throw std::runtime_error("config value must be an int: " + path);
    }
    return static_cast<int>(number);
}

double config_double(const simple_json::Value& value, const std::string& path) {
    return simple_json::as_number(value, path);
}

bool config_bool(const simple_json::Value& value, const std::string& path) {
    return simple_json::as_bool(value, path);
}

std::vector<uint32_t> config_uint32_list(const simple_json::Value& value, const std::string& path) {
    if (value.is_array()) {
        std::vector<uint32_t> out;
        out.reserve(value.array_value.size());
        for (size_t i = 0; i < value.array_value.size(); i++) {
            const std::string item_path = path + "[" + std::to_string(i) + "]";
            const uint32_t parsed = config_uint32(value.array_value[i], item_path);
            if (parsed == 0) {
                throw std::runtime_error("config list value must be positive: " + item_path);
            }
            out.push_back(parsed);
        }
        if (out.empty()) {
            throw std::runtime_error("config list cannot be empty: " + path);
        }
        return out;
    }
    if (value.is_string()) {
        return parse_uint32_list(simple_json::as_string(value, path));
    }
    const uint32_t single = config_uint32(value, path);
    if (single == 0) {
        throw std::runtime_error("config value must be positive: " + path);
    }
    return {single};
}

bool cli_provided(const std::set<std::string>& provided, const std::string& key) {
    return provided.find(normalize_config_key(key)) != provided.end();
}

void apply_query_config_section(
        Args& args,
        const simple_json::Value* section,
        const std::set<std::string>& provided,
        const std::string& path) {
    if (section == nullptr) {
        return;
    }
    if (!section->is_object()) {
        throw std::runtime_error("config section must be an object: " + path);
    }
    for (const auto& item : section->object_value) {
        const std::string key = normalize_config_key(item.first);
        const simple_json::Value& value = item.second;
        const std::string value_path = path + "." + item.first;
        if (key == "files") {
            args.files.configure(value);
        } else if (key == "source" && !cli_provided(provided, "source")) {
            args.source = config_string(value, value_path);
        } else if ((key == "index" || key == "index_path") && !cli_provided(provided, "index")) {
            args.index_path = config_string(value, value_path);
        } else if (key == "clusters" && !cli_provided(provided, "clusters")) {
            args.clusters = config_uint32(value, value_path);
        } else if (key == "tau" && !cli_provided(provided, "tau")) {
            args.tau = config_uint32(value, value_path);
        } else if (key == "k" && !cli_provided(provided, "k")) {
            args.k = config_uint32(value, value_path);
        } else if (key == "nprobe" &&
                   !cli_provided(provided, "nprobe") &&
                   !cli_provided(provided, "nprobes")) {
            args.nprobe = config_uint32(value, value_path);
            args.nprobes.clear();
        } else if (key == "nprobes" &&
                   !cli_provided(provided, "nprobe") &&
                   !cli_provided(provided, "nprobes")) {
            args.nprobes = config_uint32_list(value, value_path);
            args.nprobe = args.nprobes.front();
        } else if (key == "k2" && !cli_provided(provided, "k2")) {
            args.k2_values = config_uint32_list(value, value_path);
        } else if (key == "nqueries" && !cli_provided(provided, "nqueries")) {
            args.nqueries = config_uint32(value, value_path);
        } else if (key == "path_batch_clusters" && !cli_provided(provided, "path_batch_clusters")) {
            args.path_batch_clusters = config_uint32(value, value_path);
        } else if (key == "ring_z" && !cli_provided(provided, "ring_z")) {
            args.ring_z = config_int(value, value_path);
        } else if (key == "ring_s" && !cli_provided(provided, "ring_s")) {
            args.ring_s = config_int(value, value_path);
        } else if (key == "ring_a" && !cli_provided(provided, "ring_a")) {
            args.ring_a = config_int(value, value_path);
        } else if (key == "seed" && !cli_provided(provided, "seed")) {
            args.seed = config_uint32(value, value_path);
        } else if (key == "net_sim" && !cli_provided(provided, "net_sim")) {
            args.net_sim = lower_ascii(config_string(value, value_path));
        } else if (key == "net_bandwidth_mbps" && !cli_provided(provided, "net_bandwidth_mbps")) {
            args.net_bandwidth_mbps = config_double(value, value_path);
        } else if (key == "net_rtt_ms" && !cli_provided(provided, "net_rtt_ms")) {
            args.net_rtt_ms = config_double(value, value_path);
        } else if (key == "net_no_sleep" && !cli_provided(provided, "net_no_sleep")) {
            args.net_no_sleep = config_bool(value, value_path);
        } else if (key == "metadata_storage" && !cli_provided(provided, "metadata_storage")) {
            args.metadata_storage = lower_ascii(config_string(value, value_path));
        } else if (key == "exact_payload" && !cli_provided(provided, "exact_payload")) {
            args.exact_payload = lower_ascii(config_string(value, value_path));
        }
    }
}

void apply_query_config(
        Args& args,
        const simple_json::Value& root,
        const std::set<std::string>& provided) {
    if (!root.is_object()) {
        throw std::runtime_error("config root must be a JSON object");
    }
    if (!cli_provided(provided, "dataset")) {
        if (const simple_json::Value* default_dataset = root.get("default_dataset")) {
            args.dataset = config_string(*default_dataset, "default_dataset");
        }
    }

    if (!cli_provided(provided, "tau")) {
        if (const auto* cap = simple_json::get_path(root, {"defaults", "preprocess", "size_max"}))
            args.tau = config_uint32(*cap, "defaults.preprocess.size_max");
        if (const auto* cap = simple_json::get_path(root, {"datasets", args.dataset, "preprocess", "size_max"}))
            args.tau = config_uint32(*cap, "datasets." + args.dataset + ".preprocess.size_max");
    }
    if (const simple_json::Value* defaults_query = simple_json::get_path(root, {"defaults", "query"})) {
        apply_query_config_section(args, defaults_query, provided, "defaults.query");
    }

    const simple_json::Value* dataset_section =
        simple_json::get_path(root, {"datasets", args.dataset});
    if (dataset_section != nullptr) {
        apply_query_config_section(args, dataset_section, provided, "datasets." + args.dataset);
        if (const simple_json::Value* query_section = dataset_section->get("query")) {
            apply_query_config_section(args, query_section, provided, "datasets." + args.dataset + ".query");
        }
    }
}

Args parse_args(int argc, char** argv) {
    Args args;
    std::string config_path = repository_paths::default_config();
    bool no_config = false;
    bool explicit_config = false;
    std::set<std::string> provided;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (key == "--dataset") {
            provided.insert("dataset");
            args.dataset = need_value("--dataset");
        } else if (key == "--source") {
            provided.insert("source");
            args.source = need_value("--source");
        } else if (key == "--clusters") {
            provided.insert("clusters");
            args.clusters = static_cast<uint32_t>(std::stoul(need_value("--clusters")));
        } else if (key == "--tau") {
            provided.insert("tau");
            args.tau = static_cast<uint32_t>(std::stoul(need_value("--tau")));
        } else if (key == "--index") {
            provided.insert("index");
            args.index_path = need_value("--index");
        } else if (key == "--k") {
            provided.insert("k");
            args.k = static_cast<uint32_t>(std::stoul(need_value("--k")));
        } else if (key == "--k2") {
            provided.insert("k2");
            args.k2_values = parse_uint32_list(need_value("--k2"));
        } else if (key == "--nprobe") {
            provided.insert("nprobe");
            args.nprobe = static_cast<uint32_t>(std::stoul(need_value("--nprobe")));
        } else if (key == "--nprobes" || key == "--nprobe-list") {
            provided.insert("nprobes");
            args.nprobes = parse_uint32_list(need_value(key.c_str()));
        } else if (key == "--k2-ratio") {
            throw std::runtime_error("--k2-ratio is no longer accepted; pass per-nprobe values with --k2");
        } else if (key == "--nqueries") {
            provided.insert("nqueries");
            args.nqueries = static_cast<uint32_t>(std::stoul(need_value("--nqueries")));
        } else if (key == "--path-batch-clusters") {
            provided.insert("path_batch_clusters");
            args.path_batch_clusters = static_cast<uint32_t>(std::stoul(need_value("--path-batch-clusters")));
        } else if (key == "--ring-z") {
            provided.insert("ring_z");
            args.ring_z = std::stoi(need_value("--ring-z"));
        } else if (key == "--ring-s") {
            provided.insert("ring_s");
            args.ring_s = std::stoi(need_value("--ring-s"));
        } else if (key == "--ring-a") {
            provided.insert("ring_a");
            args.ring_a = std::stoi(need_value("--ring-a"));
        } else if (key == "--seed") {
            provided.insert("seed");
            args.seed = static_cast<uint32_t>(std::stoul(need_value("--seed")));
        } else if (key == "--net-sim") {
            provided.insert("net_sim");
            args.net_sim = lower_ascii(need_value("--net-sim"));
        } else if (key == "--net-bandwidth-mbps") {
            provided.insert("net_bandwidth_mbps");
            args.net_bandwidth_mbps = std::stod(need_value("--net-bandwidth-mbps"));
        } else if (key == "--net-rtt-ms") {
            provided.insert("net_rtt_ms");
            args.net_rtt_ms = std::stod(need_value("--net-rtt-ms"));
        } else if (key == "--net-no-sleep") {
            provided.insert("net_no_sleep");
            args.net_no_sleep = true;
        } else if (key == "--metadata-storage") {
            provided.insert("metadata_storage");
            args.metadata_storage = lower_ascii(need_value("--metadata-storage"));
        } else if (key == "--exact-payload") {
            provided.insert("exact_payload");
            args.exact_payload = lower_ascii(need_value("--exact-payload"));
        } else if (key == "--config") {
            config_path = need_value("--config");
            explicit_config = true;
        } else if (key == "--no-config") {
            no_config = true;
        } else if (key == "--help" || key == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + key);
        }
    }

    if (!no_config) {
        if (simple_json::file_exists(config_path)) {
            const simple_json::Value config = simple_json::parse_file(config_path);
            apply_query_config(args, config, provided);
            repository_paths::resolve_config_source(args.source, config_path, provided.count("source") != 0);
        } else if (explicit_config) {
            throw std::runtime_error("config file does not exist: " + config_path);
        }
    }

    if (args.source.empty()) {
        args.source = repository_paths::dataset(args.dataset);
    }
    if (!args.source.empty() && args.source.back() != '/') {
        args.source.push_back('/');
    }
    if (args.clusters == 0) {
        args.clusters = default_clusters_for_dataset(args.dataset);
    }
    if (args.nprobes.empty()) {
        args.nprobes.push_back(args.nprobe);
    } else {
        args.nprobe = args.nprobes.front();
    }
    if (args.clusters == 0 || args.k == 0 || args.nprobe == 0) {
        throw std::invalid_argument("clusters, k, and nprobe must be positive");
    }
    if (args.k2_values.empty() || args.k2_values.size() != args.nprobes.size()) {
        throw std::invalid_argument("--k2 requires one positive value per nprobe");
    }
    for (uint32_t value : args.k2_values) {
        if (value == 0) throw std::invalid_argument("k2 values must be positive");
    }
    for (uint32_t value : args.nprobes) {
        if (value == 0) throw std::invalid_argument("nprobe values must be positive");
    }
    args.initial_clusters = args.clusters;
    if (args.index_path.empty()) {
        const uint32_t actual_clusters = resolve_index_clusters(
            args.source,
            args.dataset,
            args.clusters);
        args.clusters = actual_clusters;
        args.index_path = index_path_for_clusters(args.source, args.clusters);
    }
    if (args.ring_z <= 0 || args.ring_s <= 0 || args.ring_a <= 0) {
        throw std::invalid_argument("ring-z, ring-s, and ring-a must be positive");
    }
    if (args.ring_z + args.ring_s > 128) {
        throw std::invalid_argument("ring-z + ring-s cannot exceed FuORAM metadata capacity 128");
    }
    if (args.net_sim != "none" && args.net_sim != "fast" &&
            args.net_sim != "slow" && args.net_sim != "custom") {
        throw std::invalid_argument("--net-sim must be none, fast, slow, or custom");
    }
    if (args.net_bandwidth_mbps != -1.0 && args.net_bandwidth_mbps <= 0.0) {
        throw std::invalid_argument("--net-bandwidth-mbps must be positive when provided");
    }
    if (args.net_rtt_ms != -1.0 && args.net_rtt_ms < 0.0) {
        throw std::invalid_argument("--net-rtt-ms must be non-negative when provided");
    }
    if (args.metadata_storage != "server" && args.metadata_storage != "client") {
        throw std::invalid_argument("--metadata-storage must be server or client");
    }
    std::replace(args.exact_payload.begin(), args.exact_payload.end(), '-', '_');
    if (args.exact_payload != "auto" &&
            args.exact_payload != "raw_float32" &&
            args.exact_payload != "raw_uint8") {
        throw std::invalid_argument("--exact-payload must be auto, raw_float32, or raw-uint8");
    }
    return args;
}

double elapsed_sec_since(std::chrono::high_resolution_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
}

double bytes_to_mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double sec_to_ms(double seconds) {
    return seconds * 1000.0;
}

double avg_ms_per_call(double seconds, uint64_t calls) {
    if (calls == 0) {
        return 0.0;
    }
    return sec_to_ms(seconds) / static_cast<double>(calls);
}

NetworkProfile network_profile_from_args(const Args& args) {
    NetworkProfile profile;
    const std::string mode = lower_ascii(args.net_sim);
    if (mode == "none") {
        return profile;
    }

    profile.enabled = true;
    profile.sleep = !args.net_no_sleep;
    if (mode == "fast") {
        profile.bandwidth_mbps = 3000.0;
        profile.rtt_ms = 1.0;
    } else if (mode == "slow") {
        profile.bandwidth_mbps = 400.0;
        profile.rtt_ms = 80.0;
    } else if (mode == "custom") {
        if (args.net_bandwidth_mbps <= 0.0 || args.net_rtt_ms < 0.0) {
            throw std::invalid_argument(
                "--net-sim custom requires --net-bandwidth-mbps > 0 and --net-rtt-ms >= 0");
        }
        profile.bandwidth_mbps = args.net_bandwidth_mbps;
        profile.rtt_ms = args.net_rtt_ms;
    } else {
        throw std::invalid_argument("--net-sim must be none, fast, slow, or custom");
    }

    if (mode != "custom") {
        if (args.net_bandwidth_mbps > 0.0) {
            profile.bandwidth_mbps = args.net_bandwidth_mbps;
        }
        if (args.net_rtt_ms >= 0.0) {
            profile.rtt_ms = args.net_rtt_ms;
        }
    }
    if (profile.bandwidth_mbps <= 0.0 || profile.rtt_ms < 0.0) {
        throw std::invalid_argument("network simulation requires positive bandwidth and non-negative RTT");
    }
    return profile;
}

std::string network_profile_label(const Args& args, const NetworkProfile& profile) {
    if (!profile.enabled) {
        return "none";
    }
    std::ostringstream oss;
    oss << lower_ascii(args.net_sim)
        << " bandwidth_mbps " << profile.bandwidth_mbps
        << " rtt_ms " << profile.rtt_ms
        << " sleep " << (profile.sleep ? 1 : 0);
    return oss.str();
}

bool file_exists(const std::string& path);

uint64_t int_block_cipher_bytes(int block_size) {
    return static_cast<uint64_t>(compute_ctx_len((1 + block_size) * sizeof(int)));
}

uint64_t ring_tree_storage_bytes(const RingOramConfig& config) {
    return static_cast<uint64_t>(config.num_buckets) *
           static_cast<uint64_t>(config.bucket_size) *
           int_block_cipher_bytes(config.block_size);
}

uint64_t cluster_metadata_storage_bytes(const QuORAM& cluster_oram) {
    const uint64_t slot_bytes =
        static_cast<uint64_t>(3 * sizeof(int) + sizeof(bool));
    const uint64_t slot_metadata_bytes =
        static_cast<uint64_t>(cluster_oram.num_buckets) *
        static_cast<uint64_t>(cluster_oram.bucket_size) *
        slot_bytes;
    const uint64_t bucket_count_bytes =
        static_cast<uint64_t>(cluster_oram.num_buckets) * sizeof(int);
    return slot_metadata_bytes + bucket_count_bytes;
}

uint64_t ring_metadata_storage_bytes(const FuORAM& ring_oram) {
    const uint64_t valids_bytes = ring_oram.metadata.empty()
        ? 16
        : static_cast<uint64_t>(sizeof(ring_oram.metadata.front().valids));
    const uint64_t bucket_bytes =
        static_cast<uint64_t>(sizeof(uint8_t)) +
        valids_bytes +
        static_cast<uint64_t>(ring_oram.bucket_size) * 2 * sizeof(int);
    return static_cast<uint64_t>(ring_oram.num_buckets) * bucket_bytes;
}

void print_persistent_storage_stats(
        const Index& index,
        const QuORAM& cluster_oram,
        const FuORAM& ring_oram,
        const RingOramConfig& cluster_config,
        const RingOramConfig& ring_config,
        bool metadata_on_server) {
    const uint64_t client_centroids_bytes =
        static_cast<uint64_t>(index.randomized_centroids.size()) * sizeof(float);
    const uint64_t client_cluster_counters_bytes =
        static_cast<uint64_t>(cluster_oram.cluster_counters.size()) * sizeof(uint64_t);
    const uint64_t client_oram_control_counters_bytes =
        static_cast<uint64_t>(2 * sizeof(int) + 2 * sizeof(int));
    const uint64_t client_cluster_vector_counts_bytes =
        static_cast<uint64_t>(index.len.size()) * sizeof(uint32_t);
    const uint64_t client_cluster_block_counts_bytes =
        static_cast<uint64_t>(cluster_oram.cluster_block_counts.size()) * sizeof(int);
    const uint64_t client_cluster_sizes_bytes =
        client_cluster_vector_counts_bytes + client_cluster_block_counts_bytes;
    const uint64_t total_cluster_metadata_bytes = cluster_metadata_storage_bytes(cluster_oram);
    const uint64_t total_ring_metadata_bytes = ring_metadata_storage_bytes(ring_oram);
    const uint64_t client_cluster_metadata_bytes =
        metadata_on_server ? 0 : total_cluster_metadata_bytes;
    const uint64_t client_ring_metadata_bytes =
        metadata_on_server ? 0 : total_ring_metadata_bytes;
    const uint64_t client_metadata_bytes =
        client_cluster_metadata_bytes + client_ring_metadata_bytes;
    const uint64_t client_total_bytes =
        client_centroids_bytes +
        client_cluster_counters_bytes +
        client_oram_control_counters_bytes +
        client_cluster_sizes_bytes +
        client_metadata_bytes;

    const uint64_t server_cluster_tree_bytes = ring_tree_storage_bytes(cluster_config);
    const uint64_t server_ring_tree_bytes = ring_tree_storage_bytes(ring_config);
    const uint64_t server_cluster_metadata_bytes =
        metadata_on_server ? total_cluster_metadata_bytes : 0;
    const uint64_t server_ring_metadata_bytes =
        metadata_on_server ? total_ring_metadata_bytes : 0;
    const uint64_t server_oram_tree_bytes =
        server_cluster_tree_bytes + server_ring_tree_bytes;
    const uint64_t server_metadata_bytes =
        server_cluster_metadata_bytes + server_ring_metadata_bytes;
    const uint64_t server_total_bytes =
        server_oram_tree_bytes + server_metadata_bytes;

    std::cout << "persistent_storage_begin\n";
    std::cout << "persistent_metadata_storage "
              << (metadata_on_server ? "server" : "client") << "\n";
    std::cout << "persistent_client_centroids_bytes " << client_centroids_bytes << "\n";
    std::cout << "persistent_client_cluster_counters_bytes " << client_cluster_counters_bytes << "\n";
    std::cout << "persistent_client_oram_control_counters_bytes " << client_oram_control_counters_bytes << "\n";
    std::cout << "persistent_client_cluster_vector_counts_bytes " << client_cluster_vector_counts_bytes << "\n";
    std::cout << "persistent_client_cluster_block_counts_bytes " << client_cluster_block_counts_bytes << "\n";
    std::cout << "persistent_client_cluster_sizes_bytes " << client_cluster_sizes_bytes << "\n";
    std::cout << "persistent_client_cluster_metadata_bytes " << client_cluster_metadata_bytes << "\n";
    std::cout << "persistent_client_ring_metadata_bytes " << client_ring_metadata_bytes << "\n";
    std::cout << "persistent_client_metadata_bytes " << client_metadata_bytes << "\n";
    std::cout << "persistent_client_total_bytes " << client_total_bytes << "\n";
    std::cout << "persistent_client_total_mib " << bytes_to_mib(client_total_bytes) << "\n";
    std::cout << "persistent_server_cluster_oram_tree_bytes " << server_cluster_tree_bytes << "\n";
    std::cout << "persistent_server_ring_oram_tree_bytes " << server_ring_tree_bytes << "\n";
    std::cout << "persistent_server_oram_trees_bytes " << server_oram_tree_bytes << "\n";
    std::cout << "persistent_server_cluster_metadata_bytes " << server_cluster_metadata_bytes << "\n";
    std::cout << "persistent_server_ring_metadata_bytes " << server_ring_metadata_bytes << "\n";
    std::cout << "persistent_server_metadata_bytes " << server_metadata_bytes << "\n";
    std::cout << "persistent_server_total_bytes " << server_total_bytes << "\n";
    std::cout << "persistent_server_total_mib " << bytes_to_mib(server_total_bytes) << "\n";
    std::cout << "persistent_storage_end\n";
}

std::string make_result_csv_path(const Args& args) {
    std::ostringstream path;
    path << "results/";
    path << sanitize_for_filename(args.dataset)
         << "_k" << args.k
         << "_net" << sanitize_for_filename(args.net_sim)
         << ".csv";
    return path.str();
}

void append_result_csv_row(
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& values) {
    const bool write_header = !file_exists(path);
    if (!write_header) {
        std::ifstream in(path);
        std::string first_line;
        std::getline(in, first_line);
        if (first_line.empty() || first_line.find("property") == 0) {
            throw std::runtime_error("result CSV is empty or uses the legacy property-column format; move it aside before running: " + path);
        }
    }
    std::ofstream out(path, std::ios::app);
    if (!out) throw std::runtime_error("cannot write result CSV: " + path);
    if (write_header) {
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << ",";
            out << values[i].first;
        }
        out << "\n";
    }
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ",";
        out << values[i].second;
    }
    out << "\n";
    if (!out) throw std::runtime_error("failed writing result CSV: " + path);
}

void write_result_csv(
        const std::string& path,
        const Args& args,
        const Totals& totals,
        uint32_t nq,
        uint64_t total_comm_bytes) {
    auto format_double = [](double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << value;
        return oss.str();
    };

    std::vector<std::pair<std::string, std::string>> values;
    values.reserve(32);
    values.push_back({"nprobe", std::to_string(args.nprobe)});
    values.push_back({"k2", std::to_string(args.k2)});
    values.push_back({"queries", std::to_string(nq)});
    values.push_back({"QuORAM real bucket size", std::to_string(args.ring_z)});
    values.push_back({"QuORAM dummy size", std::to_string(args.ring_s)});
    values.push_back({"QuORAM evict rate", std::to_string(args.ring_a)});
    values.push_back({"FuORAM evict rate", std::to_string(args.ring_a)});
    values.push_back({"metadata storage", args.metadata_storage});
    values.push_back({"Top" + std::to_string(args.k) + " recall", format_double(totals.recall / nq)});
    values.push_back({"Mean query latency/ms", format_double(sec_to_ms(totals.query_time_sec) / nq)});
    values.push_back({"QuORAM access time/query/ms", format_double(sec_to_ms(totals.cluster_access_time_sec) / nq)});
    values.push_back({"QuORAM writeback time/query/ms", format_double(sec_to_ms(totals.cluster_evict_time_sec) / nq)});
    values.push_back({"FuORAM access time/query/ms", format_double(sec_to_ms(totals.ring_access_time_sec) / nq)});
    values.push_back({"FuORAM writeback time/query/ms", format_double(sec_to_ms(totals.ring_evict_time_sec) / nq)});
    values.push_back({"QuORAM access calls/query", format_double(static_cast<double>(totals.cluster_accesses) / nq)});
    values.push_back({"QuORAM writebacks/query", format_double(static_cast<double>(totals.cluster_evicts) / nq)});
    values.push_back({"QuORAM time/access call/ms", format_double(avg_ms_per_call(totals.cluster_access_time_sec, totals.cluster_accesses))});
    values.push_back({"QuORAM time/writeback/ms", format_double(avg_ms_per_call(totals.cluster_evict_effective_time_sec, totals.cluster_evicts))});
    values.push_back({"FuORAM access calls/query", format_double(static_cast<double>(totals.ring_accesses) / nq)});
    values.push_back({"FuORAM writebacks/query", format_double(static_cast<double>(totals.ring_evicts) / nq)});
    values.push_back({"FuORAM time/access call/ms", format_double(avg_ms_per_call(totals.ring_access_time_sec, totals.ring_accesses))});
    values.push_back({"FuORAM time/writeback/ms", format_double(avg_ms_per_call(totals.ring_evict_effective_time_sec, totals.ring_evicts))});
    values.push_back({"Communication/query/MB", format_double(static_cast<double>(total_comm_bytes) / nq / (1000.0 * 1000.0))});
    values.push_back({"QuORAM peak stash after eviction/blocks", std::to_string(totals.max_cluster_stash_blocks_after_evict)});
    values.push_back({"FuORAM peak stash after eviction/blocks", std::to_string(totals.max_ring_stash_blocks_after_evict)});
    const uint64_t total_net_round_trips =
        totals.cluster_access_net_round_trips +
        totals.ring_access_net_round_trips +
        totals.oram_evict_net_round_trips;
    values.push_back({"Total round trips/query", format_double(static_cast<double>(total_net_round_trips) / nq)});
    values.push_back({"QuORAM access round trips/query", format_double(static_cast<double>(totals.cluster_access_net_round_trips) / nq)});
    values.push_back({"FuORAM access round trips/query", format_double(static_cast<double>(totals.ring_access_net_round_trips) / nq)});
    values.push_back({"ORAM eviction round trips/query", format_double(static_cast<double>(totals.oram_evict_net_round_trips) / nq)});
    append_result_csv_row(path, values);
}

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void ensure_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            throw std::runtime_error("cache path exists but is not a directory: " + path);
        }
        return;
    }
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("failed to create directory: " + path);
    }
}

std::string sanitize_for_filename(std::string value) {
    for (char& ch : value) {
        const bool ok =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_';
        if (!ok) {
            ch = '_';
        }
    }
    return value;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool use_raw_uint8_exact_payload(const Args& args) {
    std::string payload = lower_ascii(args.exact_payload);
    std::replace(payload.begin(), payload.end(), '-', '_');
    if (payload == "raw_uint8") {
        return true;
    }
    if (payload == "raw_float32") {
        return false;
    }
    const std::string dataset = lower_ascii(args.dataset);
    return dataset == "sift" || dataset == "siftsmall" || dataset == "sift1b" || dataset == "sift10m" || dataset == "sift100m";
}

ExactPayloadFormat exact_payload_format_for_args(const Args& args) {
    return use_raw_uint8_exact_payload(args)
        ? ExactPayloadFormat::RawUint8
        : ExactPayloadFormat::RawFloat32;
}

size_t exact_payload_bytes_for_args(const Args& args) {
    return use_raw_uint8_exact_payload(args)
        ? RawExactLayout::kPlainBytes
        : ExactLayout::kPlainBytes;
}

std::string exact_payload_format_tag(const Args& args) {
    return use_raw_uint8_exact_payload(args) ? "rawu8" : "rawf32";
}

int ann_path_block_size() {
    return 2 + 2 * static_cast<int>(Index::kBlockVectors) +
           packed_byte_int_count(static_cast<int>(ApproxLayout::kPlainBytes));
}

int total_path_blocks(const Index& index) {
    size_t total = 0;
    for (uint32_t c = 0; c < index.num_clusters; ++c) {
        total += index.cluster_num_blocks(c);
    }
    return static_cast<int>(total);
}

int choose_ring_levels_with_load_slack(int num_blocks, int real_bucket_size) {
    if (num_blocks <= 0) {
        throw std::invalid_argument("num_blocks must be positive");
    }
    if (real_bucket_size <= 0) {
        throw std::invalid_argument("real_bucket_size must be positive");
    }
    int levels = 2;
    auto real_capacity = [&]() -> long long {
        return ((1LL << levels) - 1) * static_cast<long long>(real_bucket_size);
    };
    while (real_capacity() < num_blocks) {
        levels++;
    }
    if (static_cast<long long>(num_blocks) * 10 > real_capacity() * 6) {
        levels++;
    }
    return levels;
}

int choose_cluster_levels(int num_blocks, int real_bucket_size) {
    return choose_ring_levels_with_load_slack(num_blocks, real_bucket_size);
}

Block* make_block(int block_size, int index, const std::vector<int>& data) {
    Block* block = new Block(block_size);
    block->index = index;
    block->data = data;
    return block;
}

std::map<int, std::vector<Block*>> make_dummy_buckets(int num_buckets, int bucket_size, int block_size) {
    std::map<int, std::vector<Block*>> buckets;
    for (int pos = 0; pos < num_buckets; pos++) {
        std::vector<Block*> bucket;
        bucket.reserve(bucket_size);
        for (int i = 0; i < bucket_size; i++) {
            bucket.push_back(new Block(block_size));
        }
        buckets[pos] = bucket;
    }
    return buckets;
}

template <typename VectorT>
void release_vector_storage(VectorT& values) {
    VectorT().swap(values);
}

void release_written_bucket_map_storage(std::map<int, std::vector<Block*>>& buckets) {
    // WriteBucketBatchMapAsBlockRing owns and deletes the Block* entries during
    // the write. After that only the map/vector containers remain here.
    std::map<int, std::vector<Block*>>().swap(buckets);
}

std::vector<int> bytes_to_block_data(int block_size, const std::vector<uint8_t>& bytes) {
    std::vector<int> packed = pack_bytes_to_ints(bytes);
    if (static_cast<int>(packed.size()) > block_size) {
        throw std::runtime_error("payload is larger than ORAM block size");
    }

    std::vector<int> data(block_size, 0);
    for (size_t i = 0; i < packed.size(); i++) {
        data[i] = packed[i];
    }
    return data;
}

std::vector<int> make_ann_path_payload(
        int block_size,
        uint32_t cluster_id,
        uint32_t block_id,
        const std::vector<uint32_t>& vector_ids,
        const std::vector<int>& latest_ring_leaf,
        const std::vector<uint8_t>& approx_payload) {
    std::vector<int> data(block_size, 0);
    data[0] = static_cast<int>(cluster_id);
    data[1] = static_cast<int>(block_id);

    for (uint32_t slot = 0; slot < Index::kBlockVectors; slot++) {
        uint32_t vector_id = slot < vector_ids.size() ? vector_ids[slot] : static_cast<uint32_t>(-1);
        if (vector_id == static_cast<uint32_t>(-1)) {
            data[2 + slot] = -1;
            data[2 + Index::kBlockVectors + slot] = -1;
        } else {
            data[2 + slot] = static_cast<int>(vector_id);
            data[2 + Index::kBlockVectors + slot] = latest_ring_leaf.at(vector_id);
        }
    }

    const int payload_offset = 2 + 2 * static_cast<int>(Index::kBlockVectors);
    std::vector<int> packed_payload = pack_bytes_to_ints(approx_payload);
    for (size_t i = 0; i < packed_payload.size(); i++) {
        data[payload_offset + static_cast<int>(i)] = packed_payload[i];
    }
    return data;
}

std::vector<std::vector<uint8_t>> build_exact_payloads(
        const Index& index,
        ExactPayloadFormat payload_format) {
    std::vector<std::vector<uint8_t>> exact_payloads(index.n);
    const bool raw_uint8 = payload_format == ExactPayloadFormat::RawUint8;
    for (uint32_t c = 0; c < index.num_clusters; c++) {
        log_progress(std::string("building exact ") + (raw_uint8 ? "raw uint8" : "raw float32") +
                     " payloads cluster " + std::to_string(c + 1) +
                     " / " + std::to_string(index.num_clusters));
        for (uint32_t local_id = 0; local_id < index.len[c]; local_id++) {
            uint32_t vector_id = index.ids[index.start[c] + local_id];
            if (raw_uint8) {
                exact_payloads[vector_id] = build_raw_uint8_exact_oram_payload(index, c, local_id);
            } else {
                exact_payloads[vector_id] =
                    build_exact_oram_payload(index, c, local_id);
            }
        }
    }
    log_progress(std::string("building exact ") + (raw_uint8 ? "raw uint8" : "raw float32") +
                 " payloads complete: " +
                 std::to_string(index.num_clusters) + " clusters", true);
    return exact_payloads;
}

bool try_place_ring_block(
        FuORAM& ring_oram,
        std::map<int, std::vector<Block*>>& buckets,
        std::vector<int>& used,
        std::vector<int>& latest_ring_leaf,
        uint32_t vector_id,
        int leaf,
        const std::vector<std::vector<uint8_t>>& exact_payloads,
        int ring_block_size,
        int real_bucket_size) {
    for (int level = ring_oram.getNumLevels() - 1; level >= ring_oram.num_cached_levels; level--) {
        int pos = ring_oram.P(leaf, level);
        if (used[pos] < real_bucket_size) {
            int slot = used[pos]++;
            delete buckets[pos][slot];
            buckets[pos][slot] = make_block(
                ring_block_size,
                static_cast<int>(vector_id),
                bytes_to_block_data(ring_block_size, exact_payloads[vector_id]));
            latest_ring_leaf[vector_id] = leaf;
            return true;
        }
    }
    return false;
}

void place_ring_blocks(
        FuORAM& ring_oram,
        std::map<int, std::vector<Block*>>& buckets,
        std::vector<int>& latest_ring_leaf,
        const std::vector<std::vector<uint8_t>>& exact_payloads,
        int ring_block_size,
        int real_bucket_size,
        std::mt19937& rng) {
    std::uniform_int_distribution<int> leaf_dist(0, ring_oram.getNumLeaves() - 1);
    std::vector<int> used(ring_oram.getNumBuckets(), 0);

    for (uint32_t vector_id = 0; vector_id < exact_payloads.size(); vector_id++) {
        bool placed = false;
        for (int attempt = 0; attempt < 1000 && !placed; attempt++) {
            placed = try_place_ring_block(
                ring_oram,
                buckets,
                used,
                latest_ring_leaf,
                vector_id,
                leaf_dist(rng),
                exact_payloads,
                ring_block_size,
                real_bucket_size);
        }
        for (int leaf = 0; leaf < ring_oram.getNumLeaves() && !placed; leaf++) {
            placed = try_place_ring_block(
                ring_oram,
                buckets,
                used,
                latest_ring_leaf,
                vector_id,
                leaf,
                exact_payloads,
                ring_block_size,
                real_bucket_size);
        }
        if (!placed) {
            throw std::runtime_error("failed to place ANN FuORAM vector block");
        }
    }
}

void install_ring_metadata(
        FuORAM& ring_oram,
        const std::map<int, std::vector<Block*>>& buckets,
        const std::vector<int>& latest_ring_leaf) {
    for (int pos = 0; pos < ring_oram.getNumBuckets(); pos++) {
        ring_oram.metadata[pos].count = 0;
        ring_oram.metadata[pos].valids.set();
        ring_oram.metadata[pos].block_ids.assign(ring_oram.bucket_size, -1);
        ring_oram.metadata[pos].block_leaves.assign(ring_oram.bucket_size, -1);
        int slot = 0;
        for (Block* block : buckets.at(pos)) {
            ring_oram.metadata[pos].block_ids[slot] = block->index;
            if (block->index >= 0) {
                ring_oram.metadata[pos].block_leaves[slot] = latest_ring_leaf.at(block->index);
            }
            slot++;
        }
    }
}

CachePaths make_cache_paths(
        const Args& args,
        const Index& index,
        int cluster_block_size,
        int ring_block_size,
        int cluster_real_bucket_size,
        int cluster_dummy_size,
        int cluster_num_blocks,
        int cluster_num_levels,
        int cluster_num_buckets,
        int cluster_cached_levels,
        int ring_num_levels) {
    CachePaths paths;
    paths.directory = args.source + "double_oram_cache";
    std::ostringstream prefix;
    prefix << sanitize_for_filename(args.dataset)
           << "_n" << index.n
           << "_c" << args.clusters
           << "_dim" << DIM
           << "_bb" << BB
           << "_bq" << B_QUERY
           << "_clusterbs" << cluster_block_size
           << "_clusterz" << cluster_real_bucket_size
           << "_clusters" << cluster_dummy_size
           << "_clusternb" << cluster_num_blocks
           << "_clusterlev" << cluster_num_levels
           << "_clusterbuckets" << cluster_num_buckets
           << "_clustercache" << cluster_cached_levels
           << "_ppc" << kPathsPerCluster
           << "_rawexact_v1"
           << "_exactfmt" << exact_payload_format_tag(args)
           << "_ringbs" << ring_block_size
           << "_rz" << args.ring_z
           << "_rs" << args.ring_s
           << "_ra" << args.ring_a
           << "_rlev" << ring_num_levels
           << "_rcache" << kRingCachedLevels
           << "_rleafmeta1"
           << "_seed" << args.seed;
    paths.prefix = paths.directory + "/" + prefix.str();
    paths.cluster_server = paths.prefix + ".cluster.bin";
    paths.cluster_metadata = paths.prefix + ".clustermeta.bin";
    paths.ring_server = paths.prefix + ".ring.bin";
    paths.ring_metadata = paths.prefix + ".ringmeta.bin";
    return paths;
}

std::string make_exact_payload_cache_path(
        const Args& args,
        const Index& index,
        size_t exact_payload_bytes) {
    std::ostringstream prefix;
    prefix << args.source << "double_oram_cache/"
           << sanitize_for_filename(args.dataset)
           << "_n" << index.n
           << "_c" << args.clusters
           << "_dim" << DIM
           << "_bb" << BB
           << "_bq" << B_QUERY
           << "_rawexact_v1"
           << "_exactfmt" << exact_payload_format_tag(args)
           << "_exactbytes" << exact_payload_bytes
           << "." << exact_payload_format_tag(args) << ".bin";
    return prefix.str();
}


bool cache_files_exist(const CachePaths& paths) {
    return file_exists(paths.cluster_server) &&
           file_exists(paths.cluster_metadata) &&
           file_exists(paths.ring_server) &&
           file_exists(paths.ring_metadata);
}

void save_exact_payloads(
        const std::vector<std::vector<uint8_t>>& exact_payloads,
        const std::string& path,
        size_t expected_payload_bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write exact payload cache: " + path);
    }

    const uint64_t count = static_cast<uint64_t>(exact_payloads.size());
    const uint64_t payload_bytes = static_cast<uint64_t>(expected_payload_bytes);
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    out.write(reinterpret_cast<const char*>(&payload_bytes), sizeof(payload_bytes));
    for (const std::vector<uint8_t>& payload : exact_payloads) {
        if (payload.size() != expected_payload_bytes) {
            throw std::runtime_error("exact payload has unexpected size while saving cache");
        }
        out.write(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
    }
    if (!out) {
        throw std::runtime_error("failed writing exact payload cache: " + path);
    }
}

std::vector<std::vector<uint8_t>> load_exact_payloads(
        uint32_t expected_count,
        const std::string& path,
        size_t expected_payload_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read exact payload cache: " + path);
    }

    uint64_t count = 0;
    uint64_t payload_bytes = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    in.read(reinterpret_cast<char*>(&payload_bytes), sizeof(payload_bytes));
    if (!in ||
            count != expected_count ||
            payload_bytes != static_cast<uint64_t>(expected_payload_bytes)) {
        throw std::runtime_error("exact payload cache does not match current dataset/index");
    }

    std::vector<std::vector<uint8_t>> exact_payloads(count);
    for (std::vector<uint8_t>& payload : exact_payloads) {
        payload.assign(static_cast<size_t>(payload_bytes), 0);
        in.read(
            reinterpret_cast<char*>(payload.data()),
            static_cast<std::streamsize>(payload.size()));
    }
    if (!in) {
        throw std::runtime_error("short read in exact payload cache: " + path);
    }
    return exact_payloads;
}

void save_ring_metadata(const FuORAM& ring_oram, const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write FuORAM metadata cache: " + path);
    }

    const int num_buckets = ring_oram.num_buckets;
    const int bucket_size = ring_oram.bucket_size;
    const uint32_t magic = kRingMetadataCacheMagic;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    out.write(reinterpret_cast<const char*>(&num_buckets), sizeof(num_buckets));
    out.write(reinterpret_cast<const char*>(&bucket_size), sizeof(bucket_size));
    for (int pos = 0; pos < num_buckets; pos++) {
        if (static_cast<int>(ring_oram.metadata[pos].block_ids.size()) != bucket_size ||
                static_cast<int>(ring_oram.metadata[pos].block_leaves.size()) != bucket_size) {
            throw std::runtime_error("FuORAM metadata has unexpected bucket size");
        }
        out.write(
            reinterpret_cast<const char*>(ring_oram.metadata[pos].block_ids.data()),
            static_cast<std::streamsize>(bucket_size * sizeof(int)));
        out.write(
            reinterpret_cast<const char*>(ring_oram.metadata[pos].block_leaves.data()),
            static_cast<std::streamsize>(bucket_size * sizeof(int)));
    }
    if (!out) {
        throw std::runtime_error("failed writing FuORAM metadata cache: " + path);
    }
}

void load_ring_metadata(FuORAM& ring_oram, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read FuORAM metadata cache: " + path);
    }

    uint32_t magic = 0;
    int num_buckets = 0;
    int bucket_size = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    in.read(reinterpret_cast<char*>(&num_buckets), sizeof(num_buckets));
    in.read(reinterpret_cast<char*>(&bucket_size), sizeof(bucket_size));
    if (!in ||
            magic != kRingMetadataCacheMagic ||
            num_buckets != ring_oram.num_buckets ||
            bucket_size != ring_oram.bucket_size) {
        throw std::runtime_error("FuORAM metadata cache does not match current parameters");
    }

    for (int pos = 0; pos < num_buckets; pos++) {
        ring_oram.metadata[pos].count = 0;
        ring_oram.metadata[pos].valids.set();
        ring_oram.metadata[pos].block_ids.assign(bucket_size, -1);
        ring_oram.metadata[pos].block_leaves.assign(bucket_size, -1);
        in.read(
            reinterpret_cast<char*>(ring_oram.metadata[pos].block_ids.data()),
            static_cast<std::streamsize>(bucket_size * sizeof(int)));
        in.read(
            reinterpret_cast<char*>(ring_oram.metadata[pos].block_leaves.data()),
            static_cast<std::streamsize>(bucket_size * sizeof(int)));
    }
    if (!in) {
        throw std::runtime_error("short read in FuORAM metadata cache: " + path);
    }
}

void save_cluster_metadata(const QuORAM& cluster_oram, const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write QuORAM metadata cache: " + path);
    }

    const int num_buckets = cluster_oram.num_buckets;
    const int bucket_size = cluster_oram.bucket_size;
    out.write(reinterpret_cast<const char*>(&num_buckets), sizeof(num_buckets));
    out.write(reinterpret_cast<const char*>(&bucket_size), sizeof(bucket_size));
    for (int pos = 0; pos < num_buckets; pos++) {
        for (int slot = 0; slot < bucket_size; slot++) {
            const QuORAMSlotMeta& meta = cluster_oram.metadata[pos][slot];
            out.write(reinterpret_cast<const char*>(&meta.cluster_id), sizeof(meta.cluster_id));
            out.write(reinterpret_cast<const char*>(&meta.cluster_offset), sizeof(meta.cluster_offset));
            out.write(reinterpret_cast<const char*>(&meta.leaf), sizeof(meta.leaf));
            out.write(reinterpret_cast<const char*>(&meta.valid), sizeof(meta.valid));
        }
    }
    if (!out) {
        throw std::runtime_error("failed writing QuORAM metadata cache: " + path);
    }
}

void load_cluster_metadata(QuORAM& cluster_oram, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read QuORAM metadata cache: " + path);
    }

    int num_buckets = 0;
    int bucket_size = 0;
    in.read(reinterpret_cast<char*>(&num_buckets), sizeof(num_buckets));
    in.read(reinterpret_cast<char*>(&bucket_size), sizeof(bucket_size));
    if (!in || num_buckets != cluster_oram.num_buckets || bucket_size != cluster_oram.bucket_size) {
        throw std::runtime_error("QuORAM metadata cache does not match current parameters");
    }

    const std::streamoff header_bytes = static_cast<std::streamoff>(2 * sizeof(int));
    const std::streamoff slot_count = static_cast<std::streamoff>(num_buckets) * bucket_size;
    const std::streamoff old_slot_bytes = static_cast<std::streamoff>(2 * sizeof(int) + sizeof(bool));
    const std::streamoff new_slot_bytes = static_cast<std::streamoff>(3 * sizeof(int) + sizeof(bool));
    in.seekg(0, std::ios::end);
    const std::streamoff file_bytes = in.tellg();
    const bool old_format = file_bytes == header_bytes + slot_count * old_slot_bytes;
    const bool new_format = file_bytes == header_bytes + slot_count * new_slot_bytes;
    if (!old_format && !new_format) {
        throw std::runtime_error("QuORAM metadata cache has an unknown format");
    }
    in.seekg(header_bytes, std::ios::beg);

    for (int pos = 0; pos < num_buckets; pos++) {
        for (int slot = 0; slot < bucket_size; slot++) {
            QuORAMSlotMeta& meta = cluster_oram.metadata[pos][slot];
            in.read(reinterpret_cast<char*>(&meta.cluster_id), sizeof(meta.cluster_id));
            in.read(reinterpret_cast<char*>(&meta.cluster_offset), sizeof(meta.cluster_offset));
            if (new_format) {
                in.read(reinterpret_cast<char*>(&meta.leaf), sizeof(meta.leaf));
            } else if (meta.cluster_id >= 0 && meta.cluster_offset >= 0) {
                meta.leaf = cluster_oram.cluster_leaf(meta.cluster_id, 0, meta.cluster_offset);
            } else {
                meta.leaf = -1;
            }
            in.read(reinterpret_cast<char*>(&meta.valid), sizeof(meta.valid));
        }
    }
    if (!in) {
        throw std::runtime_error("short read in QuORAM metadata cache: " + path);
    }
}

std::vector<uint32_t> block_vector_ids(const Index& index, uint32_t cluster_id, uint32_t block_id) {
    std::vector<uint32_t> vector_ids(Index::kBlockVectors, static_cast<uint32_t>(-1));
    const uint32_t block_begin = block_id * Index::kBlockVectors;
    const uint32_t active =
        block_begin >= index.len[cluster_id]
            ? 0
            : std::min<uint32_t>(Index::kBlockVectors, index.len[cluster_id] - block_begin);
    for (uint32_t j = 0; j < active; j++) {
        vector_ids[j] = index.ids[index.start[cluster_id] + block_begin + j];
    }
    return vector_ids;
}

void place_cluster_block(
        QuORAM& cluster_oram,
        std::map<int, std::vector<Block*>>& buckets,
        std::vector<int>& used,
        int block_size,
        uint32_t logical_address,
        uint32_t cluster_id,
        uint32_t block_id,
        const std::vector<uint32_t>& vector_ids,
        const std::vector<int>& latest_ring_leaf,
        const std::vector<uint8_t>& approx_payload) {
    const int leaf = cluster_oram.cluster_leaf(static_cast<int>(cluster_id), 0, static_cast<int>(block_id));
    std::vector<int> data = make_ann_path_payload(
        block_size,
        cluster_id,
        block_id,
        vector_ids,
        latest_ring_leaf,
        approx_payload);

    for (int level = cluster_oram.getNumLevels() - 1; level >= cluster_oram.num_cached_levels; level--) {
        int pos = cluster_oram.P(leaf, level);
        if (used[pos] < cluster_oram.real_bucket_size) {
            int slot = used[pos]++;
            delete buckets[pos][slot];
            buckets[pos][slot] = make_block(block_size, static_cast<int>(logical_address), data);
            cluster_oram.metadata[pos][slot].cluster_id = static_cast<int>(cluster_id);
            cluster_oram.metadata[pos][slot].cluster_offset = static_cast<int>(block_id);
            cluster_oram.metadata[pos][slot].leaf = leaf;
            cluster_oram.metadata[pos][slot].valid = true;
            return;
        }
    }

    throw std::runtime_error("failed to place ANN QuORAM approx block");
}

float recall_at_k(ResultHeap result, const Matrix<uint32_t>& gt, size_t qi, uint32_t k) {
    const uint32_t denom = std::min<uint32_t>(k, static_cast<uint32_t>(gt.d));
    if (denom == 0) {
        return 0.0f;
    }

    std::unordered_set<uint32_t> expected;
    for (uint32_t i = 0; i < denom; i++) {
        expected.insert(gt.row(qi)[i]);
    }

    uint32_t hit = 0;
    while (!result.empty()) {
        if (expected.find(result.top().second) != expected.end()) {
            hit++;
        }
        result.pop();
    }
    return static_cast<float>(hit) / static_cast<float>(denom);
}

} // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);

        log_step("loading IVF-RaBitQ index: " + args.index_path);
        Index index;
        index.load(args.index_path);
        if (index.num_clusters == 0 || args.initial_clusters == 0) {
            throw std::runtime_error("CO index has no clusters or initial cluster count is missing");
        }
        for (uint32_t nprobe : args.nprobes) {
            if (nprobe > index.num_clusters) {
                throw std::invalid_argument("nprobe exceeds the number of clusters");
            }
        }
        if (args.tau == 0) {
            // Match preprocess.py: ceil(1.2 * ceil(N / n0c)), rounded up to b=32.
            const uint64_t base = (static_cast<uint64_t>(index.n) + args.initial_clusters - 1) / args.initial_clusters;
            const uint64_t scaled = (base * 12 + 9) / 10;
            const uint64_t rounded = ((scaled + Index::kBlockVectors - 1) / Index::kBlockVectors) * Index::kBlockVectors;
            if (rounded > std::numeric_limits<uint32_t>::max()) {
                throw std::overflow_error("derived cluster capacity exceeds uint32 range");
            }
            args.tau = static_cast<uint32_t>(rounded);
        }
        const uint32_t nb = static_cast<uint32_t>(
            (static_cast<uint64_t>(args.tau) + Index::kBlockVectors - 1) / Index::kBlockVectors);
        if (nb == 0 || *std::max_element(index.len.begin(), index.len.end()) > args.tau) {
            throw std::invalid_argument("cluster exceeds tau; pass the preprocessing --size-max value via --tau");
        }
        if (index.num_clusters != args.clusters) {
            log_step("resolved actual cluster count from index: " + std::to_string(index.num_clusters));
            args.clusters = index.num_clusters;
        }

        const ExactPayloadFormat exact_payload_format = exact_payload_format_for_args(args);
        const size_t exact_payload_bytes = exact_payload_bytes_for_args(args);
        log_step("FuORAM exact payload format: " + exact_payload_format_tag(args) +
                 ", bytes: " + std::to_string(exact_payload_bytes));

        const int cluster_block_size = ann_path_block_size();
        const int ring_block_size = packed_byte_int_count(static_cast<int>(exact_payload_bytes));
        const int cluster_num_blocks = std::max(8, total_path_blocks(index));
        std::vector<int> cluster_block_counts(index.num_clusters, 0);
        for (uint32_t c = 0; c < index.num_clusters; c++) {
            cluster_block_counts[c] = static_cast<int>(index.cluster_num_blocks(c));
        }
        const int cluster_num_levels = choose_cluster_levels(cluster_num_blocks, args.ring_z);
        const int cluster_cached_levels = std::min(kClusterCachedLevels, std::max(0, cluster_num_levels - 2));
        const int ring_num_levels = choose_ring_levels_with_load_slack(static_cast<int>(index.n), args.ring_z);
        const int ring_cached_levels = std::min(kRingCachedLevels, std::max(0, ring_num_levels - 1));

        Bucket::resetState();
        SBucket::resetState();

        RingOramConfig cluster_config(
            cluster_block_size,
            args.ring_z,
            args.ring_s,
            args.ring_a,
            cluster_num_blocks,
            cluster_num_levels,
            cluster_cached_levels);
        RingOramConfig ring_config(
            ring_block_size,
            args.ring_z,
            args.ring_s,
            args.ring_a,
            static_cast<int>(index.n),
            ring_num_levels,
            ring_cached_levels);

        const CachePaths cache_paths = make_cache_paths(
            args,
            index,
            cluster_block_size,
            ring_block_size,
            cluster_config.real_bucket_size,
            cluster_config.dummy_size,
            cluster_num_blocks,
            cluster_config.num_levels,
            cluster_config.num_buckets,
            cluster_cached_levels,
            ring_num_levels);
        const std::string exact_payload_cache_path =
            make_exact_payload_cache_path(args, index, exact_payload_bytes);


        log_step("constructing ORAM clients and servers");
        RandomForOram random;
        RemoteRing cluster_server(nullptr, cluster_config, true, true, false);
        RemoteRing cluster_client(&cluster_server, cluster_config, false);
        QuORAM cluster_oram(&cluster_client, &random, cluster_config, cluster_num_levels, cluster_block_counts);

        RemoteRing ring_server(nullptr, ring_config, true, true, false);
        RemoteRing ring_client(&ring_server, ring_config, false);
        FuORAM ring_oram(&ring_client, &random, ring_config, ring_num_levels, ring_cached_levels, true, true);

        const NetworkProfile network_profile = network_profile_from_args(args);
        const bool metadata_on_server = args.metadata_storage == "server";
        cluster_oram.set_network_profile(network_profile);
        ring_oram.set_network_profile(network_profile);
        cluster_oram.set_metadata_remote(metadata_on_server);
        ring_oram.set_metadata_remote(metadata_on_server);
        log_step("network simulation: " + network_profile_label(args, network_profile));
        log_step("metadata storage: " + args.metadata_storage);
        log_step("QuORAM cached top levels: " + std::to_string(cluster_oram.num_cached_levels));
        log_step("FuORAM cached top levels: " + std::to_string(ring_cached_levels));
        log_step("checking ORAM initialization cache: " + cache_paths.prefix);
        if (cache_files_exist(cache_paths)) {
            log_step("cache hit; loading QuORAM server state");
            cluster_server.load_server_state(cache_paths.cluster_server.c_str());
            log_step("cache hit; loading QuORAM metadata");
            load_cluster_metadata(cluster_oram, cache_paths.cluster_metadata);
            log_step("cache hit; loading FuORAM server state");
            ring_server.load_server_state(cache_paths.ring_server.c_str());
            log_step("cache hit; loading FuORAM metadata");
            load_ring_metadata(ring_oram, cache_paths.ring_metadata);
        } else {
            std::mt19937 init_rng(args.seed);
            std::vector<int> latest_ring_leaf(index.n, -1);
            {
                std::vector<std::vector<uint8_t>> exact_payloads;
                if (file_exists(exact_payload_cache_path)) {
                    log_step("exact payload cache hit: " + exact_payload_cache_path);
                    exact_payloads = load_exact_payloads(index.n, exact_payload_cache_path, exact_payload_bytes);
                } else {
                    log_step("building raw-coordinate FuORAM payloads");
                    exact_payloads = build_exact_payloads(index, exact_payload_format);
                    log_step("saving exact payload cache: " + exact_payload_cache_path);
                    ensure_directory(cache_paths.directory);
                    save_exact_payloads(exact_payloads, exact_payload_cache_path, exact_payload_bytes);
                }

                log_step("placing FuORAM blocks");
                std::map<int, std::vector<Block*>> ring_buckets =
                    make_dummy_buckets(ring_config.num_buckets, ring_config.bucket_size, ring_block_size);
                place_ring_blocks(
                    ring_oram,
                    ring_buckets,
                    latest_ring_leaf,
                    exact_payloads,
                    ring_block_size,
                    args.ring_z,
                    init_rng);
                install_ring_metadata(ring_oram, ring_buckets, latest_ring_leaf);
                ring_client.WriteBucketBatchMapAsBlockRing(ring_buckets, ring_config.bucket_size, false);
                release_written_bucket_map_storage(ring_buckets);
                release_vector_storage(exact_payloads);
            }

            log_step("building QuORAM approximate blocks");
            {
                std::map<int, std::vector<Block*>> cluster_buckets =
                    make_dummy_buckets(cluster_config.num_buckets, cluster_config.bucket_size, cluster_block_size);
                std::vector<int> cluster_used(cluster_config.num_buckets, 0);
                uint32_t logical_address = 0;
                log_progress("building QuORAM cluster 0 / " +
                             std::to_string(index.num_clusters));
                for (uint32_t c = 0; c < index.num_clusters; c++) {
                    for (uint32_t block_id = 0; block_id < index.cluster_num_blocks(c); block_id++) {
                        place_cluster_block(
                            cluster_oram,
                            cluster_buckets,
                            cluster_used,
                            cluster_block_size,
                            logical_address,
                            c,
                            block_id,
                            block_vector_ids(index, c, block_id),
                            latest_ring_leaf,
                            build_approx_oram_payload(index, c, block_id));
                        logical_address++;
                    }
                    if ((c + 1) % 128 == 0 || c + 1 == index.num_clusters) {
                        log_progress("building QuORAM cluster " + std::to_string(c + 1) + " / " +
                                     std::to_string(index.num_clusters));
                    }
                }
                log_progress("building QuORAM clusters complete: " +
                             std::to_string(index.num_clusters) + " clusters", true);
                cluster_client.WriteBucketBatchMapAsBlockRing(cluster_buckets, cluster_config.bucket_size, false);
                release_written_bucket_map_storage(cluster_buckets);
            }
            release_vector_storage(latest_ring_leaf);

            log_step("saving ORAM initialization cache");
            ensure_directory(cache_paths.directory);
            cluster_server.save_server_state(cache_paths.cluster_server.c_str());
            save_cluster_metadata(cluster_oram, cache_paths.cluster_metadata);
            ring_server.save_server_state(cache_paths.ring_server.c_str());
            save_ring_metadata(ring_oram, cache_paths.ring_metadata);
        }

        DoubleORAMConfig config(
            static_cast<int>(index.num_clusters),
            static_cast<int>(ApproxLayout::kPlainBytes),
            DIM,
            static_cast<int>(Index::kBlockVectors),
            kPathsPerCluster,
            0,
            DoubleORAMPathBlockLayout::AnnBatch);
        DoubleORAM double_oram(cluster_oram, ring_oram, config);
        DoubleOramAnnReader ann_oram(double_oram, static_cast<int>(exact_payload_bytes), nb);
        PrivateIvfRabitQSearcher<DIM, BB, B_QUERY> searcher(index, exact_payload_format);

        log_step("releasing index arrays unused by DoubleORAM query loop");
        index.release_double_oram_search_unused_data();

        log_step("loading query, groundtruth, and projection files");
        Matrix<float> query = read_float_vectors_file(args.files.path(args.source, args.dataset, "query"));
        Matrix<uint32_t> gt = read_uint32_vectors_file(args.files.path(args.source, args.dataset, "groundtruth"));
        Matrix<float> randomized_query;
        {
            Matrix<float> projection(args.source + "P_C" + std::to_string(args.clusters) +
                                     "_B" + std::to_string(BB) + ".fvecs");
            log_step("computing randomized queries");
            randomized_query = multiply_padded_left(query, BB, projection);
        }

        const uint32_t nq = args.nqueries == 0
            ? static_cast<uint32_t>(query.n)
            : std::min<uint32_t>(args.nqueries, static_cast<uint32_t>(query.n));
        if (nq == 0) {
            throw std::runtime_error("no queries to run");
        }
        auto minmax_len = std::minmax_element(index.len.begin(), index.len.end());
        std::cout << std::fixed << std::setprecision(6);
        std::cout << "dataset " << args.dataset << "\n";
        std::cout << "queries " << nq << "\n";
        std::cout << "topk " << args.k << "\n";
        std::cout << "k2_values " << join_uint32_values(args.k2_values) << "\n";
        std::cout << "nprobe_values " << join_uint32_values(args.nprobes) << "\n";
        std::cout << "cluster_batch_clusters "
                  << (args.path_batch_clusters == 0
                          ? std::string("nprobe")
                          : std::to_string(args.path_batch_clusters))
                  << "\n";
        std::cout << "cluster_real_bucket_size " << cluster_config.real_bucket_size << "\n";
        std::cout << "cluster_dummy_size " << cluster_config.dummy_size << "\n";
        std::cout << "cluster_evict_rate " << cluster_config.evict_rate << "\n";
        std::cout << "ring_evict_rate " << args.ring_a << "\n";
        std::cout << "clusters " << index.num_clusters << "\n";
        std::cout << "cluster_size_min " << *minmax_len.first << "\n";
        std::cout << "cluster_size_max " << *minmax_len.second << "\n";
        std::cout << "tau " << args.tau << "\n";
        std::cout << "nb " << nb << "\n";
        std::cout << "cluster_logical_blocks " << cluster_num_blocks << "\n";
        std::cout << "max_cluster_approx_blocks " << index.approx_blocks_per_cluster << "\n";
        std::cout << "cluster_block_size_ints " << cluster_block_size << "\n";
        std::cout << "ring_block_size_ints " << ring_block_size << "\n";
        std::cout << "network_simulation " << network_profile_label(args, network_profile) << "\n";
        std::cout << "metadata_storage " << args.metadata_storage << "\n";
        print_persistent_storage_stats(
            index,
            cluster_oram,
            ring_oram,
            cluster_config,
            ring_config,
            metadata_on_server);

        ensure_directory("results");
        for (size_t nprobe_index = 0; nprobe_index < args.nprobes.size(); nprobe_index++) {
            Args run_args = args;
            run_args.nprobe = args.nprobes[nprobe_index];
            run_args.nprobes = {run_args.nprobe};
            if (run_args.path_batch_clusters == 0) {
                run_args.path_batch_clusters = run_args.nprobe;
            }
            const uint32_t k2 = args.k2_values[nprobe_index];
            run_args.k2 = k2;

            std::cout << "nprobe_run_begin " << (nprobe_index + 1)
                      << " / " << args.nprobes.size() << "\n";
            std::cout << "nprobe " << run_args.nprobe << "\n";
            std::cout << "k2 " << k2 << "\n";

            Totals totals;

            log_step("starting queries for nprobe " + std::to_string(run_args.nprobe) +
                     " (" + std::to_string(nprobe_index + 1) + " / " +
                     std::to_string(args.nprobes.size()) + ")");
            for (uint32_t qi = 0; qi < nq; qi++) {
                log_progress("running nprobe " + std::to_string(run_args.nprobe) +
                             " query " + std::to_string(qi + 1) + " / " + std::to_string(nq));
                cluster_oram.reset_query_access_stats();
                ring_oram.reset_query_access_stats();

                const auto t0 = std::chrono::high_resolution_clock::now();
                ResultHeap result = searcher.search_double_oram(
                    query.row(qi),
                    randomized_query.row(qi),
                    run_args.k,
                    run_args.nprobe,
                    ann_oram,
                    run_args.path_batch_clusters,
                    k2);
                const double query_wall_sec = elapsed_sec_since(t0);
                const uint64_t parallel_evict_round_trips = std::max(
                    cluster_oram.stat_evict_net_round_trips,
                    ring_oram.stat_evict_net_round_trips);
                const uint64_t overlapped_evict_round_trips =
                    cluster_oram.stat_evict_net_round_trips +
                    ring_oram.stat_evict_net_round_trips -
                    parallel_evict_round_trips;
                const double overlapped_evict_latency_sec =
                    network_profile.enabled && !network_profile.sleep
                        ? static_cast<double>(overlapped_evict_round_trips) *
                              network_profile.rtt_ms / 1000.0
                        : 0.0;
                const double unslept_network_sec =
                    network_profile.enabled && !network_profile.sleep
                        ? cluster_oram.stat_net_total_time_sec +
                              ring_oram.stat_net_total_time_sec -
                              overlapped_evict_latency_sec
                        : 0.0;
                const double query_sec = query_wall_sec + unslept_network_sec;

                totals.query_time_sec += query_sec;
                totals.recall += recall_at_k(result, gt, qi, run_args.k);
                totals.cluster_accesses += cluster_oram.stat_access_invocations;
                totals.cluster_evict_calls += cluster_oram.stat_evict_invocations;
                totals.cluster_evicts += cluster_oram.stat_evict_effective;
                totals.ring_accesses += ring_oram.stat_bmasr_invocations;
                totals.ring_evict_calls += ring_oram.stat_evict_invocations;
                totals.ring_evicts += ring_oram.stat_evict_effective;
                totals.cluster_access_time_sec += cluster_oram.stat_access_time_sec;
                totals.cluster_evict_time_sec += cluster_oram.stat_evict_time_sec;
                totals.cluster_evict_effective_time_sec += cluster_oram.stat_evict_effective_time_sec;
                totals.ring_access_time_sec += ring_oram.stat_bmasr_time_sec;
                totals.ring_evict_time_sec += ring_oram.stat_evict_time_sec;
                totals.ring_evict_effective_time_sec += ring_oram.stat_evict_effective_time_sec;
                totals.cluster_comm_bytes += cluster_oram.stat_comm_bytes;
                totals.ring_comm_bytes += ring_oram.stat_comm_bytes;
                totals.cluster_access_net_round_trips += cluster_oram.stat_access_net_round_trips;
                totals.ring_access_net_round_trips += ring_oram.stat_access_net_round_trips;
                totals.oram_evict_net_round_trips += parallel_evict_round_trips;
                totals.max_cluster_stash_blocks_after_evict = std::max(
                    totals.max_cluster_stash_blocks_after_evict,
                    cluster_oram.stat_max_stash_blocks_after_evict);
                totals.max_ring_stash_blocks_after_evict = std::max(
                    totals.max_ring_stash_blocks_after_evict,
                    ring_oram.stat_max_stash_blocks_after_evict);
                totals.approximate_distance_count += searcher.counters().approximate_distance_count;
                totals.exact_distance_count += searcher.counters().exact_distance_count;
                totals.ann_centroid_select_time_sec += searcher.counters().centroid_select_time_sec;
                totals.ann_path_read_time_sec += searcher.counters().path_read_time_sec;
                totals.ann_approx_scan_time_sec += searcher.counters().approx_scan_time_sec;
                totals.ann_candidate_select_time_sec += searcher.counters().candidate_select_time_sec;
                totals.ann_ring_read_time_sec += searcher.counters().ring_read_time_sec;
                totals.ann_exact_compute_time_sec += searcher.counters().exact_compute_time_sec;
                totals.ann_path_finish_time_sec += searcher.counters().path_finish_time_sec;
                totals.ann_ring_finish_time_sec += searcher.counters().ring_finish_time_sec;
            }
            log_progress("queries complete for nprobe " + std::to_string(run_args.nprobe) +
                         ": " + std::to_string(nq) + " queries", true);

            const uint64_t total_comm_bytes = totals.cluster_comm_bytes + totals.ring_comm_bytes;
            std::cout << "avg_query_time_ms " << sec_to_ms(totals.query_time_sec) / nq << "\n";
            std::cout << "avg_cluster_oram_accesses_per_query "
                      << static_cast<double>(totals.cluster_accesses) / nq << "\n";
            std::cout << "avg_cluster_oram_writeback_calls_per_query "
                      << static_cast<double>(totals.cluster_evict_calls) / nq << "\n";
            std::cout << "avg_cluster_oram_writebacks_per_query "
                      << static_cast<double>(totals.cluster_evicts) / nq << "\n";
            std::cout << "avg_ring_accesses_per_query "
                      << static_cast<double>(totals.ring_accesses) / nq << "\n";
            std::cout << "avg_ring_writeback_calls_per_query "
                      << static_cast<double>(totals.ring_evict_calls) / nq << "\n";
            std::cout << "avg_ring_writebacks_per_query "
                      << static_cast<double>(totals.ring_evicts) / nq << "\n";
            std::cout << "avg_cluster_oram_access_time_ms_per_query "
                      << sec_to_ms(totals.cluster_access_time_sec) / nq << "\n";
            std::cout << "avg_cluster_oram_writeback_time_ms_per_query "
                      << sec_to_ms(totals.cluster_evict_time_sec) / nq << "\n";
            std::cout << "avg_ring_access_time_ms_per_query "
                      << sec_to_ms(totals.ring_access_time_sec) / nq << "\n";
            std::cout << "avg_ring_writeback_time_ms_per_query "
                      << sec_to_ms(totals.ring_evict_time_sec) / nq << "\n";
            std::cout << "avg_cluster_oram_access_time_ms_per_access "
                      << avg_ms_per_call(totals.cluster_access_time_sec, totals.cluster_accesses) << "\n";
            std::cout << "avg_cluster_oram_writeback_time_ms_per_call "
                      << avg_ms_per_call(totals.cluster_evict_time_sec, totals.cluster_evict_calls) << "\n";
            std::cout << "avg_cluster_oram_writeback_time_ms_per_writeback "
                      << avg_ms_per_call(totals.cluster_evict_effective_time_sec, totals.cluster_evicts) << "\n";
            std::cout << "avg_ring_access_time_ms_per_access "
                      << avg_ms_per_call(totals.ring_access_time_sec, totals.ring_accesses) << "\n";
            std::cout << "avg_ring_writeback_time_ms_per_call "
                      << avg_ms_per_call(totals.ring_evict_time_sec, totals.ring_evict_calls) << "\n";
            std::cout << "avg_ring_writeback_time_ms_per_writeback "
                      << avg_ms_per_call(totals.ring_evict_effective_time_sec, totals.ring_evicts) << "\n";
            std::cout << "avg_ann_centroid_select_time_ms_per_query "
                      << sec_to_ms(totals.ann_centroid_select_time_sec) / nq << "\n";
            std::cout << "avg_ann_cluster_read_time_ms_per_query "
                      << sec_to_ms(totals.ann_path_read_time_sec) / nq << "\n";
            std::cout << "avg_ann_approx_scan_time_ms_per_query "
                      << sec_to_ms(totals.ann_approx_scan_time_sec) / nq << "\n";
            std::cout << "avg_ann_candidate_select_time_ms_per_query "
                      << sec_to_ms(totals.ann_candidate_select_time_sec) / nq << "\n";
            std::cout << "avg_ann_ring_read_time_ms_per_query "
                      << sec_to_ms(totals.ann_ring_read_time_sec) / nq << "\n";
            std::cout << "avg_ann_exact_compute_time_ms_per_query "
                      << sec_to_ms(totals.ann_exact_compute_time_sec) / nq << "\n";
            std::cout << "avg_ann_cluster_finish_time_ms_per_query "
                      << sec_to_ms(totals.ann_path_finish_time_sec) / nq << "\n";
            std::cout << "avg_ann_ring_finish_time_ms_per_query "
                      << sec_to_ms(totals.ann_ring_finish_time_sec) / nq << "\n";
            std::cout << "avg_comm_total_bytes_per_query "
                      << static_cast<double>(total_comm_bytes) / nq << "\n";
            std::cout << "avg_comm_cluster_oram_bytes_per_query "
                      << static_cast<double>(totals.cluster_comm_bytes) / nq << "\n";
            std::cout << "avg_comm_ring_bytes_per_query "
                      << static_cast<double>(totals.ring_comm_bytes) / nq << "\n";
            std::cout << "avg_comm_total_mib_per_query "
                      << bytes_to_mib(total_comm_bytes) / nq << "\n";
            std::cout << "avg_comm_cluster_oram_mib_per_query "
                      << bytes_to_mib(totals.cluster_comm_bytes) / nq << "\n";
            std::cout << "avg_comm_ring_mib_per_query "
                      << bytes_to_mib(totals.ring_comm_bytes) / nq << "\n";
            const uint64_t total_net_round_trips =
                totals.cluster_access_net_round_trips +
                totals.ring_access_net_round_trips +
                totals.oram_evict_net_round_trips;
            std::cout << "avg_total_net_round_trips_per_query "
                      << static_cast<double>(total_net_round_trips) / nq << "\n";
            std::cout << "avg_cluster_oram_access_net_round_trips_per_query "
                      << static_cast<double>(totals.cluster_access_net_round_trips) / nq << "\n";
            std::cout << "avg_ring_oram_access_net_round_trips_per_query "
                      << static_cast<double>(totals.ring_access_net_round_trips) / nq << "\n";
            std::cout << "avg_oram_evict_net_round_trips_per_query "
                      << static_cast<double>(totals.oram_evict_net_round_trips) / nq << "\n";
            std::cout << "recall@" << run_args.k << " " << totals.recall / nq << "\n";
            std::cout << "avg_approx_distance_count "
                      << static_cast<double>(totals.approximate_distance_count) / nq << "\n";
            std::cout << "avg_exact_distance_count "
                      << static_cast<double>(totals.exact_distance_count) / nq << "\n";
            std::cout << "max_cluster_oram_stash_blocks_after_evict "
                      << totals.max_cluster_stash_blocks_after_evict << "\n";
            std::cout << "max_ring_oram_stash_blocks_after_evict "
                      << totals.max_ring_stash_blocks_after_evict << "\n";
            const std::string result_csv_path = make_result_csv_path(run_args);
            write_result_csv(result_csv_path, run_args, totals, nq, total_comm_bytes);
            std::cout << "result_csv " << result_csv_path << "\n";
            std::cout << "nprobe_run_end " << run_args.nprobe << "\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_ivf_rabitq_double_oram failed: " << e.what() << "\n";
        return 1;
    } catch (const std::runtime_error* e) {
        std::cerr << "test_ivf_rabitq_double_oram failed: " << e->what() << "\n";
        delete e;
        return 1;
    }
}
