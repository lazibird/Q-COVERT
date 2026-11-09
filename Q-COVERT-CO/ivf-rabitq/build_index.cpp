#include "repository_paths.h"
#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ivf_rabitq_index.h"
#include "simple_json.h"

#ifndef DIM
#define DIM 128
#endif

#ifndef BB
#define BB 128
#endif

#ifndef B_QUERY
#define B_QUERY 4
#endif

namespace {
struct Args {
    repository_paths::DatasetFiles files;
    std::string dataset;
    std::string source;
    uint32_t clusters = 0;
    std::string output;
};

void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --dataset sift --source /path/to/data/sift [--clusters 4096]"
              << " [--output index_file]"
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

bool starts_with(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string assignment_manifest_path(
        const std::string& source,
        const std::string& dataset,
        uint32_t requested_clusters) {
    return source + dataset + "_assignment_" + std::to_string(requested_clusters) + ".txt";
}

uint32_t read_manifest_clusters(const Args& args) {
    const std::string path = assignment_manifest_path(
        args.source,
        args.dataset,
        args.clusters);
    std::ifstream in(path);
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

bool has_build_inputs(const Args& args, uint32_t clusters) {
    const std::string c = std::to_string(clusters);
    return matrix_file_exists(args.source + "RandCentroid_C" + c + "_B" + std::to_string(BB) + ".fvecs") &&
           matrix_file_exists(args.source + "x0_C" + c + "_B" + std::to_string(BB) + ".fvecs") &&
           matrix_file_exists(args.source + args.dataset + "_dist_to_centroid_" + c + ".fvecs") &&
           matrix_file_exists(args.source + args.dataset + "_cluster_id_" + c + ".ivecs") &&
           matrix_file_exists(args.source + "RandNet_C" + c + "_B" + std::to_string(BB) + ".Ivecs");
}

std::vector<uint32_t> find_build_input_clusters(const Args& args) {
    std::vector<uint32_t> clusters;
    DIR* dir = opendir(args.source.c_str());
    if (dir == nullptr) {
        return clusters;
    }
    const std::string prefix = "RandCentroid_C";
    const std::string suffix = "_B" + std::to_string(BB) + ".fvecs";
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (!starts_with(name, prefix) || !ends_with(name, suffix)) {
            continue;
        }
        const std::string cluster_text = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (cluster_text.empty() ||
                !std::all_of(cluster_text.begin(), cluster_text.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
            continue;
        }
        const uint32_t candidate = static_cast<uint32_t>(std::stoul(cluster_text));
        if (has_build_inputs(args, candidate)) {
            clusters.push_back(candidate);
        }
    }
    closedir(dir);
    std::sort(clusters.begin(), clusters.end());
    clusters.erase(std::unique(clusters.begin(), clusters.end()), clusters.end());
    return clusters;
}

uint32_t resolve_build_clusters(const Args& args) {
    const uint32_t manifest_clusters = read_manifest_clusters(args);
    if (manifest_clusters != 0) {
        return manifest_clusters;
    }
    if (has_build_inputs(args, args.clusters)) {
        return args.clusters;
    }
    std::vector<uint32_t> candidates = find_build_input_clusters(args);
    if (candidates.size() == 1) {
        return candidates.front();
    }
    if (!candidates.empty()) {
        throw std::runtime_error("multiple input cluster counts found; pass the actual --clusters value");
    }
    return args.clusters;
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

bool cli_provided(const std::set<std::string>& provided, const std::string& key) {
    return provided.find(normalize_config_key(key)) != provided.end();
}

uint32_t config_uint32(const simple_json::Value& value, const std::string& path) {
    const double number = simple_json::as_number(value, path);
    if (number < 0.0 || number > static_cast<double>(std::numeric_limits<uint32_t>::max()) ||
            std::floor(number) != number) {
        throw std::runtime_error("config value must be a non-negative uint32: " + path);
    }
    return static_cast<uint32_t>(number);
}

void apply_build_config_section(
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
            args.source = simple_json::as_string(value, value_path);
        } else if (key == "clusters" && !cli_provided(provided, "clusters")) {
            args.clusters = config_uint32(value, value_path);
        } else if ((key == "output" || key == "index" || key == "index_path") &&
                   !cli_provided(provided, "output")) {
            args.output = simple_json::as_string(value, value_path);
        }
    }
}

void apply_build_config(
        Args& args,
        const simple_json::Value& root,
        const std::set<std::string>& provided) {
    if (!root.is_object()) {
        throw std::runtime_error("config root must be a JSON object");
    }
    if (!cli_provided(provided, "dataset")) {
        if (const simple_json::Value* default_dataset = root.get("default_dataset")) {
            args.dataset = simple_json::as_string(*default_dataset, "default_dataset");
        }
    }
    if (const simple_json::Value* defaults_build = simple_json::get_path(root, {"defaults", "build_index"})) {
        apply_build_config_section(args, defaults_build, provided, "defaults.build_index");
    }
    const simple_json::Value* dataset_section =
        simple_json::get_path(root, {"datasets", args.dataset});
    if (dataset_section != nullptr) {
        apply_build_config_section(args, dataset_section, provided, "datasets." + args.dataset);
        if (const simple_json::Value* build_section = dataset_section->get("build_index")) {
            apply_build_config_section(args, build_section, provided, "datasets." + args.dataset + ".build_index");
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
        } else if (key == "--output") {
            provided.insert("output");
            args.output = need_value("--output");
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
            apply_build_config(args, config, provided);
            repository_paths::resolve_config_source(args.source, config_path, provided.count("source") != 0);
        } else if (explicit_config) {
            throw std::runtime_error("config file does not exist: " + config_path);
        }
    }

    if (args.dataset.empty() || args.source.empty()) {
        usage(argv[0]);
        throw std::runtime_error("missing required arguments");
    }
    if (args.clusters == 0) {
        args.clusters = default_clusters_for_dataset(args.dataset);
    }
    if (!args.source.empty() && args.source.back() != '/') {
        args.source.push_back('/');
    }
    return args;
}
} // namespace

int main(int argc, char** argv) {
    try {
        Args args = parse_args(argc, argv);
        const uint32_t actual_clusters = resolve_build_clusters(args);
        if (actual_clusters != args.clusters) {
            std::cerr << "Resolved actual cluster count " << actual_clusters
                      << " for requested " << args.clusters << "\n";
            args.clusters = actual_clusters;
        }
        if (args.output.empty()) {
            args.output = args.source + "ivfrabitq" + std::to_string(args.clusters) +
                "_B" + std::to_string(BB) + ".index";
        }
        const std::string data_path = args.files.path(args.source, args.dataset, "base");
        const std::string centroid_path =
            args.source + "RandCentroid_C" + std::to_string(args.clusters) +
            "_B" + std::to_string(BB) + ".fvecs";
        const std::string x0_path =
            args.source + "x0_C" + std::to_string(args.clusters) +
            "_B" + std::to_string(BB) + ".fvecs";
        const std::string dist_path =
            args.source + args.dataset + "_dist_to_centroid_" +
            std::to_string(args.clusters) + ".fvecs";
        const std::string cluster_path =
            args.source + args.dataset + "_cluster_id_" +
            std::to_string(args.clusters) + ".ivecs";
        const std::string binary_path =
            args.source + "RandNet_C" + std::to_string(args.clusters) +
            "_B" + std::to_string(BB) + ".Ivecs";

        IvfRabitQIndex<DIM, BB, B_QUERY> index;
        {
            Matrix<float> centroids(centroid_path);
            Matrix<float> x0(x0_path);
            Matrix<float> dist_to_centroid(dist_path);
            Matrix<uint32_t> cluster_id(cluster_path);
            Matrix<uint64_t> binary(binary_path);

            if (std::filesystem::path(data_path).extension() == ".bin" && header_bin_is_uint8(data_path)) {
                Matrix<uint8_t> x = read_bigann_u8bin_as_uint8(data_path);
                index.build(x, centroids, dist_to_centroid, x0, cluster_id, binary);
            } else {
                Matrix<float> x = read_float_vectors_file(data_path);
                index.build(x, centroids, dist_to_centroid, x0, cluster_id, binary);
            }
        }
        index.save(args.output);
        std::cerr << "Wrote " << args.output << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "build_index failed: " << e.what() << "\n";
        return 1;
    }
}
