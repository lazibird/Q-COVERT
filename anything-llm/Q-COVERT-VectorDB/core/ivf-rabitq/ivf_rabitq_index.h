#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "fast_scan.h"
#include "matrix.h"
#include "space.h"
#include "types.h"

template <uint32_t D, uint32_t B, uint32_t QueryBits = 4>
class IvfRabitQIndex {
public:
    using SpaceT = Space<D, B, QueryBits>;
    enum class RawVectorStorage : uint32_t {
        Float32 = 0,
        Uint8 = 1,
    };

    struct Factor {
        float sqr_x = 0.0f;
        float error = 0.0f;
        float factor_ppc = 0.0f;
        float factor_ip = 0.0f;
    };

    static constexpr uint32_t kBlockVectors = 32;
    static constexpr float kFacNorm = static_cast<float>(const_sqrt(1.0 * B));
    static constexpr float kMaxX1 = static_cast<float>(1.9 / const_sqrt(1.0 * B - 1.0));

    uint32_t n = 0;
    uint32_t num_clusters = 0;
    uint32_t approx_blocks_per_cluster = 0;
    std::vector<uint32_t> approx_blocks;
    std::vector<uint32_t> start;
    std::vector<uint32_t> len;
    std::vector<uint32_t> ids;
    std::vector<float> dist_to_centroid;
    std::vector<float> x0;
    std::vector<float> randomized_centroids;
    RawVectorStorage raw_vector_storage = RawVectorStorage::Float32;
    std::vector<float> raw_vectors_by_cluster;
    std::vector<uint8_t> raw_vectors_u8_by_cluster;
    std::vector<uint64_t> binary_code;
    std::vector<uint32_t> packed_start;
    std::vector<uint8_t, AlignedAllocator<uint8_t, 32>> packed_code;
    std::vector<Factor> factors;
    std::vector<float> query_quantization_offsets;

    IvfRabitQIndex() = default;

    IvfRabitQIndex(const Matrix<float>& x,
                   const Matrix<float>& randomized_centroid,
                   const Matrix<float>& dist_to_c,
                   const Matrix<float>& x0_input,
                   const Matrix<uint32_t>& cluster_id,
                   const Matrix<uint64_t>& binary) {
        build(x, randomized_centroid, dist_to_c, x0_input, cluster_id, binary);
    }

    IvfRabitQIndex(const Matrix<uint8_t>& x,
                   const Matrix<float>& randomized_centroid,
                   const Matrix<float>& dist_to_c,
                   const Matrix<float>& x0_input,
                   const Matrix<uint32_t>& cluster_id,
                   const Matrix<uint64_t>& binary) {
        build(x, randomized_centroid, dist_to_c, x0_input, cluster_id, binary);
    }

    void build(const Matrix<float>& x,
               const Matrix<float>& randomized_centroid,
               const Matrix<float>& dist_to_c,
               const Matrix<float>& x0_input,
               const Matrix<uint32_t>& cluster_id,
               const Matrix<uint64_t>& binary) {
        build_impl(x, randomized_centroid, dist_to_c, x0_input, cluster_id, binary);
    }

    void build(const Matrix<uint8_t>& x,
               const Matrix<float>& randomized_centroid,
               const Matrix<float>& dist_to_c,
               const Matrix<float>& x0_input,
               const Matrix<uint32_t>& cluster_id,
               const Matrix<uint64_t>& binary) {
        build_impl(x, randomized_centroid, dist_to_c, x0_input, cluster_id, binary);
    }

private:
    template <typename RawT>
    void build_impl(const Matrix<RawT>& x,
                    const Matrix<float>& randomized_centroid,
                    const Matrix<float>& dist_to_c,
                    const Matrix<float>& x0_input,
                    const Matrix<uint32_t>& cluster_id,
                    const Matrix<uint64_t>& binary) {
        if (x.d != D || randomized_centroid.d != B || binary.d != B / 64) {
            throw std::runtime_error("IvfRabitQIndex::build: dimension mismatch");
        }
        if (dist_to_c.n != x.n || x0_input.n != x.n || cluster_id.n != x.n || binary.n != x.n) {
            throw std::runtime_error("IvfRabitQIndex::build: row count mismatch");
        }

        n = static_cast<uint32_t>(x.n);
        num_clusters = static_cast<uint32_t>(randomized_centroid.n);
        approx_blocks_per_cluster = 0;
        approx_blocks.assign(num_clusters, 0);
        start.assign(num_clusters, 0);
        len.assign(num_clusters, 0);
        ids.assign(n, 0);
        dist_to_centroid.assign(n, 0.0f);
        x0.assign(n, 0.0f);
        randomized_centroids.assign(randomized_centroid.values.begin(), randomized_centroid.values.end());
        if constexpr (std::is_same<RawT, uint8_t>::value) {
            raw_vector_storage = RawVectorStorage::Uint8;
            raw_vectors_by_cluster.clear();
            raw_vectors_by_cluster.shrink_to_fit();
            raw_vectors_u8_by_cluster.assign(static_cast<size_t>(n) * D, 0);
        } else {
            raw_vector_storage = RawVectorStorage::Float32;
            raw_vectors_u8_by_cluster.clear();
            raw_vectors_u8_by_cluster.shrink_to_fit();
            raw_vectors_by_cluster.assign(static_cast<size_t>(n) * D, 0.0f);
        }
        binary_code.assign(static_cast<size_t>(n) * B / 64, 0);

        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t c = cluster_id.values[i * cluster_id.d];
            if (c >= num_clusters) {
                throw std::runtime_error("IvfRabitQIndex::build: cluster id out of range");
            }
            len[c]++;
        }

        uint32_t sum = 0;
        for (uint32_t c = 0; c < num_clusters; ++c) {
            start[c] = sum;
            sum += len[c];
        }

        std::vector<uint32_t> cursor = start;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t c = cluster_id.values[static_cast<size_t>(i) * cluster_id.d];
            const uint32_t dst = cursor[c]++;
            ids[dst] = i;
            dist_to_centroid[dst] = dist_to_c.values[static_cast<size_t>(i) * dist_to_c.d];
            x0[dst] = x0_input.values[static_cast<size_t>(i) * x0_input.d];
            if constexpr (std::is_same<RawT, uint8_t>::value) {
                std::memcpy(raw_vectors_u8_by_cluster.data() + static_cast<size_t>(dst) * D,
                            x.row(i),
                            D * sizeof(uint8_t));
            } else {
                std::memcpy(raw_vectors_by_cluster.data() + static_cast<size_t>(dst) * D,
                            x.row(i),
                            D * sizeof(float));
            }
            std::memcpy(binary_code.data() + static_cast<size_t>(dst) * (B / 64),
                        binary.row(i),
                        (B / 64) * sizeof(uint64_t));
        }

        initialize_query_quantization_offsets();
        compute_factors();
        compute_approx_block_counts();
        pack_binary_codes();
    }

public:
    void save(const std::string& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write index: " + path);
        }
        const uint32_t d = D;
        const uint32_t b = B;
        const uint32_t magic = 0x44524951; // "DRIQ"
        const uint32_t version = 3;
        const uint32_t raw_storage = static_cast<uint32_t>(raw_vector_storage);
        out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
        out.write(reinterpret_cast<const char*>(&version), sizeof(version));
        out.write(reinterpret_cast<const char*>(&n), sizeof(n));
        out.write(reinterpret_cast<const char*>(&d), sizeof(d));
        out.write(reinterpret_cast<const char*>(&num_clusters), sizeof(num_clusters));
        out.write(reinterpret_cast<const char*>(&b), sizeof(b));
        out.write(reinterpret_cast<const char*>(&approx_blocks_per_cluster), sizeof(approx_blocks_per_cluster));
        out.write(reinterpret_cast<const char*>(approx_blocks.data()), approx_blocks.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(start.data()), start.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(len.data()), len.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(ids.data()), ids.size() * sizeof(uint32_t));
        out.write(reinterpret_cast<const char*>(dist_to_centroid.data()), dist_to_centroid.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(x0.data()), x0.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(randomized_centroids.data()),
                  randomized_centroids.size() * sizeof(float));
        out.write(reinterpret_cast<const char*>(&raw_storage), sizeof(raw_storage));
        if (raw_vector_storage == RawVectorStorage::Uint8) {
            out.write(reinterpret_cast<const char*>(raw_vectors_u8_by_cluster.data()),
                      raw_vectors_u8_by_cluster.size() * sizeof(uint8_t));
        } else {
            out.write(reinterpret_cast<const char*>(raw_vectors_by_cluster.data()),
                      raw_vectors_by_cluster.size() * sizeof(float));
        }
        out.write(reinterpret_cast<const char*>(binary_code.data()), binary_code.size() * sizeof(uint64_t));
        if (!out) {
            throw std::runtime_error("failed writing index: " + path);
        }
    }

    void load(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open index: " + path);
        }
        uint32_t first = 0;
        uint32_t d = 0;
        uint32_t b = 0;
        in.read(reinterpret_cast<char*>(&first), sizeof(first));
        if (!in) {
            throw std::runtime_error("IvfRabitQIndex::load: missing header");
        }

        const uint32_t magic = 0x44524951; // "DRIQ"
        bool versioned = false;
        uint32_t version = 1;
        if (first == magic) {
            in.read(reinterpret_cast<char*>(&version), sizeof(version));
            if (!in || (version != 2 && version != 3)) {
                throw std::runtime_error("IvfRabitQIndex::load: unsupported index version");
            }
            versioned = true;
            in.read(reinterpret_cast<char*>(&n), sizeof(n));
        } else {
            n = first;
        }
        in.read(reinterpret_cast<char*>(&d), sizeof(d));
        in.read(reinterpret_cast<char*>(&num_clusters), sizeof(num_clusters));
        in.read(reinterpret_cast<char*>(&b), sizeof(b));
        if (!in || d != D || b != B) {
            throw std::runtime_error("IvfRabitQIndex::load: dimension mismatch");
        }

        in.read(reinterpret_cast<char*>(&approx_blocks_per_cluster), sizeof(approx_blocks_per_cluster));
        if (!in || approx_blocks_per_cluster == 0) {
            throw std::runtime_error("IvfRabitQIndex::load: invalid approximate block count");
        }
        approx_blocks.assign(num_clusters, 0);
        if (versioned) {
            in.read(reinterpret_cast<char*>(approx_blocks.data()), approx_blocks.size() * sizeof(uint32_t));
            if (!in) {
                throw std::runtime_error("IvfRabitQIndex::load: invalid per-cluster approximate block counts");
            }
        }

        start.resize(num_clusters);
        len.resize(num_clusters);
        ids.resize(n);
        dist_to_centroid.resize(n);
        x0.resize(n);
        randomized_centroids.resize(static_cast<size_t>(num_clusters) * B);
        binary_code.resize(static_cast<size_t>(n) * B / 64);

        in.read(reinterpret_cast<char*>(start.data()), start.size() * sizeof(uint32_t));
        in.read(reinterpret_cast<char*>(len.data()), len.size() * sizeof(uint32_t));
        in.read(reinterpret_cast<char*>(ids.data()), ids.size() * sizeof(uint32_t));
        in.read(reinterpret_cast<char*>(dist_to_centroid.data()), dist_to_centroid.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(x0.data()), x0.size() * sizeof(float));
        in.read(reinterpret_cast<char*>(randomized_centroids.data()), randomized_centroids.size() * sizeof(float));
        if (versioned && version >= 3) {
            uint32_t raw_storage = 0;
            in.read(reinterpret_cast<char*>(&raw_storage), sizeof(raw_storage));
            if (!in || (raw_storage != static_cast<uint32_t>(RawVectorStorage::Float32) &&
                        raw_storage != static_cast<uint32_t>(RawVectorStorage::Uint8))) {
                throw std::runtime_error("IvfRabitQIndex::load: invalid raw vector storage");
            }
            raw_vector_storage = static_cast<RawVectorStorage>(raw_storage);
        } else {
            raw_vector_storage = RawVectorStorage::Float32;
        }

        if (raw_vector_storage == RawVectorStorage::Uint8) {
            raw_vectors_by_cluster.clear();
            raw_vectors_by_cluster.shrink_to_fit();
            raw_vectors_u8_by_cluster.resize(static_cast<size_t>(n) * D);
            in.read(reinterpret_cast<char*>(raw_vectors_u8_by_cluster.data()),
                    raw_vectors_u8_by_cluster.size() * sizeof(uint8_t));
        } else {
            raw_vectors_u8_by_cluster.clear();
            raw_vectors_u8_by_cluster.shrink_to_fit();
            raw_vectors_by_cluster.resize(static_cast<size_t>(n) * D);
            in.read(reinterpret_cast<char*>(raw_vectors_by_cluster.data()),
                    raw_vectors_by_cluster.size() * sizeof(float));
        }
        in.read(reinterpret_cast<char*>(binary_code.data()), binary_code.size() * sizeof(uint64_t));
        if (!in) {
            throw std::runtime_error("short read while loading index: " + path);
        }

        initialize_query_quantization_offsets();
        compute_factors();
        if (!versioned) {
            compute_approx_block_counts();
        } else {
            validate_approx_block_counts();
        }
        pack_binary_codes();
    }

    size_t packed_code_bytes() const {
        size_t blocks = 0;
        for (uint32_t c = 0; c < num_clusters; ++c) {
            blocks += cluster_num_blocks(c);
        }
        return blocks * kBlockVectors * B / 8;
    }

    size_t cluster_num_blocks(uint32_t cluster_id) const {
        if (cluster_id >= num_clusters) {
            throw std::out_of_range("cluster_num_blocks: cluster_id out of range");
        }
        if (approx_blocks.empty()) {
            return approx_blocks_per_cluster;
        }
        return approx_blocks[cluster_id];
    }

    size_t cluster_active_blocks(uint32_t cluster_id) const {
        return (len[cluster_id] + kBlockVectors - 1) / kBlockVectors;
    }

    const uint8_t* packed_block(uint32_t cluster_id, uint32_t block_id) const {
        if (block_id >= cluster_num_blocks(cluster_id)) {
            throw std::out_of_range("packed_block: block_id out of range");
        }
        const size_t block_bytes = static_cast<size_t>(kBlockVectors) * B / 8;
        return packed_code.data() + packed_start[cluster_id] + static_cast<size_t>(block_id) * block_bytes;
    }

    const Factor* factor_block(uint32_t cluster_id, uint32_t block_id) const {
        if (block_id >= cluster_num_blocks(cluster_id)) {
            throw std::out_of_range("factor_block: block_id out of range");
        }
        return factors.data() + start[cluster_id] + static_cast<size_t>(block_id) * kBlockVectors;
    }

    bool has_raw_vectors() const {
        return raw_vector_storage == RawVectorStorage::Uint8
            ? !raw_vectors_u8_by_cluster.empty()
            : !raw_vectors_by_cluster.empty();
    }

    bool stores_raw_vectors_as_uint8() const {
        return raw_vector_storage == RawVectorStorage::Uint8;
    }

    const float* raw_vector_by_cluster_offset(uint32_t cluster_offset) const {
        if (raw_vector_storage != RawVectorStorage::Float32) {
            throw std::runtime_error("raw_vector_by_cluster_offset requires float raw vector storage");
        }
        return raw_vectors_by_cluster.data() + static_cast<size_t>(cluster_offset) * D;
    }

    const uint8_t* raw_vector_u8_by_cluster_offset(uint32_t cluster_offset) const {
        if (raw_vector_storage != RawVectorStorage::Uint8) {
            throw std::runtime_error("raw_vector_u8_by_cluster_offset requires uint8 raw vector storage");
        }
        return raw_vectors_u8_by_cluster.data() + static_cast<size_t>(cluster_offset) * D;
    }

    float raw_vector_value(uint32_t cluster_offset, uint32_t dim) const {
        if (dim >= D) {
            throw std::out_of_range("raw_vector_value: dimension out of range");
        }
        if (raw_vector_storage == RawVectorStorage::Uint8) {
            return static_cast<float>(raw_vectors_u8_by_cluster[static_cast<size_t>(cluster_offset) * D + dim]);
        }
        return raw_vectors_by_cluster[static_cast<size_t>(cluster_offset) * D + dim];
    }

    std::vector<float> raw_vector_as_float(uint32_t cluster_offset) const {
        std::vector<float> out(D, 0.0f);
        if (raw_vector_storage == RawVectorStorage::Uint8) {
            const uint8_t* src = raw_vector_u8_by_cluster_offset(cluster_offset);
            for (uint32_t d = 0; d < D; ++d) {
                out[d] = static_cast<float>(src[d]);
            }
        } else {
            const float* src = raw_vector_by_cluster_offset(cluster_offset);
            std::memcpy(out.data(), src, D * sizeof(float));
        }
        return out;
    }

    void release_raw_vectors() {
        raw_vectors_by_cluster.clear();
        raw_vectors_by_cluster.shrink_to_fit();
        raw_vectors_u8_by_cluster.clear();
        raw_vectors_u8_by_cluster.shrink_to_fit();
    }

    void release_double_oram_search_unused_data() {
        // Safe after DoubleORAM has been initialized and the searcher has copied
        // any cluster means it needs. The DoubleORAM query loop reads approximate
        // and exact payloads from ORAM, so these index-side build arrays are not
        // needed during queries.
        release_raw_vectors();
        release_vector_storage(ids);
        release_vector_storage(start);
        release_vector_storage(dist_to_centroid);
        release_vector_storage(x0);
        release_vector_storage(binary_code);
        release_vector_storage(packed_start);
        release_vector_storage(packed_code);
        release_vector_storage(factors);
    }

private:
    template <typename VectorT>
    static void release_vector_storage(VectorT& values) {
        values.clear();
        values.shrink_to_fit();
    }

    void initialize_query_quantization_offsets() {
        query_quantization_offsets.resize(B);
        std::mt19937 gen(1234567);
        std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
        for (uint32_t i = 0; i < B; ++i) {
            query_quantization_offsets[i] = uniform(gen);
        }
    }

    void compute_factors() {
        factors.assign(n, Factor{});
        for (uint32_t i = 0; i < n; ++i) {
            const long double x_x0 = static_cast<long double>(dist_to_centroid[i]) / x0[i];
            Factor& f = factors[i];
            f.sqr_x = dist_to_centroid[i] * dist_to_centroid[i];
            const long double err_arg = x_x0 * x_x0 - static_cast<long double>(dist_to_centroid[i]) * dist_to_centroid[i];
            f.error = 2.0f * kMaxX1 * std::sqrt(static_cast<double>(std::max<long double>(0.0, err_arg)));
            const uint32_t pop = SpaceT::popcount(binary_code.data() + static_cast<size_t>(i) * B / 64);
            f.factor_ppc = -2.0f / kFacNorm * static_cast<float>(x_x0) *
                           (static_cast<float>(pop) * 2.0f - static_cast<float>(B));
            f.factor_ip = -2.0f / kFacNorm * static_cast<float>(x_x0);
        }
    }

    void compute_approx_block_counts() {
        approx_blocks_per_cluster = 0;
        approx_blocks.assign(num_clusters, 0);
        for (uint32_t c = 0; c < num_clusters; ++c) {
            uint32_t blocks = static_cast<uint32_t>((len[c] + kBlockVectors - 1) / kBlockVectors);
            if (blocks == 0) {
                blocks = 1;
            }
            approx_blocks[c] = blocks;
            approx_blocks_per_cluster = std::max<uint32_t>(approx_blocks_per_cluster, blocks);
        }
        if (num_clusters > 0 && approx_blocks_per_cluster == 0) {
            approx_blocks_per_cluster = 1;
        }
    }

    void validate_approx_block_counts() {
        if (approx_blocks.size() != num_clusters) {
            throw std::runtime_error("IvfRabitQIndex: invalid per-cluster approximate block count vector");
        }
        uint32_t max_blocks = 0;
        for (uint32_t c = 0; c < num_clusters; ++c) {
            uint32_t expected = static_cast<uint32_t>((len[c] + kBlockVectors - 1) / kBlockVectors);
            if (expected == 0) {
                expected = 1;
            }
            if (approx_blocks[c] != expected) {
                throw std::runtime_error("IvfRabitQIndex: approximate block count does not match cluster size");
            }
            max_blocks = std::max(max_blocks, approx_blocks[c]);
        }
        approx_blocks_per_cluster = max_blocks;
    }

    void pack_binary_codes() {
        if (approx_blocks.empty() || approx_blocks_per_cluster == 0) {
            compute_approx_block_counts();
        }
        packed_start.resize(num_clusters);
        size_t total = 0;
        for (uint32_t c = 0; c < num_clusters; ++c) {
            packed_start[c] = static_cast<uint32_t>(total);
            total += static_cast<size_t>(cluster_num_blocks(c)) * kBlockVectors * B / 8;
        }
        packed_code.assign(total, 0);
        for (uint32_t c = 0; c < num_clusters; ++c) {
            pack_codes_binary<B>(binary_code.data() + static_cast<size_t>(start[c]) * B / 64,
                                 len[c],
                                 packed_code.data() + packed_start[c]);
        }
    }
};
