#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

#include "matrix.h"
#include "private_ivf_rabitq.h"

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
    std::string dataset;
    std::string source;
    std::string index_path;
    uint32_t clusters = 0;
    uint32_t k = 100;
    uint32_t nprobe = 200;
    uint32_t nqueries = 0;
};

void usage(const char* prog) {
    std::cerr << "Usage: " << prog
              << " --dataset sift --source /path/to/data/sift --clusters 4096"
              << " [--index path] [--k 100] [--nprobe 200] [--nqueries 100]\n";
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string("missing value for ") + name);
            }
            return argv[++i];
        };

        if (key == "--dataset") {
            args.dataset = need_value("--dataset");
        } else if (key == "--source") {
            args.source = need_value("--source");
        } else if (key == "--clusters") {
            args.clusters = static_cast<uint32_t>(std::stoul(need_value("--clusters")));
        } else if (key == "--index") {
            args.index_path = need_value("--index");
        } else if (key == "--k") {
            args.k = static_cast<uint32_t>(std::stoul(need_value("--k")));
        } else if (key == "--nprobe") {
            args.nprobe = static_cast<uint32_t>(std::stoul(need_value("--nprobe")));
        } else if (key == "--nqueries") {
            args.nqueries = static_cast<uint32_t>(std::stoul(need_value("--nqueries")));
        } else if (key == "--help" || key == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + key);
        }
    }

    if (args.dataset.empty() || args.source.empty() || args.clusters == 0) {
        usage(argv[0]);
        throw std::runtime_error("missing required arguments");
    }
    if (!args.source.empty() && args.source.back() != '/') {
        args.source.push_back('/');
    }
    if (args.index_path.empty()) {
        args.index_path = args.source + "ivfrabitq" + std::to_string(args.clusters) + "_B" + std::to_string(BB) + ".index";
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
} // namespace

int main(int argc, char** argv) {
    try {
        const Args args = parse_args(argc, argv);

        IvfRabitQIndex<DIM, BB, B_QUERY> index;
        index.load(args.index_path);

        Matrix<float> query(args.source + args.dataset + "_query.fvecs");
        Matrix<uint32_t> gt(args.source + args.dataset + "_groundtruth.ivecs");
        Matrix<float> projection(args.source + "P_C" + std::to_string(args.clusters) + "_B" + std::to_string(BB) + ".fvecs");
        Matrix<float> randomized_query = multiply_padded_left(query, BB, projection);

        LocalPlainBlockReader<DIM, BB, B_QUERY> local_reader(index);
        PrivateIvfRabitQSearcher<DIM, BB, B_QUERY> searcher(index);

        const uint32_t nq = args.nqueries == 0 ? static_cast<uint32_t>(query.n)
                                               : std::min<uint32_t>(args.nqueries, static_cast<uint32_t>(query.n));
        double recall_sum = 0.0;
        uint64_t approx_count = 0;
        uint64_t exact_count = 0;

        for (uint32_t qi = 0; qi < nq; ++qi) {
            ResultHeap result = searcher.search_local_plain(query.row(qi),
                                                            randomized_query.row(qi),
                                                            args.k,
                                                            args.nprobe,
                                                            local_reader);
            recall_sum += recall_at_k(result, gt, qi, args.k);
            approx_count += searcher.counters().approximate_distance_count;
            exact_count += searcher.counters().exact_distance_count;
        }

        std::cout << "queries " << nq << "\n";
        std::cout << "recall@" << args.k << " " << recall_sum / nq << "\n";
        std::cout << "avg_approx_distance_count " << static_cast<double>(approx_count) / nq << "\n";
        std::cout << "avg_exact_distance_count " << static_cast<double>(exact_count) / nq << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "search_local failed: " << e.what() << "\n";
        return 1;
    }
}

