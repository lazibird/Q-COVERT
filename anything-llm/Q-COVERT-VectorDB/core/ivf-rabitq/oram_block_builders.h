#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "ivf_rabitq_index.h"
#include "oram_interface.h"
#include "rabitq_plus.h"

template <uint32_t D, uint32_t B>
struct ApproxOramBlockLayout {
    using Index = IvfRabitQIndex<D, B>;
    using Factor = typename Index::Factor;

    static constexpr uint32_t kVectorsPerBlock = 32;
    static constexpr size_t kFactorBytes = kVectorsPerBlock * sizeof(Factor);
    static constexpr size_t kPackedCodeBytes = kVectorsPerBlock * B / 8;
    static constexpr size_t kPlainBytes = kFactorBytes + kPackedCodeBytes;
};

template <uint32_t D, uint32_t B>
struct ExactOramBlockLayout {
    static constexpr uint32_t kVectorsPerBlock = 1;
    static constexpr size_t kVectorBytes = RaBitQPlus::serialized_vector_bytes_static(D, 8);
    static constexpr size_t kPlainBytes = kVectorBytes + sizeof(uint32_t);
};

template <uint32_t D, uint32_t B>
struct RawUint8ExactOramBlockLayout {
    static constexpr uint32_t kVectorsPerBlock = 1;
    static constexpr size_t kVectorBytes = D;
    static constexpr size_t kPlainBytes = kVectorBytes + sizeof(uint32_t);
};

template <uint32_t D, uint32_t B>
struct RawFloat32ExactOramBlockLayout {
    static constexpr uint32_t kVectorsPerBlock = 1;
    static constexpr size_t kVectorBytes = static_cast<size_t>(D) * sizeof(float);
    static constexpr size_t kPlainBytes = kVectorBytes + sizeof(uint32_t);
};

template <uint32_t D, uint32_t B>
std::vector<uint8_t> build_approx_oram_payload(
        const IvfRabitQIndex<D, B>& index,
        uint32_t cluster_id,
        uint32_t block_id) {
    using Layout = ApproxOramBlockLayout<D, B>;
    using Factor = typename IvfRabitQIndex<D, B>::Factor;

    if (cluster_id >= index.num_clusters || block_id >= index.cluster_num_blocks(cluster_id)) {
        throw std::out_of_range("build_approx_oram_payload: block address is out of range");
    }

    std::vector<uint8_t> plain(Layout::kPlainBytes, 0);
    const uint32_t block_begin = block_id * IvfRabitQIndex<D, B>::kBlockVectors;
    const uint32_t base = index.start[cluster_id] + block_begin;
    const uint32_t active =
        block_begin >= index.len[cluster_id]
            ? 0
            : std::min<uint32_t>(IvfRabitQIndex<D, B>::kBlockVectors, index.len[cluster_id] - block_begin);
    for (uint32_t j = 0; j < active; ++j) {
        std::memcpy(plain.data() + static_cast<size_t>(j) * sizeof(Factor),
                    &index.factors[base + j],
                    sizeof(Factor));
    }
    std::memcpy(plain.data() + Layout::kFactorBytes,
                index.packed_block(cluster_id, block_id),
                Layout::kPackedCodeBytes);
    return plain;
}

template <uint32_t D, uint32_t B>
std::vector<uint8_t> build_exact_oram_payload(
        RaBitQPlus& rabitq_plus,
        const IvfRabitQIndex<D, B>& index,
        const std::vector<float>& cluster_means,
        uint32_t cluster_id,
        uint32_t local_vector_id) {
    using Layout = ExactOramBlockLayout<D, B>;

    if (cluster_id >= index.num_clusters || local_vector_id >= index.len[cluster_id]) {
        throw std::out_of_range("build_exact_oram_payload: vector address is out of range");
    }
    if (cluster_means.size() != static_cast<size_t>(index.num_clusters) * D) {
        throw std::invalid_argument("build_exact_oram_payload: cluster mean vector has wrong size");
    }

    const uint32_t global_offset = index.start[cluster_id] + local_vector_id;
    const float* mean = cluster_means.data() + static_cast<size_t>(cluster_id) * D;
    std::vector<float> vec_buffer;
    const float* vec = nullptr;
    if (index.stores_raw_vectors_as_uint8()) {
        vec_buffer = index.raw_vector_as_float(global_offset);
        vec = vec_buffer.data();
    } else {
        vec = index.raw_vector_by_cluster_offset(global_offset);
    }
    std::vector<RaBitQPlus::QuantizeResult> qrs = rabitq_plus.quantize(mean, vec, 1);
    std::vector<uint8_t> serialized = qrs[0].serialize();
    if (serialized.size() > Layout::kVectorBytes) {
        throw std::runtime_error("RaBitQPlus serialized vector exceeds exact layout");
    }

    std::vector<uint8_t> plain(Layout::kPlainBytes, 0);
    std::memcpy(plain.data(), serialized.data(), serialized.size());
    const uint32_t vector_id = index.ids[global_offset];
    std::memcpy(plain.data() + Layout::kVectorBytes, &vector_id, sizeof(vector_id));
    return plain;
}

template <uint32_t D, uint32_t B>
std::vector<uint8_t> build_raw_uint8_exact_oram_payload(
        const IvfRabitQIndex<D, B>& index,
        uint32_t cluster_id,
        uint32_t local_vector_id) {
    using Layout = RawUint8ExactOramBlockLayout<D, B>;

    if (cluster_id >= index.num_clusters || local_vector_id >= index.len[cluster_id]) {
        throw std::out_of_range("build_raw_uint8_exact_oram_payload: vector address is out of range");
    }

    const uint32_t global_offset = index.start[cluster_id] + local_vector_id;
    std::vector<uint8_t> plain(Layout::kPlainBytes, 0);
    if (index.stores_raw_vectors_as_uint8()) {
        const uint8_t* vec = index.raw_vector_u8_by_cluster_offset(global_offset);
        std::memcpy(plain.data(), vec, Layout::kVectorBytes);
    } else {
        const float* vec = index.raw_vector_by_cluster_offset(global_offset);
        for (uint32_t d = 0; d < D; ++d) {
            const float value = std::max(0.0f, std::min(255.0f, vec[d]));
            plain[d] = static_cast<uint8_t>(value + 0.5f);
        }
    }
    const uint32_t vector_id = index.ids[global_offset];
    std::memcpy(plain.data() + Layout::kVectorBytes, &vector_id, sizeof(vector_id));
    return plain;
}

template <uint32_t D, uint32_t B>
std::vector<uint8_t> build_raw_float32_exact_oram_payload(
        const IvfRabitQIndex<D, B>& index,
        uint32_t cluster_id,
        uint32_t local_vector_id) {
    using Layout = RawFloat32ExactOramBlockLayout<D, B>;

    if (cluster_id >= index.num_clusters || local_vector_id >= index.len[cluster_id]) {
        throw std::out_of_range("build_raw_float32_exact_oram_payload: vector address is out of range");
    }

    const uint32_t global_offset = index.start[cluster_id] + local_vector_id;
    std::vector<uint8_t> plain(Layout::kPlainBytes, 0);
    if (index.stores_raw_vectors_as_uint8()) {
        const uint8_t* vec = index.raw_vector_u8_by_cluster_offset(global_offset);
        for (uint32_t d = 0; d < D; ++d) {
            const float value = static_cast<float>(vec[d]);
            std::memcpy(plain.data() + static_cast<size_t>(d) * sizeof(float),
                        &value, sizeof(value));
        }
    } else {
        const float* vec = index.raw_vector_by_cluster_offset(global_offset);
        std::memcpy(plain.data(), vec, Layout::kVectorBytes);
    }
    const uint32_t vector_id = index.ids[global_offset];
    std::memcpy(plain.data() + Layout::kVectorBytes, &vector_id, sizeof(vector_id));
    return plain;
}

template <uint32_t D, uint32_t B>
std::vector<float> compute_cluster_means_for_exact_oram(const IvfRabitQIndex<D, B>& index) {
    if (!index.has_raw_vectors()) {
        throw std::runtime_error("compute_cluster_means_for_exact_oram requires raw_vectors_by_cluster");
    }

    std::vector<float> means(static_cast<size_t>(index.num_clusters) * D, 0.0f);
    for (uint32_t c = 0; c < index.num_clusters; ++c) {
        if (index.len[c] == 0) {
            continue;
        }
        float* mean = means.data() + static_cast<size_t>(c) * D;
        for (uint32_t i = 0; i < index.len[c]; ++i) {
            for (uint32_t d = 0; d < D; ++d) {
                mean[d] += index.raw_vector_value(index.start[c] + i, d);
            }
        }
        const float inv = 1.0f / static_cast<float>(index.len[c]);
        for (uint32_t d = 0; d < D; ++d) {
            mean[d] *= inv;
        }
    }
    return means;
}

template <uint32_t D, uint32_t B>
void build_and_upload_approx_oram_blocks(const IvfRabitQIndex<D, B>& index, OramWriter& approx_oram) {
    size_t logical_address = 0;
    for (uint32_t c = 0; c < index.num_clusters; ++c) {
        for (uint32_t block_id = 0; block_id < index.cluster_num_blocks(c); ++block_id) {
            std::vector<uint8_t> plain = build_approx_oram_payload(index, c, block_id);
            std::vector<uint32_t> vector_ids(IvfRabitQIndex<D, B>::kBlockVectors, static_cast<uint32_t>(-1));
            std::vector<uint32_t> ring_leaves(IvfRabitQIndex<D, B>::kBlockVectors, static_cast<uint32_t>(-1));
            const uint32_t block_begin = block_id * IvfRabitQIndex<D, B>::kBlockVectors;
            const uint32_t active =
                block_begin >= index.len[c]
                    ? 0
                    : std::min<uint32_t>(IvfRabitQIndex<D, B>::kBlockVectors, index.len[c] - block_begin);
            for (uint32_t j = 0; j < active; ++j) {
                vector_ids[j] = index.ids[index.start[c] + block_begin + j];
                uint32_t leaf = 0;
                if (!approx_oram.ring_leaf_for_vector(vector_ids[j], leaf)) {
                    throw std::logic_error("build_and_upload_approx_oram_blocks requires a RingORAM leaf for every vector");
                }
                ring_leaves[j] = leaf;
            }
            approx_oram.initialize_path_block(
                logical_address,
                c,
                block_id,
                vector_ids,
                ring_leaves,
                plain);
            logical_address++;
        }
    }
    approx_oram.seal();
}

template <uint32_t D, uint32_t B>
void build_and_upload_exact_oram_blocks(const IvfRabitQIndex<D, B>& index, OramWriter& exact_oram) {
    std::vector<float> cluster_means = compute_cluster_means_for_exact_oram(index);
    RaBitQPlus rabitq_plus(D, 8);
    for (uint32_t c = 0; c < index.num_clusters; ++c) {
        for (uint32_t local_id = 0; local_id < index.len[c]; ++local_id) {
            std::vector<uint8_t> plain =
                build_exact_oram_payload(rabitq_plus, index, cluster_means, c, local_id);
            const uint32_t vector_id = index.ids[index.start[c] + local_id];
            exact_oram.initialize_ring_block(vector_id, plain);
        }
    }
    exact_oram.seal();
}

template <uint32_t D, uint32_t B>
void build_and_upload_two_orams(const IvfRabitQIndex<D, B>& index, const TwoOramWriters& writers) {
    if (!writers.approx || !writers.exact) {
        throw std::invalid_argument("build_and_upload_two_orams requires both ORAM writers");
    }
    build_and_upload_exact_oram_blocks(index, *writers.exact);
    build_and_upload_approx_oram_blocks(index, *writers.approx);
}
