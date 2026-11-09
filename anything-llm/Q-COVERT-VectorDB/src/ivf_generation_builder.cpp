#include "qcovert/ivf_generation_builder.h"

#include "qcovert/collection_data_store.h"

#include "ivf_rabitq_index.h"
#include "matrix.h"

#include <Eigen/Dense>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace qcovert {

namespace {

using ServiceIndex = IvfRabitQIndex<kServiceVectorDimension, kServiceRabitqBits, kServiceQueryBits>;

std::string json_escape(const std::string& value) {
    std::string output;
    output.reserve(value.size());
    for (unsigned char ch : value) {
        switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20) {
                    throw std::invalid_argument("record metadata contains a control character");
                }
                output.push_back(static_cast<char>(ch));
        }
    }
    return output;
}

uint32_t choose_cluster_count(size_t vector_count) {
    if (vector_count < 10000) {
        return 1;
    }
    const uint64_t root = static_cast<uint64_t>(std::sqrt(static_cast<long double>(vector_count)));
    const uint64_t rounded_up = root * root == vector_count ? root : root + 1;
    return static_cast<uint32_t>(std::min<uint64_t>(rounded_up, vector_count));
}

struct KMeansResult {
    Matrix<float> centroids;
    Matrix<uint32_t> assignments;
    Matrix<float> distances;
};

KMeansResult train_faiss_kmeans(
        const fs::path& snapshot_file,
        const fs::path& output_directory,
        const fs::path& python_executable,
        const fs::path& faiss_worker,
        size_t vector_count,
        uint32_t cluster_count,
        uint32_t iterations,
        uint32_t restarts,
        uint32_t seed,
        uint32_t threads) {
    if (python_executable.empty() || !fs::is_regular_file(python_executable)) {
        throw std::runtime_error("Faiss Python interpreter does not exist: " + python_executable.string());
    }
    if (faiss_worker.empty() || !fs::is_regular_file(faiss_worker)) {
        throw std::runtime_error("Faiss clustering worker does not exist: " + faiss_worker.string());
    }
    if (cluster_count == 0 || cluster_count > vector_count || iterations == 0 || restarts == 0) {
        throw std::invalid_argument("invalid Faiss k-means configuration");
    }

    const std::string clusters = std::to_string(cluster_count);
    const std::string niter = std::to_string(iterations);
    const std::string nredo = std::to_string(restarts);
    const std::string seed_text = std::to_string(seed);
    const std::string threads_text = std::to_string(threads);
    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("cannot fork Python Faiss worker");
    }
    if (child == 0) {
        execl(python_executable.c_str(),
              python_executable.c_str(),
              faiss_worker.c_str(),
              "--snapshot", snapshot_file.c_str(),
              "--output-dir", output_directory.c_str(),
              "--clusters", clusters.c_str(),
              "--niter", niter.c_str(),
              "--nredo", nredo.c_str(),
              "--seed", seed_text.c_str(),
              "--threads", threads_text.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited != child) {
        throw std::runtime_error("cannot wait for Python Faiss worker");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        throw std::runtime_error("Python Faiss worker failed with exit code " + std::to_string(exit_code));
    }

    KMeansResult result{
        Matrix<float>((output_directory / "faiss_centroids.fvecs").string()),
        Matrix<uint32_t>((output_directory / "faiss_assignments.ivecs").string()),
        Matrix<float>((output_directory / "faiss_distances.fvecs").string()),
    };
    if (result.centroids.n != cluster_count || result.centroids.d != kServiceVectorDimension ||
        result.assignments.n != vector_count || result.assignments.d != 1 ||
        result.distances.n != vector_count || result.distances.d != 1) {
        throw std::runtime_error("Python Faiss worker produced incompatible output dimensions");
    }
    for (const uint32_t assignment : result.assignments.values) {
        if (assignment >= cluster_count) {
            throw std::runtime_error("Python Faiss worker produced an out-of-range assignment");
        }
    }
    for (const float distance : result.distances.values) {
        if (!std::isfinite(distance) || distance < 0.0f) {
            throw std::runtime_error("Python Faiss worker produced an invalid centroid distance");
        }
    }
    return result;
}

Matrix<float> make_projection(uint32_t seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> normal(0.0f, 1.0f);
    Eigen::MatrixXf random(kServiceRabitqBits, kServiceRabitqBits);
    for (Eigen::Index row = 0; row < random.rows(); ++row) {
        for (Eigen::Index col = 0; col < random.cols(); ++col) {
            random(row, col) = normal(rng);
        }
    }
    Eigen::HouseholderQR<Eigen::MatrixXf> qr(random);
    const Eigen::MatrixXf projection =
        (qr.householderQ() * Eigen::MatrixXf::Identity(kServiceRabitqBits, kServiceRabitqBits)).transpose();
    Matrix<float> output(kServiceRabitqBits, kServiceRabitqBits);
    std::memcpy(output.values.data(), projection.data(), output.values.size() * sizeof(float));
    // Eigen MatrixXf is column-major; Matrix expects row-major.
    for (uint32_t row = 0; row < kServiceRabitqBits; ++row) {
        for (uint32_t col = 0; col < kServiceRabitqBits; ++col) {
            output.row(row)[col] = projection(row, col);
        }
    }
    return output;
}

void write_record_map(const std::vector<VectorRecord>& records, const fs::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create generation record map: " + path.string());
    }
    for (size_t i = 0; i < records.size(); ++i) {
        output << "{\"internal_id\":" << i
               << ",\"id\":\"" << json_escape(records[i].id)
               << "\",\"metadata\":" << records[i].metadata_json << "}\n";
    }
    if (!output) {
        throw std::runtime_error("cannot write generation record map: " + path.string());
    }
}

} // namespace

IvfGenerationArtifacts IvfGenerationBuilder384::build(
        const fs::path& snapshot_file,
        const fs::path& generation_directory,
        const fs::path& python_executable,
        const fs::path& faiss_worker,
        uint32_t kmeans_iterations,
        uint32_t kmeans_restarts,
        uint32_t seed,
        uint32_t faiss_threads) {
    const VectorSnapshotInfo snapshot = CollectionDataStore::inspect_snapshot(snapshot_file);
    if (snapshot.vector_count == 0) {
        throw std::invalid_argument("cannot build an IVF-RaBitQ generation without vectors");
    }
    if (snapshot.vector_count > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument("generation exceeds the uint32 internal ID capacity");
    }
    const uint32_t cluster_count = choose_cluster_count(snapshot.vector_count);
    KMeansResult kmeans = train_faiss_kmeans(
        snapshot_file,
        generation_directory,
        python_executable,
        faiss_worker,
        snapshot.vector_count,
        cluster_count,
        kmeans_iterations,
        kmeans_restarts,
        seed,
        faiss_threads);

    // The Python worker has exited before plaintext vectors are materialized in
    // C++, so its Faiss matrix never overlaps this process's build matrices.
    std::vector<VectorRecord> records = CollectionDataStore::read_snapshot(snapshot_file);
    Matrix<float> vectors(records.size(), kServiceVectorDimension);
    for (size_t i = 0; i < records.size(); ++i) {
        if (records[i].values.size() != kServiceVectorDimension) {
            throw std::runtime_error("snapshot contains a vector with an unexpected dimension");
        }
        std::memcpy(vectors.row(i), records[i].values.data(),
                    kServiceVectorDimension * sizeof(float));
        std::vector<float>().swap(records[i].values);
    }
    Matrix<float> projection = make_projection(seed);

    Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
        centroid_map(kmeans.centroids.values.data(), cluster_count, kServiceVectorDimension);
    Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
        projection_map(projection.values.data(), kServiceRabitqBits, kServiceRabitqBits);
    const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> randomized_centroids_eigen =
        centroid_map * projection_map;
    Matrix<float> randomized_centroids(cluster_count, kServiceRabitqBits);
    std::memcpy(randomized_centroids.values.data(), randomized_centroids_eigen.data(),
                randomized_centroids.values.size() * sizeof(float));

    Matrix<float> x0(records.size(), 1);
    Matrix<uint64_t> binary(records.size(), kServiceRabitqBits / 64);
    const float inverse_sqrt_bits = 1.0f / std::sqrt(static_cast<float>(kServiceRabitqBits));
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < records.size(); ++i) {
        const uint32_t cluster = kmeans.assignments.values[i];
        Eigen::Map<const Eigen::RowVectorXf> vector_row(vectors.row(i), kServiceVectorDimension);
        Eigen::Map<const Eigen::RowVectorXf> centroid_row(kmeans.centroids.row(cluster), kServiceVectorDimension);
        const Eigen::RowVectorXf residual = (vector_row - centroid_row) * projection_map;
        const float norm = residual.norm();
        float signed_sum = 0.0f;
        std::fill(binary.row(i), binary.row(i) + kServiceRabitqBits / 64, 0ULL);
        for (uint32_t d = 0; d < kServiceRabitqBits; ++d) {
            const bool positive = residual[static_cast<Eigen::Index>(d)] > 0.0f;
            if (positive) {
                binary.row(i)[d / 64] |= 1ULL << (63U - (d % 64U));
            }
            signed_sum += positive ? residual[static_cast<Eigen::Index>(d)]
                                   : -residual[static_cast<Eigen::Index>(d)];
        }
        x0.values[i] = norm > 1e-10f ? signed_sum * inverse_sqrt_bits / norm : 0.8f;
    }

    ServiceIndex index(vectors, randomized_centroids, kmeans.distances, x0, kmeans.assignments, binary);
    vectors.values.clear();
    vectors.values.shrink_to_fit();
    index.save((generation_directory / "ivf_rabitq.index").string());
    projection.write_vecs((generation_directory / "projection.fvecs").string());
    kmeans.centroids.write_vecs((generation_directory / "centroids.fvecs").string());
    write_record_map(records, generation_directory / "records.jsonl");

    for (const char* temporary_name : {
             "faiss_centroids.fvecs",
             "faiss_assignments.ivecs",
             "faiss_distances.fvecs",
         }) {
        std::error_code remove_error;
        fs::remove(generation_directory / temporary_name, remove_error);
        if (remove_error) {
            throw std::runtime_error(
                "cannot remove Faiss worker intermediate file: " + remove_error.message());
        }
    }

    uint64_t approximate_blocks = 0;
    for (uint32_t c = 0; c < index.num_clusters; ++c) {
        approximate_blocks += index.cluster_num_blocks(c);
    }
    return {snapshot.vector_count, cluster_count, approximate_blocks};
}

} // namespace qcovert
