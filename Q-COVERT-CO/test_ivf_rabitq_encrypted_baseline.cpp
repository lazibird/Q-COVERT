#include "repository_paths.h"
#include "NetworkSimulator.h"
#include "evp.h"
#include "fast_scan.h"
#include "ivf_rabitq_index.h"
#include "matrix.h"
#include "oram_block_builders.h"
#include "simple_json.h"
#include "space.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kBB = 128;
constexpr uint32_t kQueryBits = 4;
constexpr uint32_t kIndexMagic = 0x44524951;      // "DRIQ"
constexpr uint32_t kBaselineCacheMagic = 0x45424c42; // "EBLB"
constexpr uint32_t kBaselineCacheVersion = 1;
constexpr uint32_t kClusterPlainMagic = 0x434c5042;  // "CLPB"

using double_oram::NetworkProfile;
using double_oram::simulate_network_transfer;

enum class BaselineExactPayloadFormat : uint32_t {
    RawUint8 = 1,
    RawFloat32 = 2,
};

struct Args {
    repository_paths::DatasetFiles files;
    std::string dataset = "sift";
    std::string source;
    std::string index_path;
    std::string assignment = "split";
    uint32_t clusters = 0;
    uint32_t k = 10;
    uint32_t nprobe = 100;
    std::vector<uint32_t> nprobes;
    std::vector<uint32_t> k2_values;
    double k2_ratio = 0.5;
    uint32_t nqueries = 0;
    uint32_t path_batch_clusters = 4;
    uint32_t seed = 20260623;
    std::string net_sim = "none";
    double net_bandwidth_mbps = -1.0;
    double net_rtt_ms = -1.0;
    bool net_no_sleep = true;
    std::string exact_payload = "auto";
    std::string results_dir = "results";
};

struct BaselineTotals {
    double query_time_sec = 0.0;
    double cluster_access_time_sec = 0.0;
    double exact_access_time_sec = 0.0;
    double ann_centroid_select_time_sec = 0.0;
    double ann_cluster_read_time_sec = 0.0;
    double ann_approx_scan_time_sec = 0.0;
    double ann_candidate_select_time_sec = 0.0;
    double ann_exact_read_time_sec = 0.0;
    double ann_exact_compute_time_sec = 0.0;
    double recall = 0.0;
    uint64_t cluster_accesses = 0;
    uint64_t exact_accesses = 0;
    uint64_t cluster_comm_bytes = 0;
    uint64_t exact_comm_bytes = 0;
    uint64_t cluster_net_round_trips = 0;
    uint64_t exact_net_round_trips = 0;
    uint64_t approximate_distance_count = 0;
    uint64_t exact_distance_count = 0;
};

struct QueryNetworkStats {
    double cluster_access_time_sec = 0.0;
    double exact_access_time_sec = 0.0;
    uint64_t cluster_accesses = 0;
    uint64_t exact_accesses = 0;
    uint64_t cluster_comm_bytes = 0;
    uint64_t exact_comm_bytes = 0;
    uint64_t cluster_net_round_trips = 0;
    uint64_t exact_net_round_trips = 0;
    double net_total_time_sec = 0.0;
};

struct SearchCounters {
    uint64_t approximate_distance_count = 0;
    uint64_t exact_distance_count = 0;
    double centroid_select_time_sec = 0.0;
    double cluster_read_time_sec = 0.0;
    double approx_scan_time_sec = 0.0;
    double candidate_select_time_sec = 0.0;
    double exact_read_time_sec = 0.0;
    double exact_compute_time_sec = 0.0;
};

struct IndexHeader {
    uint32_t dim = 0;
    uint32_t bits = 0;
    uint32_t n = 0;
    uint32_t num_clusters = 0;
    uint32_t version = 0;
    uint32_t raw_storage = 0;
};

std::string sanitize_for_filename(std::string value);
std::string lower_ascii(std::string value);
bool file_exists(const std::string& path);
void ensure_directory(const std::string& path);

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

double elapsed_sec_since(std::chrono::high_resolution_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
}

double sec_to_ms(double seconds) {
    return seconds * 1000.0;
}

double bytes_to_mib(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

double bytes_to_gb(uint64_t bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
}

double avg_ms_per_call(double seconds, uint64_t calls) {
    if (calls == 0) {
        return 0.0;
    }
    return sec_to_ms(seconds) / static_cast<double>(calls);
}

uint32_t default_clusters_for_dataset(std::string dataset) {
    dataset = lower_ascii(std::move(dataset));
    if (dataset == "sift1b" || dataset == "sift100m") {
        return 1u << 16;
    }
    if (dataset == "siftsmall") {
        return 48;
    }
    return 4096;
}

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
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
        uint32_t requested_clusters,
        const std::string& assignment) {
    if (assignment == "split") {
        return source + dataset + "_assignment_" + std::to_string(requested_clusters) + ".txt";
    }
    return source + dataset + "_assignment_" + std::to_string(requested_clusters) +
           "_" + assignment_tag_for_paths(assignment) + ".txt";
}

uint32_t read_manifest_clusters(
        const std::string& source,
        const std::string& dataset,
        uint32_t requested_clusters,
        const std::string& assignment) {
    std::ifstream in(assignment_manifest_path(source, dataset, requested_clusters, assignment));
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
        uint32_t clusters,
        const std::string& assignment,
        uint32_t bits) {
    if (assignment == "split") {
        return source + "ivfrabitq" + std::to_string(clusters) +
               "_B" + std::to_string(bits) + ".index";
    }
    return source + "ivfrabitq" + std::to_string(clusters) +
           "_" + assignment_tag_for_paths(assignment) +
           "_B" + std::to_string(bits) + ".index";
}

uint32_t resolve_index_clusters(
        const std::string& source,
        const std::string& dataset,
        uint32_t requested_clusters,
        const std::string& assignment,
        uint32_t& resolved_bits) {
    const uint32_t manifest_clusters =
        read_manifest_clusters(source, dataset, requested_clusters, assignment);
    if (manifest_clusters != 0 &&
            file_exists(index_path_for_clusters(source, manifest_clusters, assignment, resolved_bits))) {
        return manifest_clusters;
    }
    if (file_exists(index_path_for_clusters(source, requested_clusters, assignment, resolved_bits))) {
        return requested_clusters;
    }

    std::vector<std::pair<uint32_t, uint32_t>> candidates;
    DIR* dir = opendir(source.c_str());
    if (dir != nullptr) {
        const std::string prefix = "ivfrabitq";
        const std::string marker = (assignment == "split")
            ? "_B"
            : "_" + assignment_tag_for_paths(assignment) + "_B";
        const std::string suffix = ".index";
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (!starts_with(name, prefix) || !ends_with(name, suffix)) {
                continue;
            }
            const size_t marker_pos = name.find(marker, prefix.size());
            if (marker_pos == std::string::npos) {
                continue;
            }
            const std::string cluster_text = name.substr(prefix.size(), marker_pos - prefix.size());
            const std::string bits_text = name.substr(
                marker_pos + marker.size(),
                name.size() - marker_pos - marker.size() - suffix.size());
            const auto all_digits = [](const std::string& text) {
                return !text.empty() &&
                       std::all_of(text.begin(), text.end(), [](unsigned char ch) {
                           return std::isdigit(ch);
                       });
            };
            if (!all_digits(cluster_text) || !all_digits(bits_text)) {
                continue;
            }
            candidates.emplace_back(
                static_cast<uint32_t>(std::stoul(cluster_text)),
                static_cast<uint32_t>(std::stoul(bits_text)));
        }
        closedir(dir);
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1) {
        resolved_bits = candidates.front().second;
        return candidates.front().first;
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
            const uint32_t parsed = config_uint32(value.array_value[i], path + "[" + std::to_string(i) + "]");
            if (parsed == 0) {
                throw std::runtime_error("config list value must be positive: " + path);
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
        } else if (key == "assignment" && !cli_provided(provided, "assignment")) {
            args.assignment = config_string(value, value_path);
        } else if (key == "clusters" && !cli_provided(provided, "clusters")) {
            args.clusters = config_uint32(value, value_path);
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
        } else if (key == "k2_ratio" && !cli_provided(provided, "k2_ratio")) {
            args.k2_ratio = config_double(value, value_path);
        } else if (key == "nqueries" && !cli_provided(provided, "nqueries")) {
            args.nqueries = config_uint32(value, value_path);
        } else if (key == "path_batch_clusters" && !cli_provided(provided, "path_batch_clusters")) {
            args.path_batch_clusters = config_uint32(value, value_path);
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
    if (const simple_json::Value* defaults_query = simple_json::get_path(root, {"defaults", "query"})) {
        apply_query_config_section(args, defaults_query, provided, "defaults.query");
    }
    const simple_json::Value* dataset_section = simple_json::get_path(root, {"datasets", args.dataset});
    if (dataset_section != nullptr) {
        apply_query_config_section(args, dataset_section, provided, "datasets." + args.dataset);
        if (const simple_json::Value* query_section = dataset_section->get("query")) {
            apply_query_config_section(args, query_section, provided, "datasets." + args.dataset + ".query");
        }
    }
}

void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--dataset sift] [--source ../data/sift]"
              << " [--clusters dataset-default] [--assignment nearest|split] [--index path]"
              << " [--k 10] [--nprobe 100] [--nprobes 20,50,100] [--k2 60,150,250]"
              << " [--path-batch-clusters 4] [--nqueries 0]"
              << " [--net-sim none|fast|slow|custom]"
              << " [--net-bandwidth-mbps value] [--net-rtt-ms value] [--net-no-sleep]"
              << " [--exact-payload auto|raw-uint8|raw-float32]"
              << " [--results-dir results] [--config config.json] [--no-config]\n";
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
        } else if (key == "--assignment") {
            provided.insert("assignment");
            args.assignment = need_value("--assignment");
        } else if (key == "--index") {
            provided.insert("index");
            args.index_path = need_value("--index");
        } else if (key == "--k") {
            provided.insert("k");
            args.k = static_cast<uint32_t>(std::stoul(need_value("--k")));
        } else if (key == "--nprobe") {
            provided.insert("nprobe");
            args.nprobe = static_cast<uint32_t>(std::stoul(need_value("--nprobe")));
        } else if (key == "--nprobes" || key == "--nprobe-list") {
            provided.insert("nprobes");
            args.nprobes = parse_uint32_list(need_value(key.c_str()));
        } else if (key == "--k2") {
            provided.insert("k2");
            args.k2_values = parse_uint32_list(need_value("--k2"));
        } else if (key == "--k2-ratio") {
            provided.insert("k2_ratio");
            args.k2_ratio = std::stod(need_value("--k2-ratio"));
        } else if (key == "--nqueries") {
            provided.insert("nqueries");
            args.nqueries = static_cast<uint32_t>(std::stoul(need_value("--nqueries")));
        } else if (key == "--path-batch-clusters") {
            provided.insert("path_batch_clusters");
            args.path_batch_clusters = static_cast<uint32_t>(std::stoul(need_value("--path-batch-clusters")));
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
        } else if (key == "--exact-payload") {
            provided.insert("exact_payload");
            args.exact_payload = lower_ascii(need_value("--exact-payload"));
        } else if (key == "--results-dir") {
            args.results_dir = need_value("--results-dir");
        } else if (key == "--ring-z" || key == "--ring-s" || key == "--ring-a" ||
                   key == "--metadata-storage") {
            (void)need_value(key.c_str());
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
    if (args.path_batch_clusters == 0) {
        throw std::invalid_argument("path-batch-clusters must be positive");
    }
    if (!args.k2_values.empty()) {
        if (args.k2_values.size() != args.nprobes.size()) {
            throw std::invalid_argument(
                "--k2 count (" + std::to_string(args.k2_values.size()) +
                ") must match nprobe count (" + std::to_string(args.nprobes.size()) + ")");
        }
        for (uint32_t k2_value : args.k2_values) {
            if (k2_value == 0) {
                throw std::invalid_argument("--k2 values must be positive");
            }
        }
    } else if (args.k2_ratio <= 0.0) {
        throw std::invalid_argument("--k2-ratio must be positive");
    }
    for (uint32_t nprobe_value : args.nprobes) {
        if (nprobe_value == 0) {
            throw std::invalid_argument("nprobe values must be positive");
        }
    }
    if (args.assignment != "nearest" && args.assignment != "split") {
        throw std::invalid_argument("--assignment must be nearest or split");
    }
    if (args.index_path.empty()) {
        uint32_t index_bits = kBB;
        const uint32_t actual_clusters =
            resolve_index_clusters(args.source, args.dataset, args.clusters, args.assignment, index_bits);
        args.clusters = actual_clusters;
        args.index_path = index_path_for_clusters(args.source, args.clusters, args.assignment, index_bits);
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
    std::replace(args.exact_payload.begin(), args.exact_payload.end(), '-', '_');
    if (args.exact_payload != "auto" &&
            args.exact_payload != "raw_uint8" &&
            args.exact_payload != "raw_float32") {
        throw std::invalid_argument("--exact-payload must be auto, raw-uint8, or raw-float32");
    }
    return args;
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

uint32_t derived_k2(const Args& args) {
    const double scaled =
        static_cast<double>(static_cast<uint64_t>(args.k) * args.nprobe) * args.k2_ratio;
    if (scaled > static_cast<double>(std::numeric_limits<uint32_t>::max())) {
        throw std::invalid_argument("derived k2 is too large");
    }
    return static_cast<uint32_t>(std::max(1.0, std::floor(scaled)));
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

bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void ensure_directory(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            throw std::runtime_error("path exists but is not a directory: " + path);
        }
        return;
    }
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
        throw std::runtime_error("failed to create directory: " + path);
    }
}

IndexHeader read_index_header(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open index: " + path);
    }
    IndexHeader header;
    uint32_t first = 0;
    in.read(reinterpret_cast<char*>(&first), sizeof(first));
    if (!in) {
        throw std::runtime_error("index header is too short: " + path);
    }
    if (first == kIndexMagic) {
        in.read(reinterpret_cast<char*>(&header.version), sizeof(header.version));
        in.read(reinterpret_cast<char*>(&header.n), sizeof(header.n));
    } else {
        header.version = 1;
        header.n = first;
    }
    in.read(reinterpret_cast<char*>(&header.dim), sizeof(header.dim));
    in.read(reinterpret_cast<char*>(&header.num_clusters), sizeof(header.num_clusters));
    in.read(reinterpret_cast<char*>(&header.bits), sizeof(header.bits));
    if (!in) {
        throw std::runtime_error("invalid index header: " + path);
    }
    return header;
}

template <typename T>
void read_pod(std::istream& in, T& value, const std::string& label) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    if (!in) {
        throw std::runtime_error("short read while reading " + label);
    }
}

template <typename T>
void write_pod(std::ostream& out, const T& value, const std::string& label) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    if (!out) {
        throw std::runtime_error("failed writing " + label);
    }
}

void skip_bytes(std::istream& in, uint64_t bytes, const std::string& label) {
    in.seekg(static_cast<std::streamoff>(bytes), std::ios::cur);
    if (!in) {
        throw std::runtime_error("failed skipping " + label);
    }
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
void initialize_query_quantization_offsets(IvfRabitQIndex<D, B, QueryBits>& index) {
    index.query_quantization_offsets.resize(B);
    std::mt19937 gen(1234567);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    for (uint32_t i = 0; i < B; ++i) {
        index.query_quantization_offsets[i] = uniform(gen);
    }
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
void load_index_search_metadata_only(
        const std::string& path,
        IvfRabitQIndex<D, B, QueryBits>& index) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open index: " + path);
    }

    uint32_t first = 0;
    read_pod(in, first, "index magic");
    if (first != kIndexMagic) {
        throw std::runtime_error("baseline metadata-only load requires a versioned index: " + path);
    }

    uint32_t version = 0;
    uint32_t d = 0;
    uint32_t b = 0;
    uint32_t raw_storage = 0;
    read_pod(in, version, "index version");
    if (version != 2 && version != 3) {
        throw std::runtime_error("unsupported index version for baseline metadata-only load");
    }
    read_pod(in, index.n, "index n");
    read_pod(in, d, "index dimension");
    read_pod(in, index.num_clusters, "index cluster count");
    read_pod(in, b, "index bits");
    if (d != D || b != B) {
        throw std::runtime_error("index dimension mismatch in metadata-only load");
    }

    read_pod(in, index.approx_blocks_per_cluster, "index approximate block count");
    if (index.approx_blocks_per_cluster == 0) {
        throw std::runtime_error("invalid approximate block count in index");
    }
    index.approx_blocks.assign(index.num_clusters, 0);
    in.read(reinterpret_cast<char*>(index.approx_blocks.data()),
            static_cast<std::streamsize>(index.approx_blocks.size() * sizeof(uint32_t)));
    if (!in) {
        throw std::runtime_error("short read while reading per-cluster approximate block counts");
    }

    skip_bytes(in, static_cast<uint64_t>(index.num_clusters) * sizeof(uint32_t), "index start array");
    index.len.assign(index.num_clusters, 0);
    in.read(reinterpret_cast<char*>(index.len.data()),
            static_cast<std::streamsize>(index.len.size() * sizeof(uint32_t)));
    if (!in) {
        throw std::runtime_error("short read while reading cluster lengths");
    }
    skip_bytes(in, static_cast<uint64_t>(index.n) * sizeof(uint32_t), "index ids");
    skip_bytes(in, static_cast<uint64_t>(index.n) * sizeof(float), "dist_to_centroid");
    skip_bytes(in, static_cast<uint64_t>(index.n) * sizeof(float), "x0");
    index.randomized_centroids.assign(static_cast<size_t>(index.num_clusters) * B, 0.0f);
    in.read(reinterpret_cast<char*>(index.randomized_centroids.data()),
            static_cast<std::streamsize>(index.randomized_centroids.size() * sizeof(float)));
    if (!in) {
        throw std::runtime_error("short read while reading randomized centroids");
    }

    if (version >= 3) {
        read_pod(in, raw_storage, "raw vector storage type");
        if (raw_storage != static_cast<uint32_t>(IvfRabitQIndex<D, B, QueryBits>::RawVectorStorage::Float32) &&
                raw_storage != static_cast<uint32_t>(IvfRabitQIndex<D, B, QueryBits>::RawVectorStorage::Uint8)) {
            throw std::runtime_error("invalid raw vector storage type in index");
        }
        index.raw_vector_storage =
            static_cast<typename IvfRabitQIndex<D, B, QueryBits>::RawVectorStorage>(raw_storage);
    } else {
        index.raw_vector_storage = IvfRabitQIndex<D, B, QueryBits>::RawVectorStorage::Float32;
    }

    const uint64_t raw_element_bytes = index.stores_raw_vectors_as_uint8() ? 1 : sizeof(float);
    skip_bytes(in, static_cast<uint64_t>(index.n) * D * raw_element_bytes, "raw vectors");
    skip_bytes(in, static_cast<uint64_t>(index.n) * B / 64 * sizeof(uint64_t), "binary codes");
    initialize_query_quantization_offsets(index);
}

BaselineExactPayloadFormat exact_payload_format_for_args(const Args& args, bool index_stores_uint8) {
    std::string payload = lower_ascii(args.exact_payload);
    std::replace(payload.begin(), payload.end(), '-', '_');
    if (payload == "raw_uint8") {
        return BaselineExactPayloadFormat::RawUint8;
    }
    if (payload == "raw_float32") {
        return BaselineExactPayloadFormat::RawFloat32;
    }
    return index_stores_uint8
        ? BaselineExactPayloadFormat::RawUint8
        : BaselineExactPayloadFormat::RawFloat32;
}

std::string exact_payload_format_tag(BaselineExactPayloadFormat format) {
    switch (format) {
        case BaselineExactPayloadFormat::RawUint8:
            return "rawu8";
        case BaselineExactPayloadFormat::RawFloat32:
            return "rawf32";
    }
    return "unknown";
}

template <uint32_t D, uint32_t B>
size_t exact_plain_bytes(BaselineExactPayloadFormat format) {
    if (format == BaselineExactPayloadFormat::RawUint8) {
        return D + sizeof(uint32_t);
    }
    if (format == BaselineExactPayloadFormat::RawFloat32) {
        return static_cast<size_t>(D) * sizeof(float) + sizeof(uint32_t);
    }
    return ExactOramBlockLayout<D, B>::kPlainBytes;
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
std::vector<float> compute_cluster_means_if_needed(
        const IvfRabitQIndex<D, B, QueryBits>& index,
        BaselineExactPayloadFormat format) {
    return {};
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
std::vector<uint8_t> build_raw_float_exact_payload(
        const IvfRabitQIndex<D, B, QueryBits>& index,
        uint32_t cluster_id,
        uint32_t local_vector_id) {
    if (cluster_id >= index.num_clusters || local_vector_id >= index.len[cluster_id]) {
        throw std::out_of_range("build_raw_float_exact_payload: vector address is out of range");
    }
    const uint32_t global_offset = index.start[cluster_id] + local_vector_id;
    std::vector<uint8_t> plain(static_cast<size_t>(D) * sizeof(float) + sizeof(uint32_t), 0);
    if (index.stores_raw_vectors_as_uint8()) {
        for (uint32_t d = 0; d < D; d++) {
            const float value = static_cast<float>(index.raw_vector_u8_by_cluster_offset(global_offset)[d]);
            std::memcpy(plain.data() + static_cast<size_t>(d) * sizeof(float), &value, sizeof(float));
        }
    } else {
        std::memcpy(plain.data(),
                    index.raw_vector_by_cluster_offset(global_offset),
                    static_cast<size_t>(D) * sizeof(float));
    }
    const uint32_t vector_id = index.ids[global_offset];
    std::memcpy(plain.data() + static_cast<size_t>(D) * sizeof(float), &vector_id, sizeof(vector_id));
    return plain;
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
std::vector<uint8_t> build_exact_payload(
        const IvfRabitQIndex<D, B, QueryBits>& index,
        const std::vector<float>& cluster_means,
        BaselineExactPayloadFormat format,
        uint32_t cluster_id,
        uint32_t local_vector_id) {
    if (format == BaselineExactPayloadFormat::RawUint8) {
        return build_raw_uint8_exact_oram_payload(index, cluster_id, local_vector_id);
    }
    if (format == BaselineExactPayloadFormat::RawFloat32) {
        return build_raw_float_exact_payload(index, cluster_id, local_vector_id);
    }
    throw std::invalid_argument("unsupported exact payload format");
}

std::vector<uint8_t> encrypt_plain(const std::vector<uint8_t>& plain) {
    if (plain.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("plaintext is too large for encryption wrapper");
    }
    std::vector<uint8_t> cipher(static_cast<size_t>(compute_ctx_len(static_cast<int>(plain.size()))), 0);
    const int written = encrypt_wrapper(
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(plain.data())),
        static_cast<int>(plain.size()),
        reinterpret_cast<unsigned char*>(cipher.data()));
    if (written <= 0 || static_cast<size_t>(written) > cipher.size()) {
        throw std::runtime_error("encryption failed");
    }
    cipher.resize(static_cast<size_t>(written));
    return cipher;
}

std::vector<uint8_t> decrypt_cipher(const std::vector<uint8_t>& cipher, size_t plain_bytes) {
    if (cipher.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            plain_bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("ciphertext is too large for decryption wrapper");
    }
    std::vector<uint8_t> plain(plain_bytes, 0);
    const int written = decrypt_wrapper(
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(cipher.data())),
        static_cast<int>(cipher.size()),
        reinterpret_cast<unsigned char*>(plain.data()));
    if (written != static_cast<int>(plain_bytes)) {
        throw std::runtime_error("decryption produced an unexpected plaintext size");
    }
    return plain;
}

std::vector<uint8_t> decrypt_cipher_span(const uint8_t* cipher, size_t cipher_bytes, size_t plain_bytes) {
    if (cipher_bytes > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            plain_bytes > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("ciphertext is too large for decryption wrapper");
    }
    std::vector<uint8_t> plain(plain_bytes, 0);
    const int written = decrypt_wrapper(
        const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(cipher)),
        static_cast<int>(cipher_bytes),
        reinterpret_cast<unsigned char*>(plain.data()));
    if (written != static_cast<int>(plain_bytes)) {
        throw std::runtime_error("decryption produced an unexpected plaintext size");
    }
    return plain;
}

template <typename T>
void append_pod(std::vector<uint8_t>& out, const T& value) {
    const size_t old_size = out.size();
    out.resize(old_size + sizeof(T));
    std::memcpy(out.data() + old_size, &value, sizeof(T));
}

void append_bytes(std::vector<uint8_t>& out, const uint8_t* data, size_t bytes) {
    const size_t old_size = out.size();
    out.resize(old_size + bytes);
    if (bytes != 0) {
        std::memcpy(out.data() + old_size, data, bytes);
    }
}

template <typename T>
T read_pod_from_buffer(const std::vector<uint8_t>& buffer, size_t& offset, const std::string& label) {
    if (offset + sizeof(T) > buffer.size()) {
        throw std::runtime_error("cluster plaintext is truncated while reading " + label);
    }
    T value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
std::vector<uint8_t> build_cluster_plaintext(
        const IvfRabitQIndex<D, B, QueryBits>& index,
        const std::vector<float>& cluster_means,
        BaselineExactPayloadFormat exact_format,
        uint32_t cluster_id) {
    using ApproxLayout = ApproxOramBlockLayout<D, B>;
    const uint32_t num_blocks = static_cast<uint32_t>(index.cluster_num_blocks(cluster_id));
    std::vector<uint8_t> plain;
    plain.reserve(
        4 * sizeof(uint32_t) +
        static_cast<size_t>(D) * sizeof(float) +
        static_cast<size_t>(num_blocks) *
            (2 * sizeof(uint32_t) + IvfRabitQIndex<D, B, QueryBits>::kBlockVectors * sizeof(uint32_t) +
             ApproxLayout::kPlainBytes));
    append_pod(plain, kClusterPlainMagic);
    append_pod(plain, cluster_id);
    append_pod(plain, index.len[cluster_id]);
    append_pod(plain, num_blocks);

    std::vector<float> zeros(D, 0.0f);
    append_bytes(plain, reinterpret_cast<const uint8_t*>(zeros.data()), zeros.size() * sizeof(float));

    for (uint32_t block_id = 0; block_id < num_blocks; block_id++) {
        append_pod(plain, block_id);
        const uint32_t block_begin = block_id * IvfRabitQIndex<D, B, QueryBits>::kBlockVectors;
        const uint32_t active =
            block_begin >= index.len[cluster_id]
                ? 0
                : std::min<uint32_t>(
                    IvfRabitQIndex<D, B, QueryBits>::kBlockVectors,
                    index.len[cluster_id] - block_begin);
        append_pod(plain, active);
        std::array<uint32_t, IvfRabitQIndex<D, B, QueryBits>::kBlockVectors> vector_ids{};
        vector_ids.fill(static_cast<uint32_t>(-1));
        for (uint32_t j = 0; j < active; j++) {
            vector_ids[j] = index.ids[index.start[cluster_id] + block_begin + j];
        }
        append_bytes(plain, reinterpret_cast<const uint8_t*>(vector_ids.data()),
                     vector_ids.size() * sizeof(uint32_t));
        const std::vector<uint8_t> approx_payload =
            build_approx_oram_payload(index, cluster_id, block_id);
        append_bytes(plain, approx_payload.data(), approx_payload.size());
    }
    return plain;
}

std::string cache_directory(const Args& args) {
    return args.source + "encrypted_baseline_cache";
}

std::string result_directory(const Args& args) {
    return args.results_dir;
}

std::string make_result_csv_path(const Args& args) {
    std::ostringstream path;
    path << result_directory(args) << "/ivf_rabitq_"
         << sanitize_for_filename(args.dataset)
         << "_" << sanitize_for_filename(args.net_sim)
         << ".csv";
    return path.str();
}

std::string make_baseline_cache_path(
        const Args& args,
        uint32_t n,
        uint32_t dim,
        uint32_t bits,
        BaselineExactPayloadFormat format,
        size_t exact_plain_bytes_value) {
    std::ostringstream path;
    path << cache_directory(args) << "/"
         << sanitize_for_filename(args.dataset)
         << "_n" << n
         << "_c" << args.clusters
         << "_assign" << sanitize_for_filename(args.assignment)
         << "_dim" << dim
         << "_bb" << bits
         << "_bq" << kQueryBits
         << "_exactfmt" << exact_payload_format_tag(format)
         << "_exactbytes" << exact_plain_bytes_value
         << "_seed" << args.seed
         << ".baseline.bin";
    return path.str();
}

std::vector<std::vector<std::string>> read_csv_rows(const std::string& path) {
    std::vector<std::vector<std::string>> rows;
    if (!file_exists(path)) {
        return rows;
    }
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot read result csv: " + path);
    }
    std::string line;
    while (std::getline(in, line)) {
        std::vector<std::string> row;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ',')) {
            row.push_back(cell);
        }
        if (!row.empty()) {
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

void append_result_csv_row(
        const std::string& path,
        const std::vector<std::pair<std::string, std::string>>& values) {
    std::vector<std::vector<std::string>> rows = read_csv_rows(path);
    const bool legacy_wide = !rows.empty() && !rows[0].empty() && rows[0][0] == "property";
    if (rows.empty() || legacy_wide) {
        rows.clear();
        rows.push_back({});
        for (const auto& pair : values) {
            rows[0].push_back(pair.first);
        }
    } else {
        std::vector<std::string>& header = rows[0];
        for (const auto& pair : values) {
            if (std::find(header.begin(), header.end(), pair.first) == header.end()) {
                header.push_back(pair.first);
                for (size_t i = 1; i < rows.size(); i++) {
                    rows[i].push_back("");
                }
            }
        }
    }

    std::vector<std::string> row(rows[0].size(), "");
    for (const auto& pair : values) {
        const auto it = std::find(rows[0].begin(), rows[0].end(), pair.first);
        if (it != rows[0].end()) {
            row[static_cast<size_t>(it - rows[0].begin())] = pair.second;
        }
    }
    rows.push_back(std::move(row));

    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        throw std::runtime_error("cannot write result csv: " + path);
    }
    for (const std::vector<std::string>& line : rows) {
        for (size_t i = 0; i < line.size(); i++) {
            if (i != 0) {
                out << ",";
            }
            out << line[i];
        }
        out << "\n";
    }
}

void write_result_csv(
        const std::string& path,
        const Args& args,
        const BaselineTotals& totals,
        uint32_t nq,
        uint32_t k2) {
    auto format_double = [](double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << value;
        return oss.str();
    };

    const uint64_t total_comm_bytes = totals.cluster_comm_bytes + totals.exact_comm_bytes;
    const uint64_t total_rtts = totals.cluster_net_round_trips + totals.exact_net_round_trips;
    std::vector<std::pair<std::string, std::string>> values;
    values.push_back({"scheme", "encrypted_baseline"});
    values.push_back({"nprobe", std::to_string(args.nprobe)});
    values.push_back({"k2", std::to_string(k2)});
    values.push_back({"recall_k", format_double(totals.recall / nq)});
    values.push_back({"Mean query latency/ms", format_double(sec_to_ms(totals.query_time_sec) / nq)});
    values.push_back({"Cluster read time/query/ms", format_double(sec_to_ms(totals.cluster_access_time_sec) / nq)});
    values.push_back({"Exact read time/query/ms", format_double(sec_to_ms(totals.exact_access_time_sec) / nq)});
    values.push_back({"Cluster read calls/query", format_double(static_cast<double>(totals.cluster_accesses) / nq)});
    values.push_back({"Cluster time/read call/ms", format_double(avg_ms_per_call(totals.cluster_access_time_sec, totals.cluster_accesses))});
    values.push_back({"Exact read calls/query", format_double(static_cast<double>(totals.exact_accesses) / nq)});
    values.push_back({"Exact time/read call/ms", format_double(avg_ms_per_call(totals.exact_access_time_sec, totals.exact_accesses))});
    values.push_back({"Communication/query/MB", format_double(static_cast<double>(total_comm_bytes) / nq / (1000.0 * 1000.0))});
    values.push_back({"Cluster communication/query/MB", format_double(static_cast<double>(totals.cluster_comm_bytes) / nq / (1000.0 * 1000.0))});
    values.push_back({"Exact communication/query/MB", format_double(static_cast<double>(totals.exact_comm_bytes) / nq / (1000.0 * 1000.0))});
    values.push_back({"Total round trips/query", format_double(static_cast<double>(total_rtts) / nq)});
    values.push_back({"Cluster read round trips/query", format_double(static_cast<double>(totals.cluster_net_round_trips) / nq)});
    values.push_back({"Exact read round trips/query", format_double(static_cast<double>(totals.exact_net_round_trips) / nq)});
    values.push_back({"ANN centroid selection time/query/ms", format_double(sec_to_ms(totals.ann_centroid_select_time_sec) / nq)});
    values.push_back({"ANN quantized read time/query/ms", format_double(sec_to_ms(totals.ann_cluster_read_time_sec) / nq)});
    values.push_back({"ANN quantized scan time/query/ms", format_double(sec_to_ms(totals.ann_approx_scan_time_sec) / nq)});
    values.push_back({"ANN candidate selection time/query/ms", format_double(sec_to_ms(totals.ann_candidate_select_time_sec) / nq)});
    values.push_back({"ANN exact read time/query/ms", format_double(sec_to_ms(totals.ann_exact_read_time_sec) / nq)});
    values.push_back({"ANN exact distance time/query/ms", format_double(sec_to_ms(totals.ann_exact_compute_time_sec) / nq)});
    values.push_back({"Mean quantized distance count", format_double(static_cast<double>(totals.approximate_distance_count) / nq)});
    values.push_back({"Mean exact distance count", format_double(static_cast<double>(totals.exact_distance_count) / nq)});
    append_result_csv_row(path, values);
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
class EncryptedBaselineServer {
public:
    using Index = IvfRabitQIndex<D, B, QueryBits>;
    using ApproxLayout = ApproxOramBlockLayout<D, B>;

    explicit EncryptedBaselineServer(
            BaselineExactPayloadFormat exact_format,
            size_t exact_plain_bytes,
            NetworkProfile network_profile)
        : exact_format_(exact_format),
          exact_plain_bytes_(exact_plain_bytes),
          network_profile_(network_profile) {}

    void build_from_index(const Args& args, const Index& index, const std::string& cache_path) {
        exact_cipher_bytes_ = static_cast<size_t>(compute_ctx_len(static_cast<int>(exact_plain_bytes_)));
        cluster_plain_bytes_.assign(index.num_clusters, 0);
        cluster_ciphertexts_.assign(index.num_clusters, {});
        exact_ciphertexts_.assign(static_cast<size_t>(index.n) * exact_cipher_bytes_, 0);

        std::vector<float> cluster_means =
            compute_cluster_means_if_needed(index, exact_format_);
        log_step("building encrypted baseline low-precision cluster payloads");
        for (uint32_t c = 0; c < index.num_clusters; c++) {
            if (c == 0 || (c + 1) % 128 == 0 || c + 1 == index.num_clusters) {
                log_progress("building baseline clusters " + std::to_string(c + 1) +
                             " / " + std::to_string(index.num_clusters));
            }
            std::vector<uint8_t> plain =
                build_cluster_plaintext(index, cluster_means, exact_format_, c);
            cluster_plain_bytes_[c] = plain.size();
            cluster_ciphertexts_[c] = encrypt_plain(plain);
        }
        log_progress("building baseline clusters complete: " +
                     std::to_string(index.num_clusters) + " clusters", true);

        log_step("building encrypted baseline high-precision payloads");
        for (uint32_t c = 0; c < index.num_clusters; c++) {
            if (c == 0 || (c + 1) % 128 == 0 || c + 1 == index.num_clusters) {
                log_progress("building baseline exact payloads cluster " + std::to_string(c + 1) +
                             " / " + std::to_string(index.num_clusters));
            }
            for (uint32_t local_id = 0; local_id < index.len[c]; local_id++) {
                const uint32_t global_offset = index.start[c] + local_id;
                const uint32_t vector_id = index.ids[global_offset];
                if (vector_id >= index.n) {
                    throw std::runtime_error("vector id is out of range while building baseline exact payloads");
                }
                const std::vector<uint8_t> plain =
                    build_exact_payload(index, cluster_means, exact_format_, c, local_id);
                if (plain.size() != exact_plain_bytes_) {
                    throw std::runtime_error("exact plaintext size does not match baseline format");
                }
                const std::vector<uint8_t> cipher = encrypt_plain(plain);
                if (cipher.size() != exact_cipher_bytes_) {
                    throw std::runtime_error("exact ciphertext size does not match baseline format");
                }
                std::memcpy(
                    exact_ciphertexts_.data() + static_cast<size_t>(vector_id) * exact_cipher_bytes_,
                    cipher.data(),
                    exact_cipher_bytes_);
            }
        }
        log_progress("building baseline exact payloads complete: " +
                     std::to_string(index.num_clusters) + " clusters", true);

        save_cache(args, index, cache_path);
    }

    void load_cache(
            const Args& args,
            const Index& index,
            const std::string& cache_path) {
        (void)args;
        std::ifstream in(cache_path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot read encrypted baseline cache: " + cache_path);
        }

        uint32_t magic = 0;
        uint32_t version = 0;
        uint32_t dim = 0;
        uint32_t bits = 0;
        uint32_t query_bits = 0;
        uint32_t n = 0;
        uint32_t clusters = 0;
        uint32_t exact_format = 0;
        uint64_t exact_plain = 0;
        uint64_t exact_cipher = 0;
        read_pod(in, magic, "baseline cache magic");
        read_pod(in, version, "baseline cache version");
        read_pod(in, dim, "baseline cache dimension");
        read_pod(in, bits, "baseline cache bits");
        read_pod(in, query_bits, "baseline cache query bits");
        read_pod(in, n, "baseline cache n");
        read_pod(in, clusters, "baseline cache clusters");
        read_pod(in, exact_format, "baseline cache exact format");
        read_pod(in, exact_plain, "baseline cache exact plaintext bytes");
        read_pod(in, exact_cipher, "baseline cache exact ciphertext bytes");
        if (magic != kBaselineCacheMagic ||
                version != kBaselineCacheVersion ||
                dim != D ||
                bits != B ||
                query_bits != QueryBits ||
                n != index.n ||
                clusters != index.num_clusters ||
                exact_format != static_cast<uint32_t>(exact_format_) ||
                exact_plain != exact_plain_bytes_) {
            throw std::runtime_error("encrypted baseline cache does not match current parameters");
        }
        exact_cipher_bytes_ = static_cast<size_t>(exact_cipher);

        uint64_t cluster_count = 0;
        read_pod(in, cluster_count, "baseline cache cluster count");
        if (cluster_count != index.num_clusters) {
            throw std::runtime_error("encrypted baseline cache cluster count mismatch");
        }
        cluster_plain_bytes_.assign(index.num_clusters, 0);
        cluster_ciphertexts_.assign(index.num_clusters, {});
        for (uint32_t c = 0; c < index.num_clusters; c++) {
            uint64_t plain_bytes = 0;
            uint64_t cipher_bytes = 0;
            read_pod(in, plain_bytes, "cluster plaintext bytes");
            read_pod(in, cipher_bytes, "cluster ciphertext bytes");
            cluster_plain_bytes_[c] = static_cast<size_t>(plain_bytes);
            cluster_ciphertexts_[c].assign(static_cast<size_t>(cipher_bytes), 0);
            in.read(reinterpret_cast<char*>(cluster_ciphertexts_[c].data()),
                    static_cast<std::streamsize>(cipher_bytes));
            if (!in) {
                throw std::runtime_error("short read in encrypted baseline cluster payload");
            }
        }

        uint64_t exact_count = 0;
        read_pod(in, exact_count, "baseline exact count");
        if (exact_count != index.n) {
            throw std::runtime_error("encrypted baseline exact count mismatch");
        }
        exact_ciphertexts_.assign(static_cast<size_t>(exact_count) * exact_cipher_bytes_, 0);
        in.read(reinterpret_cast<char*>(exact_ciphertexts_.data()),
                static_cast<std::streamsize>(exact_ciphertexts_.size()));
        if (!in) {
            throw std::runtime_error("short read in encrypted baseline exact payloads");
        }
    }

    void save_cache(const Args& args, const Index& index, const std::string& cache_path) const {
        ensure_directory(cache_directory(args));
        log_step("saving encrypted baseline cache: " + cache_path);
        std::ofstream out(cache_path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write encrypted baseline cache: " + cache_path);
        }
        const uint32_t magic = kBaselineCacheMagic;
        const uint32_t version = kBaselineCacheVersion;
        const uint32_t dim = D;
        const uint32_t bits = B;
        const uint32_t query_bits = QueryBits;
        const uint32_t exact_format = static_cast<uint32_t>(exact_format_);
        const uint64_t exact_plain = static_cast<uint64_t>(exact_plain_bytes_);
        const uint64_t exact_cipher = static_cast<uint64_t>(exact_cipher_bytes_);
        write_pod(out, magic, "baseline cache magic");
        write_pod(out, version, "baseline cache version");
        write_pod(out, dim, "baseline cache dimension");
        write_pod(out, bits, "baseline cache bits");
        write_pod(out, query_bits, "baseline cache query bits");
        write_pod(out, index.n, "baseline cache n");
        write_pod(out, index.num_clusters, "baseline cache clusters");
        write_pod(out, exact_format, "baseline cache exact format");
        write_pod(out, exact_plain, "baseline cache exact plaintext bytes");
        write_pod(out, exact_cipher, "baseline cache exact ciphertext bytes");
        const uint64_t cluster_count = static_cast<uint64_t>(cluster_ciphertexts_.size());
        write_pod(out, cluster_count, "baseline cache cluster count");
        for (size_t c = 0; c < cluster_ciphertexts_.size(); c++) {
            const uint64_t plain_bytes = static_cast<uint64_t>(cluster_plain_bytes_[c]);
            const uint64_t cipher_bytes = static_cast<uint64_t>(cluster_ciphertexts_[c].size());
            write_pod(out, plain_bytes, "cluster plaintext bytes");
            write_pod(out, cipher_bytes, "cluster ciphertext bytes");
            out.write(reinterpret_cast<const char*>(cluster_ciphertexts_[c].data()),
                      static_cast<std::streamsize>(cluster_ciphertexts_[c].size()));
            if (!out) {
                throw std::runtime_error("failed writing encrypted cluster payload");
            }
        }
        const uint64_t exact_count = index.n;
        write_pod(out, exact_count, "baseline exact count");
        out.write(reinterpret_cast<const char*>(exact_ciphertexts_.data()),
                  static_cast<std::streamsize>(exact_ciphertexts_.size()));
        if (!out) {
            throw std::runtime_error("failed writing encrypted baseline exact payloads");
        }
    }

    void reset_query_stats() {
        query_stats_ = QueryNetworkStats{};
        std::fill(cluster_mean_valid_.begin(), cluster_mean_valid_.end(), uint8_t{0});
    }

    const QueryNetworkStats& query_stats() const {
        return query_stats_;
    }

    std::vector<AnnApproxBlock> read_clusters(const std::vector<uint32_t>& cluster_ids) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        uint64_t bytes = static_cast<uint64_t>(cluster_ids.size()) * sizeof(uint32_t);
        size_t block_count = 0;
        for (uint32_t cluster_id : cluster_ids) {
            if (cluster_id >= cluster_ciphertexts_.size()) {
                throw std::out_of_range("baseline cluster id is out of range");
            }
            bytes += cluster_ciphertexts_[cluster_id].size();
        }
        const auto net = simulate_network_transfer(network_profile_, bytes, 1);
        query_stats_.cluster_comm_bytes += bytes;
        query_stats_.cluster_net_round_trips += net.round_trips;
        query_stats_.net_total_time_sec += net.total_time_sec;

        std::vector<AnnApproxBlock> out;
        for (uint32_t cluster_id : cluster_ids) {
            const std::vector<uint8_t> plain =
                decrypt_cipher(cluster_ciphertexts_[cluster_id], cluster_plain_bytes_[cluster_id]);
            block_count += parse_cluster_plaintext(cluster_id, plain, out);
        }
        (void)block_count;

        double elapsed = elapsed_sec_since(t0);
        if (network_profile_.enabled && !network_profile_.sleep) {
            elapsed += net.total_time_sec;
        }
        query_stats_.cluster_access_time_sec += elapsed;
        query_stats_.cluster_accesses++;
        return out;
    }

    std::vector<AnnExactVector> read_exact_vectors(const std::vector<uint32_t>& vector_ids) {
        const auto t0 = std::chrono::high_resolution_clock::now();
        const uint64_t bytes =
            static_cast<uint64_t>(vector_ids.size()) *
            (sizeof(uint32_t) + static_cast<uint64_t>(exact_cipher_bytes_));
        const auto net = simulate_network_transfer(network_profile_, bytes, 1);
        query_stats_.exact_comm_bytes += bytes;
        query_stats_.exact_net_round_trips += net.round_trips;
        query_stats_.net_total_time_sec += net.total_time_sec;

        std::vector<AnnExactVector> out;
        out.reserve(vector_ids.size());
        for (uint32_t vector_id : vector_ids) {
            if (static_cast<size_t>(vector_id) >= exact_ciphertexts_.size() / exact_cipher_bytes_) {
                continue;
            }
            const uint8_t* cipher =
                exact_ciphertexts_.data() + static_cast<size_t>(vector_id) * exact_cipher_bytes_;
            AnnExactVector exact;
            exact.vector_id = vector_id;
            exact.payload = decrypt_cipher_span(cipher, exact_cipher_bytes_, exact_plain_bytes_);
            out.push_back(std::move(exact));
        }

        double elapsed = elapsed_sec_since(t0);
        if (network_profile_.enabled && !network_profile_.sleep) {
            elapsed += net.total_time_sec;
        }
        query_stats_.exact_access_time_sec += elapsed;
        query_stats_.exact_accesses++;
        return out;
    }

    const float* cluster_mean_or_null(uint32_t) const { return nullptr; }

    BaselineExactPayloadFormat exact_format() const {
        return exact_format_;
    }

    size_t exact_plain_bytes() const {
        return exact_plain_bytes_;
    }

    uint64_t server_cluster_bytes() const {
        uint64_t total = 0;
        for (const std::vector<uint8_t>& cipher : cluster_ciphertexts_) {
            total += cipher.size();
        }
        return total;
    }

    uint64_t server_exact_bytes() const {
        return static_cast<uint64_t>(exact_ciphertexts_.size());
    }

private:
    BaselineExactPayloadFormat exact_format_;
    size_t exact_plain_bytes_ = 0;
    size_t exact_cipher_bytes_ = 0;
    NetworkProfile network_profile_;
    std::vector<size_t> cluster_plain_bytes_;
    std::vector<std::vector<uint8_t>> cluster_ciphertexts_;
    std::vector<uint8_t> exact_ciphertexts_;
    std::vector<float> cluster_means_;
    std::vector<uint8_t> cluster_mean_valid_;
    QueryNetworkStats query_stats_;

    size_t parse_cluster_plaintext(
            uint32_t expected_cluster_id,
            const std::vector<uint8_t>& plain,
            std::vector<AnnApproxBlock>& out) {
        if (cluster_means_.empty()) {
            cluster_means_.assign(cluster_ciphertexts_.size() * static_cast<size_t>(D), 0.0f);
            cluster_mean_valid_.assign(cluster_ciphertexts_.size(), 0);
        }

        size_t offset = 0;
        const uint32_t magic = read_pod_from_buffer<uint32_t>(plain, offset, "cluster magic");
        const uint32_t cluster_id = read_pod_from_buffer<uint32_t>(plain, offset, "cluster id");
        const uint32_t cluster_len = read_pod_from_buffer<uint32_t>(plain, offset, "cluster length");
        const uint32_t num_blocks = read_pod_from_buffer<uint32_t>(plain, offset, "cluster block count");
        (void)cluster_len;
        if (magic != kClusterPlainMagic || cluster_id != expected_cluster_id) {
            throw std::runtime_error("encrypted baseline cluster payload has mismatched header");
        }
        if (offset + static_cast<size_t>(D) * sizeof(float) > plain.size()) {
            throw std::runtime_error("encrypted baseline cluster payload is truncated in cluster mean");
        }
        std::memcpy(cluster_means_.data() + static_cast<size_t>(cluster_id) * D,
                    plain.data() + offset,
                    static_cast<size_t>(D) * sizeof(float));
        cluster_mean_valid_[cluster_id] = 1;
        offset += static_cast<size_t>(D) * sizeof(float);

        const size_t before = out.size();
        for (uint32_t i = 0; i < num_blocks; i++) {
            AnnApproxBlock block;
            block.cluster_id = cluster_id;
            block.block_id = read_pod_from_buffer<uint32_t>(plain, offset, "block id");
            const uint32_t active = read_pod_from_buffer<uint32_t>(plain, offset, "block active count");
            if (active > IvfRabitQIndex<D, B, QueryBits>::kBlockVectors) {
                throw std::runtime_error("encrypted baseline cluster payload has invalid active count");
            }
            block.vector_ids.assign(IvfRabitQIndex<D, B, QueryBits>::kBlockVectors, static_cast<uint32_t>(-1));
            const size_t vector_id_bytes = block.vector_ids.size() * sizeof(uint32_t);
            if (offset + vector_id_bytes > plain.size()) {
                throw std::runtime_error("encrypted baseline cluster payload is truncated in vector ids");
            }
            std::memcpy(block.vector_ids.data(), plain.data() + offset, vector_id_bytes);
            offset += vector_id_bytes;
            if (offset + ApproxLayout::kPlainBytes > plain.size()) {
                throw std::runtime_error("encrypted baseline cluster payload is truncated in approximate payload");
            }
            block.payload.assign(plain.begin() + static_cast<std::ptrdiff_t>(offset),
                                 plain.begin() + static_cast<std::ptrdiff_t>(offset + ApproxLayout::kPlainBytes));
            offset += ApproxLayout::kPlainBytes;
            out.push_back(std::move(block));
        }
        if (offset != plain.size()) {
            throw std::runtime_error("encrypted baseline cluster payload has trailing bytes");
        }
        return out.size() - before;
    }
};

template <uint32_t D, uint32_t B, uint32_t QueryBits>
class BaselineSearcher {
public:
    using Index = IvfRabitQIndex<D, B, QueryBits>;
    using Factor = typename Index::Factor;
    using SpaceT = Space<D, B, QueryBits>;

    explicit BaselineSearcher(const Index& index)
        : index_(index) {}

    ResultHeap search(
            const float* query,
            const float* randomized_query,
            uint32_t k,
            uint32_t nprobe,
            uint32_t path_batch_clusters,
            uint32_t exact_candidate_limit,
            EncryptedBaselineServer<D, B, QueryBits>& server,
            double dist_k = std::numeric_limits<float>::max()) {
        reset_counters();
        ResultHeap knns;
        if (index_.num_clusters == 0 || k == 0 || nprobe == 0) {
            return knns;
        }
        nprobe = std::min(nprobe, index_.num_clusters);
        const size_t candidate_limit = exact_candidate_limit == 0
            ? static_cast<size_t>(k) * nprobe
            : exact_candidate_limit;
        if (candidate_limit == 0) {
            return knns;
        }

        struct ApproxCandidate {
            float quantized_distance = 0.0f;
            uint32_t cluster_id = 0;
            uint32_t local_vector_id = 0;
            uint32_t vector_id = 0;
        };
        auto candidate_less = [](const ApproxCandidate& lhs, const ApproxCandidate& rhs) {
            if (lhs.quantized_distance != rhs.quantized_distance) {
                return lhs.quantized_distance < rhs.quantized_distance;
            }
            if (lhs.vector_id != rhs.vector_id) {
                return lhs.vector_id < rhs.vector_id;
            }
            return lhs.local_vector_id < rhs.local_vector_id;
        };

        std::vector<Result> centroid_dist(index_.num_clusters);
        {
            const auto t = std::chrono::high_resolution_clock::now();
            for (uint32_t c = 0; c < index_.num_clusters; c++) {
                centroid_dist[c].first =
                    sqr_dist<B>(randomized_query,
                                index_.randomized_centroids.data() + static_cast<size_t>(c) * B);
                centroid_dist[c].second = c;
            }
            std::partial_sort(centroid_dist.begin(), centroid_dist.begin() + nprobe, centroid_dist.end());
            counters_.centroid_select_time_sec += elapsed_sec_since(t);
        }

        std::vector<ApproxCandidate> top_candidates;
        top_candidates.reserve(candidate_limit);
        auto keep_candidate = [&](const ApproxCandidate& candidate) {
            if (top_candidates.size() < candidate_limit) {
                top_candidates.push_back(candidate);
                std::push_heap(top_candidates.begin(), top_candidates.end(), candidate_less);
                return;
            }
            if (candidate_less(candidate, top_candidates.front())) {
                std::pop_heap(top_candidates.begin(), top_candidates.end(), candidate_less);
                top_candidates.back() = candidate;
                std::push_heap(top_candidates.begin(), top_candidates.end(), candidate_less);
            }
        };

        uint8_t PORTABLE_ALIGN64 byte_query[B];
        for (uint32_t batch_begin = 0; batch_begin < nprobe; batch_begin += path_batch_clusters) {
            const uint32_t batch_end = std::min<uint32_t>(nprobe, batch_begin + path_batch_clusters);
            std::vector<uint32_t> cluster_batch;
            cluster_batch.reserve(batch_end - batch_begin);
            for (uint32_t pi = batch_begin; pi < batch_end; pi++) {
                const uint32_t c = centroid_dist[pi].second;
                if (index_.len[c] != 0) {
                    cluster_batch.push_back(c);
                }
            }
            if (cluster_batch.empty()) {
                continue;
            }

            const double cluster_read_before = server.query_stats().cluster_access_time_sec;
            std::vector<AnnApproxBlock> approx_blocks = server.read_clusters(cluster_batch);
            counters_.cluster_read_time_sec +=
                server.query_stats().cluster_access_time_sec - cluster_read_before;

            std::unordered_map<uint64_t, AnnApproxBlock*> blocks_by_id;
            blocks_by_id.reserve(approx_blocks.size());
            for (AnnApproxBlock& block : approx_blocks) {
                const uint64_t key = (static_cast<uint64_t>(block.cluster_id) << 32) | block.block_id;
                blocks_by_id[key] = &block;
            }

            std::vector<ApproxCandidate> batch_candidates;
            const auto t_approx = std::chrono::high_resolution_clock::now();
            for (uint32_t c : cluster_batch) {
                float centroid_sqr = 0.0f;
                for (uint32_t pi = batch_begin; pi < batch_end; pi++) {
                    if (centroid_dist[pi].second == c) {
                        centroid_sqr = centroid_dist[pi].first;
                        break;
                    }
                }

                float vl = 0.0f;
                float vr = 0.0f;
                SpaceT::range(
                    randomized_query,
                    index_.randomized_centroids.data() + static_cast<size_t>(c) * B,
                    vl,
                    vr);
                const float width = (vr - vl) / static_cast<float>((1u << QueryBits) - 1u);
                uint32_t sum_q = 0;
                SpaceT::quantize(
                    byte_query,
                    randomized_query,
                    index_.randomized_centroids.data() + static_cast<size_t>(c) * B,
                    index_.query_quantization_offsets.data(),
                    vl,
                    width,
                    sum_q);
                uint8_t PORTABLE_ALIGN32 lut[B / 4 * 16];
                pack_lut_4bit<B>(byte_query, lut);
                for (uint32_t i = 0; i < B / 4 * 16; i++) {
                    lut[i] = static_cast<uint8_t>(lut[i] * 2);
                }

                const uint32_t num_blocks = static_cast<uint32_t>(index_.cluster_num_blocks(c));
                for (uint32_t block_id = 0; block_id < num_blocks; block_id++) {
                    const uint64_t key = (static_cast<uint64_t>(c) << 32) | block_id;
                    auto it = blocks_by_id.find(key);
                    if (it == blocks_by_id.end()) {
                        continue;
                    }
                    collect_candidates(
                        c,
                        block_id,
                        index_.len[c],
                        lut,
                        centroid_sqr,
                        vl,
                        width,
                        static_cast<float>(sum_q),
                        *it->second,
                        batch_candidates);
                }
            }
            counters_.approx_scan_time_sec += elapsed_sec_since(t_approx);

            if (!batch_candidates.empty()) {
                const auto t_select = std::chrono::high_resolution_clock::now();
                for (const ApproxCandidate& candidate : batch_candidates) {
                    keep_candidate(candidate);
                }
                counters_.candidate_select_time_sec += elapsed_sec_since(t_select);
            }
        }

        if (!top_candidates.empty()) {
            {
                const auto t_select = std::chrono::high_resolution_clock::now();
                std::sort_heap(top_candidates.begin(), top_candidates.end(), candidate_less);
                counters_.candidate_select_time_sec += elapsed_sec_since(t_select);
            }
            std::vector<uint32_t> exact_vector_ids;
            exact_vector_ids.reserve(top_candidates.size());
            for (const ApproxCandidate& candidate : top_candidates) {
                exact_vector_ids.push_back(candidate.vector_id);
            }

            const double exact_read_before = server.query_stats().exact_access_time_sec;
            std::vector<AnnExactVector> exact_reads = server.read_exact_vectors(exact_vector_ids);
            counters_.exact_read_time_sec +=
                server.query_stats().exact_access_time_sec - exact_read_before;
            counters_.exact_distance_count += exact_reads.size();

            std::unordered_map<uint32_t, std::vector<uint8_t>> exact_by_id;
            exact_by_id.reserve(exact_reads.size());
            for (AnnExactVector& exact : exact_reads) {
                exact_by_id[exact.vector_id] = std::move(exact.payload);
            }

            const auto t_exact_compute = std::chrono::high_resolution_clock::now();
            for (const ApproxCandidate& candidate : top_candidates) {
                auto exact_it = exact_by_id.find(candidate.vector_id);
                if (exact_it == exact_by_id.end()) {
                    continue;
                }
                double exact_dist = 0.0;
                if (!compute_exact_distance(
                        query,
                        server.cluster_mean_or_null(candidate.cluster_id),
                        candidate.vector_id,
                        exact_it->second,
                        server.exact_format(),
                        exact_dist)) {
                    continue;
                }
                if (exact_dist < dist_k) {
                    knns.emplace(exact_dist, candidate.vector_id);
                    if (knns.size() > k) {
                        knns.pop();
                    }
                    if (knns.size() == k) {
                        dist_k = knns.top().first;
                    }
                }
            }
            counters_.exact_compute_time_sec += elapsed_sec_since(t_exact_compute);
        }
        return knns;
    }

    const SearchCounters& counters() const {
        return counters_;
    }

private:
    const Index& index_;
    SearchCounters counters_;

    void reset_counters() {
        counters_ = SearchCounters{};
    }

    template <typename Candidate>
    void collect_candidates(
            uint32_t cluster_id,
            uint32_t block_id,
            uint32_t cluster_len,
            const uint8_t* lut,
            float sqr_y,
            float vl,
            float width,
            float sum_q,
            const AnnApproxBlock& approx_block,
            std::vector<Candidate>& candidates) {
        using ApproxLayout = ApproxOramBlockLayout<D, B>;
        if (approx_block.payload.size() < ApproxLayout::kPlainBytes) {
            return;
        }
        const uint8_t* packed = approx_block.payload.data() + ApproxLayout::kFactorBytes;
        const auto* factors = reinterpret_cast<const Factor*>(approx_block.payload.data());
        uint16_t PORTABLE_ALIGN32 result[Index::kBlockVectors];
        accumulate<B>(1, packed, lut, result);

        const uint32_t block_begin = block_id * Index::kBlockVectors;
        const uint32_t active =
            block_begin >= cluster_len ? 0 : std::min<uint32_t>(Index::kBlockVectors, cluster_len - block_begin);
        for (uint32_t j = 0; j < active; j++) {
            counters_.approximate_distance_count++;
            if (j >= approx_block.vector_ids.size()) {
                continue;
            }
            const uint32_t vector_id = approx_block.vector_ids[j];
            if (vector_id == static_cast<uint32_t>(-1)) {
                continue;
            }
            const Factor& f = factors[j];
            const float quantized_distance =
                f.sqr_x + sqr_y + f.factor_ppc * vl + (result[j] - sum_q) * f.factor_ip * width;
            Candidate candidate;
            candidate.quantized_distance = quantized_distance;
            candidate.cluster_id = cluster_id;
            candidate.local_vector_id = block_begin + j;
            candidate.vector_id = vector_id;
            candidates.push_back(candidate);
        }
    }

    bool compute_exact_distance(
            const float* query,
            const float* exact_centroid,
            uint32_t expected_vector_id,
            const std::vector<uint8_t>& exact_plain,
            BaselineExactPayloadFormat format,
            double& exact_dist) const {
        if (format == BaselineExactPayloadFormat::RawUint8) {
            const size_t vector_bytes = D;
            if (exact_plain.size() < vector_bytes + sizeof(uint32_t)) {
                return false;
            }
            uint32_t stored_vector_id = 0;
            std::memcpy(&stored_vector_id, exact_plain.data() + vector_bytes, sizeof(stored_vector_id));
            if (stored_vector_id != expected_vector_id) {
                throw std::runtime_error("encrypted baseline raw uint8 payload returned a mismatched vector id");
            }
            double dist = 0.0;
            for (uint32_t d = 0; d < D; d++) {
                const double diff = double(query[d]) - double(exact_plain[d]);
                dist += diff * diff;
            }
            exact_dist = dist;
            return true;
        }

        if (format == BaselineExactPayloadFormat::RawFloat32) {
            const size_t vector_bytes = static_cast<size_t>(D) * sizeof(float);
            if (exact_plain.size() < vector_bytes + sizeof(uint32_t)) {
                return false;
            }
            uint32_t stored_vector_id = 0;
            std::memcpy(&stored_vector_id, exact_plain.data() + vector_bytes, sizeof(stored_vector_id));
            if (stored_vector_id != expected_vector_id) {
                throw std::runtime_error("encrypted baseline raw float32 payload returned a mismatched vector id");
            }
            const float* vector = reinterpret_cast<const float*>(exact_plain.data());
            double dist = 0.0;
            for (uint32_t d = 0; d < D; d++) {
                const double diff = double(query[d]) - double(vector[d]);
                dist += diff * diff;
            }
            exact_dist = dist;
            return true;
        }

        throw std::invalid_argument("unsupported exact payload format");
    }
};

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

template <uint32_t D, uint32_t B, uint32_t QueryBits>
void print_persistent_storage_stats(
        const IvfRabitQIndex<D, B, QueryBits>& index,
        const EncryptedBaselineServer<D, B, QueryBits>& server) {
    const uint64_t client_centroids_bytes =
        static_cast<uint64_t>(index.randomized_centroids.size()) * sizeof(float);
    const uint64_t server_cluster_bytes = server.server_cluster_bytes();
    const uint64_t server_exact_bytes = server.server_exact_bytes();
    const uint64_t server_total = server_cluster_bytes + server_exact_bytes;
    std::cout << "persistent_storage_begin\n";
    std::cout << "persistent_client_centroids_bytes " << client_centroids_bytes << "\n";
    std::cout << "persistent_client_total_bytes " << client_centroids_bytes << "\n";
    std::cout << "persistent_client_total_mib " << bytes_to_mib(client_centroids_bytes) << "\n";
    std::cout << "persistent_server_low_precision_bytes " << server_cluster_bytes << "\n";
    std::cout << "persistent_server_high_precision_bytes " << server_exact_bytes << "\n";
    std::cout << "persistent_server_total_bytes " << server_total << "\n";
    std::cout << "persistent_server_total_mib " << bytes_to_mib(server_total) << "\n";
    std::cout << "persistent_storage_end\n";
}

std::string make_storage_csv_path(const Args& args) {
    return args.results_dir + "/ivf_rabitq_storage.csv";
}

void write_persistent_storage_csv(
        const std::string& path,
        const std::string& dataset,
        uint64_t client_bytes,
        uint64_t server_low,
        uint64_t server_high) {
    const uint64_t server_total = server_low + server_high;
    auto existing = read_csv_rows(path);
    for (size_t i = 1; i < existing.size(); i++) {
        if (!existing[i].empty() && existing[i][0] == dataset) {
            return;
        }
    }

    auto format_double = [](double value) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6) << value;
        return oss.str();
    };
    std::ofstream out(path, std::ios::app);
    if (!out) {
        throw std::runtime_error("cannot write storage csv: " + path);
    }
    if (existing.empty()) {
        out << "dataset,client_storage_MB,client_storage_GB,"
               "server_low_precision_MB,server_low_precision_GB,"
               "server_high_precision_MB,server_high_precision_GB,"
               "server_total_MB,server_total_GB\n";
    }
    out << dataset << ","
        << format_double(bytes_to_mib(client_bytes)) << ","
        << format_double(bytes_to_gb(client_bytes)) << ","
        << format_double(bytes_to_mib(server_low)) << ","
        << format_double(bytes_to_gb(server_low)) << ","
        << format_double(bytes_to_mib(server_high)) << ","
        << format_double(bytes_to_gb(server_high)) << ","
        << format_double(bytes_to_mib(server_total)) << ","
        << format_double(bytes_to_gb(server_total)) << "\n";
}

template <uint32_t D, uint32_t B, uint32_t QueryBits>
void release_baseline_search_unused_index_arrays(IvfRabitQIndex<D, B, QueryBits>& index) {
    index.release_double_oram_search_unused_data();
}

template <uint32_t D, uint32_t B>
int run_for_dimension(Args args) {
    using Index = IvfRabitQIndex<D, B, kQueryBits>;

    log_step("loading IVF-RaBitQ index metadata: " + args.index_path);
    Index index;
    bool full_index_loaded = false;
    try {
        load_index_search_metadata_only(args.index_path, index);
    } catch (const std::exception& e) {
        log_step(std::string("metadata-only index load unavailable; loading full index: ") + e.what());
        index.load(args.index_path);
        full_index_loaded = true;
    }
    if (index.num_clusters != args.clusters) {
        if (args.assignment != "split") {
            throw std::runtime_error("index cluster count does not match --clusters");
        }
        log_step("resolved split cluster count from index: " + std::to_string(index.num_clusters));
        args.clusters = index.num_clusters;
    }

    const BaselineExactPayloadFormat exact_format =
        exact_payload_format_for_args(args, index.stores_raw_vectors_as_uint8());
    const size_t exact_bytes = exact_plain_bytes<D, B>(exact_format);
    const std::string cache_path =
        make_baseline_cache_path(args, index.n, D, B, exact_format, exact_bytes);
    const NetworkProfile network_profile = network_profile_from_args(args);

    EncryptedBaselineServer<D, B, kQueryBits> server(exact_format, exact_bytes, network_profile);
    log_step("network simulation: " + network_profile_label(args, network_profile));
    log_step("baseline exact payload format: " + exact_payload_format_tag(exact_format) +
             ", bytes: " + std::to_string(exact_bytes));
    log_step("checking encrypted baseline cache: " + cache_path);
    if (file_exists(cache_path)) {
        log_step("baseline cache hit; loading encrypted server data");
        server.load_cache(args, index, cache_path);
        if (full_index_loaded) {
            release_baseline_search_unused_index_arrays(index);
        }
    } else {
        if (!full_index_loaded) {
            log_step("baseline cache miss; loading full IVF-RaBitQ index for encrypted server build");
            Index full_index;
            full_index.load(args.index_path);
            if (full_index.num_clusters != args.clusters) {
                if (args.assignment != "split") {
                    throw std::runtime_error("index cluster count does not match --clusters");
                }
                args.clusters = full_index.num_clusters;
            }
            server.build_from_index(args, full_index, cache_path);
            index = std::move(full_index);
        } else {
            log_step("baseline cache miss; building from already loaded full index");
            server.build_from_index(args, index, cache_path);
        }
        release_baseline_search_unused_index_arrays(index);
    }

    log_step("loading query, groundtruth, and projection files");
    Matrix<float> query = read_float_vectors_file(args.files.path(args.source, args.dataset, "query"));
    Matrix<uint32_t> gt = read_uint32_vectors_file(args.files.path(args.source, args.dataset, "groundtruth"));
    Matrix<float> randomized_query;
    {
        const std::string projection_path = args.assignment == "split"
            ? args.source + "P_C" + std::to_string(args.clusters) +
                  "_B" + std::to_string(B) + ".fvecs"
            : args.source + "P_C" + std::to_string(args.clusters) +
                  "_assign" + sanitize_for_filename(args.assignment) +
                  "_B" + std::to_string(B) + ".fvecs";
        Matrix<float> projection(projection_path);
        log_step("computing randomized queries");
        randomized_query = multiply_padded_left(query, B, projection);
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
    std::cout << "assignment " << args.assignment << "\n";
    std::cout << "queries " << nq << "\n";
    std::cout << "topk " << args.k << "\n";
    std::cout << "k2_values " << join_uint32_values(args.k2_values) << "\n";
    std::cout << "nprobe_values " << join_uint32_values(args.nprobes) << "\n";
    std::cout << "cluster_batch_clusters " << args.path_batch_clusters << "\n";
    std::cout << "clusters " << index.num_clusters << "\n";
    std::cout << "dimension " << D << "\n";
    std::cout << "cluster_size_min " << *minmax_len.first << "\n";
    std::cout << "cluster_size_max " << *minmax_len.second << "\n";
    std::cout << "max_cluster_approx_blocks " << index.approx_blocks_per_cluster << "\n";
    std::cout << "baseline_exact_payload_format " << exact_payload_format_tag(exact_format) << "\n";
    std::cout << "network_simulation " << network_profile_label(args, network_profile) << "\n";
    ensure_directory(result_directory(args));
    print_persistent_storage_stats(index, server);
    write_persistent_storage_csv(
        make_storage_csv_path(args),
        args.dataset,
        static_cast<uint64_t>(index.randomized_centroids.size()) * sizeof(float),
        server.server_cluster_bytes(),
        server.server_exact_bytes());
    BaselineSearcher<D, B, kQueryBits> searcher(index);
    for (size_t nprobe_index = 0; nprobe_index < args.nprobes.size(); nprobe_index++) {
        Args run_args = args;
        run_args.nprobe = args.nprobes[nprobe_index];
        run_args.nprobes = {run_args.nprobe};
        const uint32_t k2 = args.k2_values.empty()
            ? derived_k2(run_args)
            : args.k2_values[nprobe_index];

        std::cout << "nprobe_run_begin " << (nprobe_index + 1)
                  << " / " << args.nprobes.size() << "\n";
        std::cout << "nprobe " << run_args.nprobe << "\n";
        std::cout << "k2 " << k2 << "\n";

        BaselineTotals totals;
        log_step("starting encrypted baseline queries for nprobe " + std::to_string(run_args.nprobe) +
                 " (" + std::to_string(nprobe_index + 1) + " / " +
                 std::to_string(args.nprobes.size()) + ")");
        for (uint32_t qi = 0; qi < nq; qi++) {
            log_progress("running nprobe " + std::to_string(run_args.nprobe) +
                         " query " + std::to_string(qi + 1) + " / " + std::to_string(nq));
            server.reset_query_stats();
            const auto t0 = std::chrono::high_resolution_clock::now();
            ResultHeap result = searcher.search(
                query.row(qi),
                randomized_query.row(qi),
                run_args.k,
                run_args.nprobe,
                run_args.path_batch_clusters,
                k2,
                server);
            const double query_wall_sec = elapsed_sec_since(t0);
            const QueryNetworkStats& qs = server.query_stats();
            const double query_sec = query_wall_sec +
                (network_profile.enabled && !network_profile.sleep ? qs.net_total_time_sec : 0.0);

            totals.query_time_sec += query_sec;
            totals.recall += recall_at_k(result, gt, qi, run_args.k);
            totals.cluster_access_time_sec += qs.cluster_access_time_sec;
            totals.exact_access_time_sec += qs.exact_access_time_sec;
            totals.cluster_accesses += qs.cluster_accesses;
            totals.exact_accesses += qs.exact_accesses;
            totals.cluster_comm_bytes += qs.cluster_comm_bytes;
            totals.exact_comm_bytes += qs.exact_comm_bytes;
            totals.cluster_net_round_trips += qs.cluster_net_round_trips;
            totals.exact_net_round_trips += qs.exact_net_round_trips;
            totals.approximate_distance_count += searcher.counters().approximate_distance_count;
            totals.exact_distance_count += searcher.counters().exact_distance_count;
            totals.ann_centroid_select_time_sec += searcher.counters().centroid_select_time_sec;
            totals.ann_cluster_read_time_sec += searcher.counters().cluster_read_time_sec;
            totals.ann_approx_scan_time_sec += searcher.counters().approx_scan_time_sec;
            totals.ann_candidate_select_time_sec += searcher.counters().candidate_select_time_sec;
            totals.ann_exact_read_time_sec += searcher.counters().exact_read_time_sec;
            totals.ann_exact_compute_time_sec += searcher.counters().exact_compute_time_sec;
        }
        log_progress("encrypted baseline queries complete for nprobe " + std::to_string(run_args.nprobe) +
                     ": " + std::to_string(nq) + " queries", true);

        const uint64_t total_comm_bytes = totals.cluster_comm_bytes + totals.exact_comm_bytes;
        const uint64_t total_rtts = totals.cluster_net_round_trips + totals.exact_net_round_trips;
        std::cout << "avg_query_time_ms " << sec_to_ms(totals.query_time_sec) / nq << "\n";
        std::cout << "avg_cluster_reads_per_query "
                  << static_cast<double>(totals.cluster_accesses) / nq << "\n";
        std::cout << "avg_exact_reads_per_query "
                  << static_cast<double>(totals.exact_accesses) / nq << "\n";
        std::cout << "avg_cluster_read_time_ms_per_query "
                  << sec_to_ms(totals.cluster_access_time_sec) / nq << "\n";
        std::cout << "avg_exact_read_time_ms_per_query "
                  << sec_to_ms(totals.exact_access_time_sec) / nq << "\n";
        std::cout << "avg_cluster_read_time_ms_per_read "
                  << avg_ms_per_call(totals.cluster_access_time_sec, totals.cluster_accesses) << "\n";
        std::cout << "avg_exact_read_time_ms_per_read "
                  << avg_ms_per_call(totals.exact_access_time_sec, totals.exact_accesses) << "\n";
        std::cout << "avg_ann_centroid_select_time_ms_per_query "
                  << sec_to_ms(totals.ann_centroid_select_time_sec) / nq << "\n";
        std::cout << "avg_ann_cluster_read_time_ms_per_query "
                  << sec_to_ms(totals.ann_cluster_read_time_sec) / nq << "\n";
        std::cout << "avg_ann_approx_scan_time_ms_per_query "
                  << sec_to_ms(totals.ann_approx_scan_time_sec) / nq << "\n";
        std::cout << "avg_ann_candidate_select_time_ms_per_query "
                  << sec_to_ms(totals.ann_candidate_select_time_sec) / nq << "\n";
        std::cout << "avg_ann_exact_read_time_ms_per_query "
                  << sec_to_ms(totals.ann_exact_read_time_sec) / nq << "\n";
        std::cout << "avg_ann_exact_compute_time_ms_per_query "
                  << sec_to_ms(totals.ann_exact_compute_time_sec) / nq << "\n";
        std::cout << "avg_comm_total_bytes_per_query "
                  << static_cast<double>(total_comm_bytes) / nq << "\n";
        std::cout << "avg_comm_cluster_bytes_per_query "
                  << static_cast<double>(totals.cluster_comm_bytes) / nq << "\n";
        std::cout << "avg_comm_exact_bytes_per_query "
                  << static_cast<double>(totals.exact_comm_bytes) / nq << "\n";
        std::cout << "avg_total_net_round_trips_per_query "
                  << static_cast<double>(total_rtts) / nq << "\n";
        std::cout << "avg_cluster_net_round_trips_per_query "
                  << static_cast<double>(totals.cluster_net_round_trips) / nq << "\n";
        std::cout << "avg_exact_net_round_trips_per_query "
                  << static_cast<double>(totals.exact_net_round_trips) / nq << "\n";
        std::cout << "recall@" << run_args.k << " " << totals.recall / nq << "\n";
        std::cout << "avg_approx_distance_count "
                  << static_cast<double>(totals.approximate_distance_count) / nq << "\n";
        std::cout << "avg_exact_distance_count "
                  << static_cast<double>(totals.exact_distance_count) / nq << "\n";

        const std::string result_csv_path = make_result_csv_path(run_args);
        write_result_csv(result_csv_path, run_args, totals, nq, k2);
        std::cout << "result_csv " << result_csv_path << "\n";
        std::cout << "nprobe_run_end " << run_args.nprobe << "\n";
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        const IndexHeader header = read_index_header(args.index_path);
        if (header.dim == 128 && header.bits == 128) {
            return run_for_dimension<128, 128>(args);
        }
        if (header.dim == 96 && header.bits == 128) {
            return run_for_dimension<96, 128>(args);
        }
        if (header.dim == 768 && header.bits == 768) {
            return run_for_dimension<768, 768>(args);
        }
        throw std::runtime_error("unsupported index dimension/bits: dim=" + std::to_string(header.dim) +
                                 " B=" + std::to_string(header.bits));
    } catch (const std::exception& e) {
        finish_progress_line();
        std::cerr << "test_ivf_rabitq_encrypted_baseline failed: " << e.what() << "\n";
        return 1;
    }
}
