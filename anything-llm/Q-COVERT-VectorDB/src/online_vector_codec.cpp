#include "online_vector_codec.h"

#include "ivf_rabitq_index.h"
#include "oram_block_builders.h"

#include <Eigen/Dense>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace qcovert {

namespace {

constexpr uint32_t kDimension = 384;
constexpr uint32_t kBits = 384;
using ServiceIndex = IvfRabitQIndex<kDimension, kBits, 4>;
using RawExactLayout = RawFloat32ExactOramBlockLayout<kDimension, kBits>;

} // namespace

uint32_t OnlineVectorCodec384::candidate_cluster_count(
        uint32_t top_k,
        uint32_t total_clusters) {
    if (top_k == 0 || total_clusters == 0) {
        throw std::invalid_argument("top_k and total_clusters must be positive");
    }
    const uint64_t requested =
        2ULL * static_cast<uint64_t>(top_k) + total_clusters / 25U;
    return static_cast<uint32_t>(std::min<uint64_t>(total_clusters, requested));
}

uint32_t OnlineVectorCodec384::exact_candidate_count(
        uint32_t top_k,
        uint32_t nprobe,
        double candidate_factor) {
    if (top_k == 0 || nprobe == 0 || !std::isfinite(candidate_factor) ||
        candidate_factor <= 0.0) {
        throw std::invalid_argument(
            "top_k, nprobe, and candidate_factor must be positive and finite");
    }
    const long double requested =
        2.0L * static_cast<long double>(top_k) +
        static_cast<long double>(top_k) * static_cast<long double>(nprobe) *
            static_cast<long double>(candidate_factor);
    const long double rounded = std::ceil(requested);
    if (rounded > static_cast<long double>(std::numeric_limits<uint32_t>::max())) {
        throw std::invalid_argument("exact candidate limit exceeds uint32");
    }
    return static_cast<uint32_t>(rounded);
}

OnlineQuantizedVector384 OnlineVectorCodec384::quantize(
        const VectorRecord& record,
        const Matrix<float>& centroids,
        const Matrix<float>& projection,
        uint32_t top_k) {
    if (record.values.size() != kDimension || centroids.d != kDimension || centroids.n == 0 ||
        centroids.n > std::numeric_limits<uint32_t>::max() ||
        projection.n != kBits || projection.d != kBits) {
        throw std::runtime_error("online RaBitQ inputs have incompatible dimensions");
    }
    std::vector<std::pair<float, int>> distances;
    distances.reserve(centroids.n);
    for (size_t cluster = 0; cluster < centroids.n; ++cluster) {
        float squared_distance = 0.0f;
        for (uint32_t d = 0; d < kDimension; ++d) {
            const float difference = record.values[d] - centroids.row(cluster)[d];
            squared_distance += difference * difference;
        }
        distances.emplace_back(squared_distance, static_cast<int>(cluster));
    }
    const size_t candidate_count = candidate_cluster_count(
        top_k, static_cast<uint32_t>(centroids.n));
    std::partial_sort(
        distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(candidate_count),
        distances.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.first != rhs.first ? lhs.first < rhs.first : lhs.second < rhs.second;
        });

    OnlineQuantizedVector384 output;
    output.target_cluster = distances.front().second;
    output.candidate_clusters.reserve(candidate_count);
    for (size_t i = 0; i < candidate_count; ++i) {
        output.candidate_clusters.push_back(distances[i].second);
    }

    Eigen::Map<const Eigen::RowVectorXf> vector(record.values.data(), kDimension);
    Eigen::Map<const Eigen::RowVectorXf> centroid(
        centroids.row(static_cast<size_t>(output.target_cluster)), kDimension);
    Eigen::Map<const Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>>
        projection_matrix(projection.values.data(), kBits, kBits);
    const Eigen::RowVectorXf residual = (vector - centroid) * projection_matrix;
    const float distance = std::sqrt(std::max(0.0f, distances.front().first));
    float signed_sum = 0.0f;
    std::array<uint64_t, kBits / 64> words{};
    for (uint32_t d = 0; d < kBits; ++d) {
        const bool positive = residual[static_cast<Eigen::Index>(d)] > 0.0f;
        if (positive) words[d / 64] |= 1ULL << (63U - d % 64U);
        signed_sum += positive ? residual[static_cast<Eigen::Index>(d)]
                               : -residual[static_cast<Eigen::Index>(d)];
    }
    const float inverse_sqrt_bits = 1.0f / std::sqrt(static_cast<float>(kBits));
    const float x0 = distance > 1e-10f ? signed_sum * inverse_sqrt_bits / distance : 0.8f;
    const long double x_x0 = static_cast<long double>(distance) / x0;
    ServiceIndex::Factor factor;
    factor.sqr_x = distance * distance;
    const long double error_argument =
        x_x0 * x_x0 - static_cast<long double>(distance) * distance;
    factor.error = 2.0f * ServiceIndex::kMaxX1 *
        std::sqrt(static_cast<double>(std::max<long double>(0.0, error_argument)));
    uint32_t population = 0;
    for (uint64_t word : words) {
        population += static_cast<uint32_t>(__builtin_popcountll(word));
    }
    factor.factor_ppc = -2.0f / ServiceIndex::kFacNorm * static_cast<float>(x_x0) *
        (static_cast<float>(population) * 2.0f - static_cast<float>(kBits));
    factor.factor_ip = -2.0f / ServiceIndex::kFacNorm * static_cast<float>(x_x0);

    output.approximate.factor.resize(sizeof(factor));
    std::memcpy(output.approximate.factor.data(), &factor, sizeof(factor));
    output.approximate.binary_code.resize(sizeof(words));
    std::memcpy(output.approximate.binary_code.data(), words.data(), sizeof(words));
    return output;
}

std::vector<uint8_t> OnlineVectorCodec384::raw_float32_exact_payload(
        const VectorRecord& record,
        int internal_id) {
    if (record.values.size() != kDimension || internal_id < 0) {
        throw std::invalid_argument("cannot encode an invalid online exact vector");
    }
    std::vector<uint8_t> payload(RawExactLayout::kPlainBytes, 0);
    std::memcpy(payload.data(), record.values.data(), record.values.size() * sizeof(float));
    const uint32_t id = static_cast<uint32_t>(internal_id);
    std::memcpy(payload.data() + kDimension * sizeof(float), &id, sizeof(id));
    return payload;
}

} // namespace qcovert
