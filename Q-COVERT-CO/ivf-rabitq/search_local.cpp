#include "repository_paths.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "matrix.h"
#include "private_ivf_rabitq.h"
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
    std::string index_path;
    std::string exact_payload = "auto";
    std::string config = repository_paths::default_config();
    std::string results_dir = "results";
    uint32_t clusters = 0;
    uint32_t k = 10;
    uint32_t nprobe = 100;
    std::vector<uint32_t> nprobes;
    std::vector<uint32_t> k2_values;
    uint32_t nqueries = 0;
    int threads = 1;
    bool no_config = false;
};

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
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

bool path_exists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

std::vector<uint32_t> parse_uint32_list(std::string text) {
    for (char& ch : text) {
        if (ch == ',' || ch == ';' || ch == ':') {
            ch = ' ';
        }
    }
    std::istringstream iss(text);
    std::vector<uint32_t> values;
    uint32_t value = 0;
    while (iss >> value) {
        values.push_back(value);
    }
    return values;
}

uint32_t default_clusters(const std::string& dataset) {
    const std::string key = lower_ascii(dataset);
    if (key == "sift1b" || key == "sift100m") {
        return 65536;
    }
    if (key == "siftsmall") {
        return 48;
    }
    return 4096;
}

uint32_t read_manifest_clusters(
        const std::string& source,
        const std::string& dataset,
        uint32_t requested_clusters) {
    const std::string path =
        source + dataset + "_assignment_" + std::to_string(requested_clusters) + ".txt";
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

std::string index_path_for_clusters(const std::string& source, uint32_t clusters) {
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
            path_exists(index_path_for_clusters(source, manifest_clusters))) {
        return manifest_clusters;
    }
    if (path_exists(index_path_for_clusters(source, requested_clusters))) {
        return requested_clusters;
    }

    std::vector<uint32_t> candidates;
    const std::string prefix = "ivfrabitq";
    const std::string suffix = "_B" + std::to_string(BB) + ".index";
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(source, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() <= prefix.size() + suffix.size() ||
                name.compare(0, prefix.size(), prefix) != 0 ||
                name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
            continue;
        }
        const std::string cluster_text =
            name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
        if (!cluster_text.empty() &&
                std::all_of(cluster_text.begin(), cluster_text.end(),
                            [](unsigned char ch) { return std::isdigit(ch); })) {
            candidates.push_back(static_cast<uint32_t>(std::stoul(cluster_text)));
        }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.size() == 1) {
        return candidates.front();
    }
    if (!candidates.empty()) {
        throw std::runtime_error(
            "multiple matching index cluster counts found; pass the actual --clusters value");
    }
    return requested_clusters;
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

std::string config_string(const simple_json::Value& value, const std::string& path) {
    return simple_json::as_string(value, path);
}

uint32_t config_uint32(const simple_json::Value& value, const std::string& path) {
    const double number = simple_json::as_number(value, path);
    if (number < 0) {
        throw std::runtime_error(path + " must be non-negative");
    }
    return static_cast<uint32_t>(number);
}

int config_int(const simple_json::Value& value, const std::string& path) {
    return static_cast<int>(simple_json::as_number(value, path));
}

std::vector<uint32_t> config_uint32_list(const simple_json::Value& value, const std::string& path) {
    if (value.is_number()) {
        return {config_uint32(value, path)};
    }
    if (!value.is_array() || value.array_value.empty()) {
        throw std::runtime_error(path + " must be a non-empty array");
    }
    std::vector<uint32_t> out;
    for (size_t i = 0; i < value.array_value.size(); ++i) {
        out.push_back(config_uint32(value.array_value[i], path + "[" + std::to_string(i) + "]"));
    }
    return out;
}

void apply_query_config_section(
        Args& args,
        const simple_json::Value* section,
        const std::set<std::string>& provided,
        const std::string& path) {
    if (section == nullptr || !section->is_object()) {
        return;
    }
    auto cli_provided = [&](const char* key) { return provided.count(key) != 0; };
    for (const auto& item : section->object_value) {
        const std::string& key = item.first;
        const simple_json::Value& value = item.second;
        const std::string value_path = path + "." + key;
        if (key == "files") {
            args.files.configure(value);
        } else if (key == "source" && !cli_provided("source")) {
            args.source = config_string(value, value_path);
        } else if (key == "clusters" && !cli_provided("clusters")) {
            args.clusters = config_uint32(value, value_path);
        } else if (key == "k" && !cli_provided("k")) {
            args.k = config_uint32(value, value_path);
        } else if (key == "nprobe" && !cli_provided("nprobe") && !cli_provided("nprobes")) {
            args.nprobe = config_uint32(value, value_path);
        } else if (key == "nprobes" && !cli_provided("nprobes") && !cli_provided("nprobe")) {
            args.nprobes = config_uint32_list(value, value_path);
            args.nprobe = args.nprobes.front();
        } else if (key == "k2" && !cli_provided("k2")) {
            args.k2_values = config_uint32_list(value, value_path);
        } else if (key == "nqueries" && !cli_provided("nqueries")) {
            args.nqueries = config_uint32(value, value_path);
        } else if (key == "exact_payload" && !cli_provided("exact_payload")) {
            args.exact_payload = lower_ascii(config_string(value, value_path));
        } else if (key == "threads" && !cli_provided("threads")) {
            args.threads = config_int(value, value_path);
        }
    }
}

void apply_config(
        Args& args,
        const simple_json::Value& root,
        const std::set<std::string>& provided) {
    if (args.dataset.empty()) {
        if (const simple_json::Value* default_dataset = root.get("default_dataset")) {
            args.dataset = config_string(*default_dataset, "default_dataset");
        }
    }
    apply_query_config_section(
        args, simple_json::get_path(root, {"defaults", "query"}), provided, "defaults.query");
    const simple_json::Value* dataset_section =
        simple_json::get_path(root, {"datasets", args.dataset});
    apply_query_config_section(args, dataset_section, provided, "datasets." + args.dataset);
    if (dataset_section != nullptr) {
        apply_query_config_section(
            args, dataset_section->get("query"), provided, "datasets." + args.dataset + ".query");
    }
}

void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " [--dataset sift] [--source ../data/sift] [--clusters dataset-default]"
              << " [--index path]"
              << " [--k 10] [--nprobe 100] [--nprobes 20,50,100] [--k2 60,150,250]"
              << " [--nqueries 0]"
              << " [--threads 1] [--exact-payload auto|raw-float32|raw-uint8]"
              << " [--results-dir results]"
              << " [--config config.json] [--no-config]\n";
}

Args parse_args(int argc, char** argv) {
    Args args;
    std::set<std::string> provided;
    auto need_value = [&](int& i, const char* name) -> std::string {
        if (i + 1 >= argc) {
            throw std::runtime_error(std::string("missing value for ") + name);
        }
        return argv[++i];
    };

    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--dataset") {
            provided.insert("dataset");
            args.dataset = need_value(i, "--dataset");
        } else if (key == "--source") {
            provided.insert("source");
            args.source = need_value(i, "--source");
        } else if (key == "--clusters") {
            provided.insert("clusters");
            args.clusters = static_cast<uint32_t>(std::stoul(need_value(i, "--clusters")));
        } else if (key == "--index") {
            provided.insert("index");
            args.index_path = need_value(i, "--index");
        } else if (key == "--k") {
            provided.insert("k");
            args.k = static_cast<uint32_t>(std::stoul(need_value(i, "--k")));
        } else if (key == "--nprobe") {
            provided.insert("nprobe");
            args.nprobe = static_cast<uint32_t>(std::stoul(need_value(i, "--nprobe")));
        } else if (key == "--nprobes" || key == "--nprobe-list") {
            provided.insert("nprobes");
            args.nprobes = parse_uint32_list(need_value(i, "--nprobes"));
        } else if (key == "--k2") {
            provided.insert("k2");
            args.k2_values = parse_uint32_list(need_value(i, "--k2"));
        } else if (key == "--nqueries") {
            provided.insert("nqueries");
            args.nqueries = static_cast<uint32_t>(std::stoul(need_value(i, "--nqueries")));
        } else if (key == "--threads") {
            provided.insert("threads");
            args.threads = std::stoi(need_value(i, "--threads"));
        } else if (key == "--exact-payload") {
            provided.insert("exact_payload");
            args.exact_payload = lower_ascii(need_value(i, "--exact-payload"));
        } else if (key == "--results-dir") {
            args.results_dir = need_value(i, "--results-dir");
        } else if (key == "--config") {
            args.config = need_value(i, "--config");
        } else if (key == "--no-config") {
            args.no_config = true;
        } else if (key == "--help" || key == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + key);
        }
    }

    if (!args.no_config && simple_json::file_exists(args.config)) {
        const simple_json::Value root = simple_json::parse_file(args.config);
        apply_config(args, root, provided);
            repository_paths::resolve_config_source(args.source, args.config, provided.count("source") != 0);
    }

    if (args.dataset.empty()) {
        usage(argv[0]);
        throw std::runtime_error("missing --dataset (and no default_dataset in config)");
    }
    if (args.source.empty()) {
        args.source = repository_paths::dataset(args.dataset);
    }
    if (args.source.back() != '/') {
        args.source.push_back('/');
    }
    if (args.clusters == 0) {
        args.clusters = default_clusters(args.dataset);
    }
    if (args.nprobes.empty()) {
        args.nprobes.push_back(args.nprobe);
    }
    if (args.k2_values.empty()) {
        throw std::invalid_argument(
            "--k2 is required: pass one k2 value per nprobe, e.g. --nprobes 20,50 --k2 60,150");
    }
    if (args.k2_values.size() != args.nprobes.size()) {
        throw std::invalid_argument(
            "--k2 count (" + std::to_string(args.k2_values.size()) +
            ") must match nprobe count (" + std::to_string(args.nprobes.size()) + ")");
    }
    if (args.k == 0) {
        throw std::invalid_argument("--k must be positive");
    }
    for (uint32_t nprobe : args.nprobes) {
        if (nprobe == 0) {
            throw std::invalid_argument("--nprobe values must be positive");
        }
    }
    for (uint32_t k2 : args.k2_values) {
        if (k2 == 0) {
            throw std::invalid_argument("--k2 values must be positive");
        }
    }
    std::replace(args.exact_payload.begin(), args.exact_payload.end(), '-', '_');
    if (args.exact_payload != "auto" &&
            args.exact_payload != "raw_float32" &&
            args.exact_payload != "raw_uint8") {
        throw std::invalid_argument("--exact-payload must be auto, raw-float32, or raw-uint8");
    }
    if (args.threads == 0) {
#ifdef _OPENMP
        args.threads = omp_get_max_threads();
#else
        args.threads = 1;
#endif
    }
    if (args.threads < 1) {
        throw std::invalid_argument("--threads must be positive (or 0 for all cores)");
    }
    return args;
}

float recall_at_k(ResultHeap result, const Matrix<uint32_t>& gt, size_t qi, uint32_t k) {
    std::unordered_set<uint32_t> expected;
    for (uint32_t i = 0; i < std::min<uint32_t>(k, static_cast<uint32_t>(gt.d)); ++i) {
        expected.insert(gt.row(qi)[i]);
    }
    uint32_t hit = 0;
    while (!result.empty()) {
        if (expected.find(result.top().second) != expected.end()) {
            hit++;
        }
        result.pop();
    }
    return static_cast<float>(hit) / static_cast<float>(k);
}

std::string join_uint32_values(const std::vector<uint32_t>& values) {
    std::ostringstream oss;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) {
            oss << ',';
        }
        oss << values[i];
    }
    return oss.str();
}

double now_sec() {
    return std::chrono::duration<double>(
               std::chrono::high_resolution_clock::now().time_since_epoch())
        .count();
}
} // namespace

int main(int argc, char** argv) {
    try {
        const Args args = parse_args(argc, argv);
        const uint32_t actual_clusters = resolve_index_clusters(
            args.source, args.dataset, args.clusters);
        const std::string index_path = args.index_path.empty()
            ? index_path_for_clusters(args.source, actual_clusters)
            : args.index_path;
        const ExactPayloadFormat exact_format = use_raw_uint8_exact_payload(args)
            ? ExactPayloadFormat::RawUint8
            : ExactPayloadFormat::RawFloat32;

        std::cout << std::fixed << std::setprecision(6);
        std::cout << "dataset " << args.dataset << "\n";
        std::cout << "source " << args.source << "\n";
        std::cout << "requested_clusters " << args.clusters << "\n";
        std::cout << "actual_clusters " << actual_clusters << "\n";
        std::cout << "index " << index_path << "\n";
        std::cout << "exact_payload "
                  << (exact_format == ExactPayloadFormat::RawUint8 ? "raw_uint8" : "raw_float32")
                  << "\n";
        std::cout << "threads " << args.threads << "\n";

        double t0 = now_sec();
        IvfRabitQIndex<DIM, BB, B_QUERY> index;
        index.load(index_path);
        const double index_load_sec = now_sec() - t0;
        std::cout << "index_load_sec " << index_load_sec << "\n";
        std::cout << "vectors " << index.n << "\n";

        t0 = now_sec();
        LocalPlainBlockReader<DIM, BB, B_QUERY> reader(index, exact_format);
        const double reader_build_sec = now_sec() - t0;
        std::cout << "reader_build_sec " << reader_build_sec << "\n";

        t0 = now_sec();
        LocalPlainAnnDoubleOramReader<DIM, BB, B_QUERY> ann_reader(index, reader);
        std::cout << "adapter_build_sec " << now_sec() - t0 << "\n";

        t0 = now_sec();
        Matrix<float> query = read_float_vectors_file(args.files.path(args.source, args.dataset, "query"));
        Matrix<uint32_t> gt = read_uint32_vectors_file(args.files.path(args.source, args.dataset, "groundtruth"));
        Matrix<float> randomized_query;
        {
            Matrix<float> projection(args.source + "P_C" + std::to_string(actual_clusters) +
                                     "_B" + std::to_string(BB) + ".fvecs");
            randomized_query = multiply_padded_left(query, BB, projection);
        }
        std::cout << "query_prepare_sec " << now_sec() - t0 << "\n";

        const uint32_t nq = args.nqueries == 0
            ? static_cast<uint32_t>(query.n)
            : std::min<uint32_t>(args.nqueries, static_cast<uint32_t>(query.n));
        if (nq == 0) {
            throw std::runtime_error("no queries to run");
        }
        std::cout << "queries " << nq << "\n";
        std::cout << "topk " << args.k << "\n";
        std::cout << "nprobe_values " << join_uint32_values(args.nprobes) << "\n";
        std::cout << "k2_values " << join_uint32_values(args.k2_values) << "\n";

        std::filesystem::create_directories(args.results_dir);
        const std::string csv_path =
            args.results_dir + "/" + sanitize_for_filename(args.dataset) +
            "_k" + std::to_string(args.k) + "_plain.csv";
        const bool csv_new = !path_exists(csv_path);
        std::ofstream csv(csv_path, std::ios::app);
        if (!csv) {
            throw std::runtime_error("cannot open result csv: " + csv_path);
        }
        if (csv_new) {
            csv << "timestamp,dataset,clusters,k,nprobe,k2,nqueries,threads,"
                   "recall_at_k,avg_query_time_ms,min_query_time_ms,max_query_time_ms,"
                   "wall_time_ms,avg_approx_distance_count,avg_exact_distance_count\n";
        }

        for (size_t nprobe_index = 0; nprobe_index < args.nprobes.size(); ++nprobe_index) {
            const uint32_t nprobe = args.nprobes[nprobe_index];
            const uint32_t k2 = args.k2_values[nprobe_index];
            std::vector<double> query_ms(nq, 0.0);
            std::vector<float> query_recall(nq, 0.0f);
            uint64_t approx_count = 0;
            uint64_t exact_count = 0;

            const double wall_t0 = now_sec();
#ifdef _OPENMP
#pragma omp parallel num_threads(args.threads)
#endif
            {
                PrivateIvfRabitQSearcher<DIM, BB, B_QUERY> searcher(index, exact_format);
                uint64_t approx_local = 0;
                uint64_t exact_local = 0;
#ifdef _OPENMP
#pragma omp for schedule(dynamic, 1)
#endif
                for (int64_t qi = 0; qi < static_cast<int64_t>(nq); ++qi) {
                    const double qt0 = now_sec();
                    ResultHeap result = searcher.search_double_oram(
                        query.row(qi), randomized_query.row(qi), args.k, nprobe,
                        ann_reader, nprobe, k2);
                    query_ms[qi] = (now_sec() - qt0) * 1000.0;
                    query_recall[qi] = recall_at_k(std::move(result), gt, qi, args.k);
                    approx_local += searcher.counters().approximate_distance_count;
                    exact_local += searcher.counters().exact_distance_count;
                }
#ifdef _OPENMP
#pragma omp atomic
#endif
                approx_count += approx_local;
#ifdef _OPENMP
#pragma omp atomic
#endif
                exact_count += exact_local;
            }
            const double wall_ms = (now_sec() - wall_t0) * 1000.0;

            double recall_sum = 0.0;
            double ms_sum = 0.0;
            double ms_min = query_ms.empty() ? 0.0 : query_ms[0];
            double ms_max = 0.0;
            for (uint32_t qi = 0; qi < nq; ++qi) {
                recall_sum += query_recall[qi];
                ms_sum += query_ms[qi];
                ms_min = std::min(ms_min, query_ms[qi]);
                ms_max = std::max(ms_max, query_ms[qi]);
            }

            const double recall = recall_sum / nq;
            const double avg_ms = ms_sum / nq;
            const double avg_approx = static_cast<double>(approx_count) / nq;
            const double avg_exact = static_cast<double>(exact_count) / nq;

            std::cout << "nprobe_run_begin " << nprobe << "\n";
            std::cout << "nprobe " << nprobe << "\n";
            std::cout << "k2 " << k2 << "\n";
            std::cout << "recall@" << args.k << " " << recall << "\n";
            std::cout << "avg_query_time_ms " << avg_ms << "\n";
            std::cout << "min_query_time_ms " << ms_min << "\n";
            std::cout << "max_query_time_ms " << ms_max << "\n";
            std::cout << "wall_time_ms " << wall_ms << "\n";
            std::cout << "avg_approx_distance_count " << avg_approx << "\n";
            std::cout << "avg_exact_distance_count " << avg_exact << "\n";
            std::cout << "nprobe_run_end " << nprobe << "\n";

            const auto now = std::chrono::system_clock::now();
            const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
            std::ostringstream timestamp;
            timestamp << std::put_time(std::localtime(&now_time), "%Y-%m-%d %H:%M:%S");
            csv << timestamp.str() << ','
                << args.dataset << ','
                << actual_clusters << ','
                << args.k << ','
                << nprobe << ','
                << k2 << ','
                << nq << ','
                << args.threads << ','
                << recall << ','
                << avg_ms << ','
                << ms_min << ','
                << ms_max << ','
                << wall_ms << ','
                << avg_approx << ','
                << avg_exact << '\n';
            csv.flush();
        }
        std::cout << "result_csv " << csv_path << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "search_local failed: " << e.what() << "\n";
        return 1;
    }
}
