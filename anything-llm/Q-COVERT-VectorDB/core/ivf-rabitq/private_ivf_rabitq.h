#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fast_scan.h"
#include "ivf_rabitq_index.h"
#include "oram_block_builders.h"
#include "oram_interface.h"
#include "rabitq_plus.h"
#include "space.h"
#include "types.h"

enum class ExactPayloadFormat {
    Quantized8Bit,
    RawUint8,
    RawFloat32
};

template <uint32_t D, uint32_t B, uint32_t QueryBits = 4>
class ClusterBlockAddressMap {
public:
    ClusterBlockAddressMap() = default;

    explicit ClusterBlockAddressMap(const IvfRabitQIndex<D, B, QueryBits>& index) {
        reset(index);
    }

    void reset(const IvfRabitQIndex<D, B, QueryBits>& index) {
        starts_.assign(index.num_clusters, 0);
        total_blocks_ = 0;
        for (uint32_t c = 0; c < index.num_clusters; ++c) {
            starts_[c] = total_blocks_;
            total_blocks_ += index.cluster_num_blocks(c);
        }
    }

    size_t block_address(uint32_t cluster_id, uint32_t block_id) const {
        return starts_.at(cluster_id) + block_id;
    }

    size_t vector_address(const IvfRabitQIndex<D, B, QueryBits>& index,
                          uint32_t cluster_id,
                          uint32_t local_vector_id) const {
        return static_cast<size_t>(index.start[cluster_id]) + local_vector_id;
    }

    size_t total_blocks() const {
        return total_blocks_;
    }

private:
    std::vector<size_t> starts_;
    size_t total_blocks_ = 0;
};

template <uint32_t D, uint32_t B, uint32_t QueryBits = 4>
class LocalPlainBlockReader {
public:
    using Index = IvfRabitQIndex<D, B, QueryBits>;
    using Factor = typename Index::Factor;

    explicit LocalPlainBlockReader(
            const Index& index,
            ExactPayloadFormat exact_payload_format = ExactPayloadFormat::Quantized8Bit)
        : index_(index),
          rabitq_plus_(D, 8),
          exact_payload_format_(exact_payload_format) {
        compute_cluster_means();
        build_exact_cache();
    }

    std::vector<uint8_t> read_approx(uint32_t cluster_id, uint32_t block_id) const {
        using Layout = ApproxOramBlockLayout<D, B>;
        std::vector<uint8_t> plain(Layout::kPlainBytes, 0);

        const uint32_t block_begin = block_id * Index::kBlockVectors;
        const uint32_t base = index_.start[cluster_id] + block_begin;
        const uint32_t active =
            block_begin >= index_.len[cluster_id]
                ? 0
                : std::min<uint32_t>(Index::kBlockVectors, index_.len[cluster_id] - block_begin);
        for (uint32_t j = 0; j < active; ++j) {
            std::memcpy(plain.data() + static_cast<size_t>(j) * sizeof(Factor),
                        &index_.factors[base + j],
                        sizeof(Factor));
        }
        std::memcpy(plain.data() + Layout::kFactorBytes,
                    index_.packed_block(cluster_id, block_id),
                    Layout::kPackedCodeBytes);
        return plain;
    }

    std::vector<uint8_t> read_exact(uint32_t cluster_id, uint32_t local_vector_id) const {
        using Layout = ExactOramBlockLayout<D, B>;
        const uint32_t global_offset = index_.start[cluster_id] + local_vector_id;
        if (global_offset >= exact_plain_by_cluster_offset_.size()) {
            return std::vector<uint8_t>(Layout::kPlainBytes, 0);
        }
        return exact_plain_by_cluster_offset_[global_offset];
    }

    const float* cluster_mean(uint32_t cluster_id) const {
        return cluster_means_.data() + static_cast<size_t>(cluster_id) * D;
    }

private:
    const Index& index_;
    RaBitQPlus rabitq_plus_;
    ExactPayloadFormat exact_payload_format_;
    std::vector<float> cluster_means_;
    std::vector<std::vector<uint8_t>> exact_plain_by_cluster_offset_;

    void compute_cluster_means() {
        if (!index_.has_raw_vectors()) {
            throw std::runtime_error("LocalPlainBlockReader requires raw_vectors_by_cluster");
        }
        cluster_means_.assign(static_cast<size_t>(index_.num_clusters) * D, 0.0f);
        for (uint32_t c = 0; c < index_.num_clusters; ++c) {
            if (index_.len[c] == 0) {
                continue;
            }
            float* mean = cluster_means_.data() + static_cast<size_t>(c) * D;
            for (uint32_t i = 0; i < index_.len[c]; ++i) {
                for (uint32_t d = 0; d < D; ++d) {
                    mean[d] += index_.raw_vector_value(index_.start[c] + i, d);
                }
            }
            const float inv = 1.0f / static_cast<float>(index_.len[c]);
            for (uint32_t d = 0; d < D; ++d) {
                mean[d] *= inv;
            }
        }
    }

    void build_exact_cache() {
        using Layout = ExactOramBlockLayout<D, B>;
        using RawLayout = RawUint8ExactOramBlockLayout<D, B>;
        const size_t payload_bytes =
            exact_payload_format_ == ExactPayloadFormat::RawUint8
                ? RawLayout::kPlainBytes
                : Layout::kPlainBytes;
        exact_plain_by_cluster_offset_.assign(index_.n, std::vector<uint8_t>(payload_bytes, 0));

        for (uint32_t c = 0; c < index_.num_clusters; ++c) {
            for (uint32_t i = 0; i < index_.len[c]; ++i) {
                const uint32_t global_offset = index_.start[c] + i;
                if (exact_payload_format_ == ExactPayloadFormat::RawUint8) {
                    exact_plain_by_cluster_offset_[global_offset] =
                        build_raw_uint8_exact_oram_payload(index_, c, i);
                } else {
                    const float* mean = cluster_mean(c);
                    std::vector<float> vec_buffer;
                    const float* vec = nullptr;
                    if (index_.stores_raw_vectors_as_uint8()) {
                        vec_buffer = index_.raw_vector_as_float(global_offset);
                        vec = vec_buffer.data();
                    } else {
                        vec = index_.raw_vector_by_cluster_offset(global_offset);
                    }
                    std::vector<RaBitQPlus::QuantizeResult> qrs = rabitq_plus_.quantize(mean, vec, 1);
                    std::vector<uint8_t> ser = qrs[0].serialize();
                    if (ser.size() > Layout::kVectorBytes) {
                        throw std::runtime_error("RaBitQPlus serialized vector exceeds exact layout");
                    }
                    std::vector<uint8_t>& plain = exact_plain_by_cluster_offset_[global_offset];
                    std::memcpy(plain.data(), ser.data(), ser.size());
                    const uint32_t vector_id = index_.ids[global_offset];
                    std::memcpy(plain.data() + Layout::kVectorBytes, &vector_id, sizeof(vector_id));
                }
            }
        }
    }
};

template <uint32_t D, uint32_t B, uint32_t QueryBits = 4>
class PrivateIvfRabitQSearcher {
public:
    using Index = IvfRabitQIndex<D, B, QueryBits>;
    using Factor = typename Index::Factor;
    using SpaceT = Space<D, B, QueryBits>;

    struct Counters {
        uint64_t approximate_distance_count = 0;
        uint64_t exact_distance_count = 0;
        uint64_t approx_oram_reads = 0;
        uint64_t exact_oram_reads = 0;
        double centroid_select_time_sec = 0.0;
        double path_read_time_sec = 0.0;
        double approx_scan_time_sec = 0.0;
        double candidate_select_time_sec = 0.0;
        double ring_read_time_sec = 0.0;
        double exact_compute_time_sec = 0.0;
        double path_finish_time_sec = 0.0;
        double ring_finish_time_sec = 0.0;
    };

    explicit PrivateIvfRabitQSearcher(
            const Index& index,
            ExactPayloadFormat exact_payload_format = ExactPayloadFormat::Quantized8Bit)
        : index_(index),
          address_map_(index),
          rabitq_plus_(D, 8),
          exact_payload_format_(exact_payload_format) {
        if (exact_payload_format_ == ExactPayloadFormat::Quantized8Bit) {
            compute_cluster_means_from_raw_if_available();
        }
    }

    ResultHeap search_local_plain(const float* query,
                                  const float* randomized_query,
                                  uint32_t k,
                                  uint32_t nprobe,
                                  LocalPlainBlockReader<D, B, QueryBits>& reader,
                                  float dist_k = std::numeric_limits<float>::max()) {
        auto approx_reader = [&](uint32_t c, uint32_t block_id) {
            counters_.approx_oram_reads++;
            return reader.read_approx(c, block_id);
        };
        auto exact_reader = [&](uint32_t c, uint32_t local_vector_id) {
            counters_.exact_oram_reads++;
            return reader.read_exact(c, local_vector_id);
        };
        auto cluster_ref = [&](uint32_t c) {
            return reader.cluster_mean(c);
        };
        return search_impl(query, randomized_query, k, nprobe, dist_k, approx_reader, exact_reader, cluster_ref);
    }

    ResultHeap search_oram(const float* query,
                           const float* randomized_query,
                           uint32_t k,
                           uint32_t nprobe,
                           const TwoOramReaders& orams,
                           float dist_k = std::numeric_limits<float>::max()) {
        if (!orams.approx || !orams.exact) {
            throw std::invalid_argument("search_oram requires both approximate and exact ORAM readers");
        }
        if (exact_payload_format_ == ExactPayloadFormat::Quantized8Bit && cluster_means_.empty()) {
            throw std::runtime_error("search_oram needs cluster means; provide them when raw vectors are not resident");
        }

        auto approx_reader = [&](uint32_t c, uint32_t block_id) {
            counters_.approx_oram_reads++;
            return orams.approx->read(address_map_.block_address(c, block_id));
        };
        auto exact_reader = [&](uint32_t c, uint32_t local_vector_id) {
            counters_.exact_oram_reads++;
            return orams.exact->read(address_map_.vector_address(index_, c, local_vector_id));
        };
        auto cluster_ref = [&](uint32_t c) {
            return cluster_mean_or_null(c);
        };

        ResultHeap out = search_impl(query, randomized_query, k, nprobe, dist_k, approx_reader, exact_reader, cluster_ref);
        orams.approx->end_query();
        orams.exact->end_query();
        return out;
    }

    ResultHeap search_double_oram(const float* query,
                                  const float* randomized_query,
                                  uint32_t k,
                                  uint32_t nprobe,
                                  AnnDoubleOramReader& oram,
                                  uint32_t path_batch_clusters = 4,
                                  uint32_t exact_candidate_limit = 0,
                                  float dist_k = std::numeric_limits<float>::max()) {
        if (exact_payload_format_ == ExactPayloadFormat::Quantized8Bit && cluster_means_.empty()) {
            throw std::runtime_error("search_double_oram needs cluster means; provide them when raw vectors are not resident");
        }
        if (path_batch_clusters == 0) {
            throw std::invalid_argument("path_batch_clusters must be positive");
        }

        auto cluster_ref = [&](uint32_t c) {
            return cluster_mean_or_null(c);
        };

        ResultHeap out = search_double_oram_impl(
            query,
            randomized_query,
            k,
            nprobe,
            dist_k,
            oram,
            path_batch_clusters,
            exact_candidate_limit,
            cluster_ref);
        const auto t_end_query = std::chrono::high_resolution_clock::now();
        oram.end_query();
        counters_.ring_finish_time_sec += elapsed_sec_since(t_end_query);
        return out;
    }

    void set_cluster_means(std::vector<float> means) {
        if (means.size() != static_cast<size_t>(index_.num_clusters) * D) {
            throw std::invalid_argument("set_cluster_means: wrong size");
        }
        cluster_means_ = std::move(means);
    }

    const Counters& counters() const {
        return counters_;
    }

    void reset_counters() {
        counters_ = Counters{};
    }

private:
    const Index& index_;
    ClusterBlockAddressMap<D, B, QueryBits> address_map_;
    RaBitQPlus rabitq_plus_;
    ExactPayloadFormat exact_payload_format_;
    Counters counters_;
    std::vector<float> cluster_means_;

    static double elapsed_sec_since(std::chrono::high_resolution_clock::time_point t0) {
        return std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    void compute_cluster_means_from_raw_if_available() {
        if (!index_.has_raw_vectors()) {
            return;
        }
        cluster_means_.assign(static_cast<size_t>(index_.num_clusters) * D, 0.0f);
        for (uint32_t c = 0; c < index_.num_clusters; ++c) {
            if (index_.len[c] == 0) {
                continue;
            }
            float* mean = cluster_means_.data() + static_cast<size_t>(c) * D;
            for (uint32_t i = 0; i < index_.len[c]; ++i) {
                for (uint32_t d = 0; d < D; ++d) {
                    mean[d] += index_.raw_vector_value(index_.start[c] + i, d);
                }
            }
            const float inv = 1.0f / static_cast<float>(index_.len[c]);
            for (uint32_t d = 0; d < D; ++d) {
                mean[d] *= inv;
            }
        }
    }

    const float* cluster_mean_or_null(uint32_t cluster_id) const {
        if (cluster_means_.empty()) {
            return nullptr;
        }
        return cluster_means_.data() + static_cast<size_t>(cluster_id) * D;
    }

    template <typename ApproxReader, typename ExactReader, typename ClusterRef>
    ResultHeap search_impl(const float* query,
                           const float* randomized_query,
                           uint32_t k,
                           uint32_t nprobe,
                           float dist_k,
                           ApproxReader&& approx_reader,
                           ExactReader&& exact_reader,
                           ClusterRef&& cluster_ref) {
        reset_counters();
        ResultHeap knns;
        if (index_.num_clusters == 0 || k == 0 || nprobe == 0) {
            return knns;
        }
        nprobe = std::min(nprobe, index_.num_clusters);

        std::vector<Result> centroid_dist(index_.num_clusters);
        {
            const auto t_centroid = std::chrono::high_resolution_clock::now();
            for (uint32_t c = 0; c < index_.num_clusters; ++c) {
                centroid_dist[c].first = sqr_dist<B>(randomized_query,
                                                     index_.randomized_centroids.data() + static_cast<size_t>(c) * B);
                centroid_dist[c].second = c;
            }
            std::partial_sort(centroid_dist.begin(), centroid_dist.begin() + nprobe, centroid_dist.end());
            counters_.centroid_select_time_sec += elapsed_sec_since(t_centroid);
        }

        uint8_t PORTABLE_ALIGN64 byte_query[B];
        for (uint32_t pi = 0; pi < nprobe; ++pi) {
            const uint32_t c = centroid_dist[pi].second;
            const float sqr_y = centroid_dist[pi].first;
            const uint32_t cluster_len = index_.len[c];
            if (cluster_len == 0) {
                continue;
            }

            float vl = 0.0f;
            float vr = 0.0f;
            SpaceT::range(randomized_query,
                          index_.randomized_centroids.data() + static_cast<size_t>(c) * B,
                          vl,
                          vr);
            const float width = (vr - vl) / static_cast<float>((1u << QueryBits) - 1u);
            uint32_t sum_q = 0;
            SpaceT::quantize(byte_query,
                             randomized_query,
                             index_.randomized_centroids.data() + static_cast<size_t>(c) * B,
                             index_.query_quantization_offsets.data(),
                             vl,
                             width,
                             sum_q);

            uint8_t PORTABLE_ALIGN32 lut[B / 4 * 16];
            pack_lut_4bit<B>(byte_query, lut);
            for (uint32_t i = 0; i < B / 4 * 16; ++i) {
                lut[i] = static_cast<uint8_t>(lut[i] * 2);
            }

            const uint32_t num_blocks = static_cast<uint32_t>(index_.cluster_num_blocks(c));
            for (uint32_t block_id = 0; block_id < num_blocks; ++block_id) {
                scan_approx_block(query,
                                  c,
                                  block_id,
                                  cluster_len,
                                  lut,
                                  sqr_y,
                                  vl,
                                  width,
                                  static_cast<float>(sum_q),
                                  cluster_ref(c),
                                  dist_k,
                                  k,
                                  knns,
                                  approx_reader,
                                  exact_reader);
            }
        }
        return knns;
    }

    template <typename ClusterRef>
    ResultHeap search_double_oram_impl(const float* query,
                                       const float* randomized_query,
                                       uint32_t k,
                                       uint32_t nprobe,
                                       float dist_k,
                                       AnnDoubleOramReader& oram,
                                       uint32_t path_batch_clusters,
                                       uint32_t exact_candidate_limit,
                                       ClusterRef&& cluster_ref) {
        struct ApproxCandidate {
            float quantized_distance = 0.0f;
            uint32_t cluster_id = 0;
            uint32_t local_vector_id = 0;
            uint32_t vector_id = 0;
        };

        reset_counters();
        ResultHeap knns;
        if (index_.num_clusters == 0 || k == 0 || nprobe == 0) {
            return knns;
        }
        nprobe = std::min(nprobe, index_.num_clusters);
        const uint64_t derived_candidate_limit =
            exact_candidate_limit == 0
                ? static_cast<uint64_t>(k) * static_cast<uint64_t>(nprobe)
                : static_cast<uint64_t>(exact_candidate_limit);
        const size_t candidate_limit = static_cast<size_t>(derived_candidate_limit);
        if (candidate_limit == 0) {
            return knns;
        }

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
            const auto t_centroid = std::chrono::high_resolution_clock::now();
            for (uint32_t c = 0; c < index_.num_clusters; ++c) {
                centroid_dist[c].first = sqr_dist<B>(
                    randomized_query,
                    index_.randomized_centroids.data() + static_cast<size_t>(c) * B);
                centroid_dist[c].second = c;
            }
            std::partial_sort(centroid_dist.begin(), centroid_dist.begin() + nprobe, centroid_dist.end());
            counters_.centroid_select_time_sec += elapsed_sec_since(t_centroid);
        }

        uint8_t PORTABLE_ALIGN64 byte_query[B];
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

        for (uint32_t batch_begin = 0; batch_begin < nprobe; batch_begin += path_batch_clusters) {
            const uint32_t batch_end = std::min<uint32_t>(nprobe, batch_begin + path_batch_clusters);
            std::vector<uint32_t> cluster_batch;
            cluster_batch.reserve(batch_end - batch_begin);
            for (uint32_t pi = batch_begin; pi < batch_end; ++pi) {
                const uint32_t c = centroid_dist[pi].second;
                if (index_.len[c] != 0) {
                    cluster_batch.push_back(c);
                }
            }
            if (cluster_batch.empty()) {
                continue;
            }

            const auto t_path_read = std::chrono::high_resolution_clock::now();
            std::vector<AnnApproxBlock> approx_blocks = oram.read_clusters(cluster_batch);
            counters_.path_read_time_sec += elapsed_sec_since(t_path_read);
            counters_.approx_oram_reads += approx_blocks.size();

            std::unordered_map<uint64_t, AnnApproxBlock*> blocks_by_id;
            blocks_by_id.reserve(approx_blocks.size());
            for (AnnApproxBlock& block : approx_blocks) {
                const uint64_t key = (static_cast<uint64_t>(block.cluster_id) << 32) | block.block_id;
                blocks_by_id[key] = &block;
            }

            std::vector<ApproxCandidate> candidates;
            {
                const auto t_approx_scan = std::chrono::high_resolution_clock::now();
                for (uint32_t c : cluster_batch) {
                    float centroid_sqr = 0.0f;
                    for (uint32_t pi = batch_begin; pi < batch_end; ++pi) {
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
                    for (uint32_t i = 0; i < B / 4 * 16; ++i) {
                        lut[i] = static_cast<uint8_t>(lut[i] * 2);
                    }

                    const uint32_t num_blocks = static_cast<uint32_t>(index_.cluster_num_blocks(c));
                    for (uint32_t block_id = 0; block_id < num_blocks; ++block_id) {
                        const uint64_t key = (static_cast<uint64_t>(c) << 32) | block_id;
                        auto block_it = blocks_by_id.find(key);
                        if (block_it == blocks_by_id.end()) {
                            continue;
                        }
                        collect_double_oram_quantized_candidates(
                            c,
                            block_id,
                            index_.len[c],
                            lut,
                            centroid_sqr,
                            vl,
                            width,
                            static_cast<float>(sum_q),
                            *block_it->second,
                            candidates);
                    }
                }
                counters_.approx_scan_time_sec += elapsed_sec_since(t_approx_scan);
            }

            if (!candidates.empty()) {
                const auto t_candidate_select = std::chrono::high_resolution_clock::now();
                for (const ApproxCandidate& candidate : candidates) {
                    keep_candidate(candidate);
                }
                counters_.candidate_select_time_sec += elapsed_sec_since(t_candidate_select);
            }
        }

        if (!top_candidates.empty()) {
            const auto t_candidate_select = std::chrono::high_resolution_clock::now();
            std::sort_heap(top_candidates.begin(), top_candidates.end(), candidate_less);
            counters_.candidate_select_time_sec += elapsed_sec_since(t_candidate_select);

            counters_.exact_distance_count += top_candidates.size();
            counters_.exact_oram_reads += top_candidates.size();
            std::vector<uint32_t> exact_vector_ids;
            exact_vector_ids.reserve(top_candidates.size());
            for (const ApproxCandidate& candidate : top_candidates) {
                exact_vector_ids.push_back(candidate.vector_id);
            }

            const auto t_ring_read = std::chrono::high_resolution_clock::now();
            std::vector<AnnExactVector> exact_reads = oram.read_exact_vectors(exact_vector_ids);
            counters_.ring_read_time_sec += elapsed_sec_since(t_ring_read);

            const auto t_path_finish = std::chrono::high_resolution_clock::now();
            oram.finish_clusters();
            counters_.path_finish_time_sec += elapsed_sec_since(t_path_finish);

            std::unordered_map<uint32_t, std::vector<uint8_t>> exact_by_id;
            exact_by_id.reserve(exact_reads.size());
            for (AnnExactVector& read : exact_reads) {
                exact_by_id[read.vector_id] = std::move(read.payload);
            }

            const auto t_exact_compute = std::chrono::high_resolution_clock::now();
            for (const ApproxCandidate& candidate : top_candidates) {
                auto exact_it = exact_by_id.find(candidate.vector_id);
                if (exact_it == exact_by_id.end()) {
                    continue;
                }
                update_knn_from_exact_payload(
                    query,
                    cluster_ref(candidate.cluster_id),
                    candidate.vector_id,
                    exact_it->second,
                    dist_k,
                    k,
                    knns);
            }
            counters_.exact_compute_time_sec += elapsed_sec_since(t_exact_compute);
        } else {
            const auto t_path_finish = std::chrono::high_resolution_clock::now();
            oram.finish_clusters();
            counters_.path_finish_time_sec += elapsed_sec_since(t_path_finish);
        }
        return knns;
    }

    template <typename ApproxReader, typename ExactReader>
    void scan_approx_block(const float* query,
                           uint32_t cluster_id,
                           uint32_t block_id,
                           uint32_t cluster_len,
                           const uint8_t* lut,
                           float sqr_y,
                           float vl,
                           float width,
                           float sum_q,
                           const float* exact_centroid,
                           float& dist_k,
                           uint32_t k,
                           ResultHeap& knns,
                           ApproxReader&& approx_reader,
                           ExactReader&& exact_reader) {
        using ApproxLayout = ApproxOramBlockLayout<D, B>;
        std::vector<uint8_t> approx_plain = approx_reader(cluster_id, block_id);
        if (approx_plain.size() < ApproxLayout::kPlainBytes) {
            return;
        }

        const uint8_t* packed = approx_plain.data() + ApproxLayout::kFactorBytes;
        const auto* factors = reinterpret_cast<const Factor*>(approx_plain.data());

        uint16_t PORTABLE_ALIGN32 result[Index::kBlockVectors];
        accumulate<B>(1, packed, lut, result);

        const float y = std::sqrt(sqr_y);
        const uint32_t block_begin = block_id * Index::kBlockVectors;
        const uint32_t active =
            block_begin >= cluster_len ? 0 : std::min<uint32_t>(Index::kBlockVectors, cluster_len - block_begin);

        for (uint32_t j = 0; j < active; ++j) {
            counters_.approximate_distance_count++;
            const Factor& f = factors[j];
            const float tmp_dist =
                f.sqr_x + sqr_y + f.factor_ppc * vl + (result[j] - sum_q) * f.factor_ip * width;
            const float low_dist = tmp_dist - y * f.error;
            if (low_dist >= dist_k) {
                continue;
            }

            counters_.exact_distance_count++;
            const uint32_t local_vector_id = block_begin + j;
            std::vector<uint8_t> exact_plain = exact_reader(cluster_id, local_vector_id);
            uint32_t vector_id = index_.ids[index_.start[cluster_id] + local_vector_id];

            float exact_dist = 0.0f;
            if (!compute_exact_distance_from_payload(
                    query,
                    exact_centroid,
                    vector_id,
                    exact_plain,
                    exact_dist)) {
                continue;
            }
            if (exact_dist < dist_k) {
                knns.emplace(exact_dist, vector_id);
                if (knns.size() > k) {
                    knns.pop();
                }
                if (knns.size() == k) {
                    dist_k = knns.top().first;
                }
            }
        }
    }

    void scan_double_oram_approx_block(const float* query,
                                       uint32_t cluster_id,
                                       uint32_t block_id,
                                       uint32_t cluster_len,
                                       const uint8_t* lut,
                                       float sqr_y,
                                       float vl,
                                       float width,
                                       float sum_q,
                                       const float* exact_centroid,
                                       float& dist_k,
                                       uint32_t k,
                                       ResultHeap& knns,
                                       const AnnApproxBlock& approx_block,
                                       AnnDoubleOramReader& oram) {
        using ApproxLayout = ApproxOramBlockLayout<D, B>;
        if (approx_block.payload.size() < ApproxLayout::kPlainBytes) {
            return;
        }

        const uint8_t* packed = approx_block.payload.data() + ApproxLayout::kFactorBytes;
        const auto* factors = reinterpret_cast<const Factor*>(approx_block.payload.data());

        uint16_t PORTABLE_ALIGN32 result[Index::kBlockVectors];
        accumulate<B>(1, packed, lut, result);

        const float y = std::sqrt(sqr_y);
        const uint32_t block_begin = block_id * Index::kBlockVectors;
        const uint32_t active =
            block_begin >= cluster_len ? 0 : std::min<uint32_t>(Index::kBlockVectors, cluster_len - block_begin);

        std::vector<uint32_t> exact_vector_ids;
        exact_vector_ids.reserve(active);

        for (uint32_t j = 0; j < active; ++j) {
            counters_.approximate_distance_count++;
            const Factor& f = factors[j];
            const float tmp_dist =
                f.sqr_x + sqr_y + f.factor_ppc * vl + (result[j] - sum_q) * f.factor_ip * width;
            const float low_dist = tmp_dist - y * f.error;
            if (low_dist >= dist_k) {
                continue;
            }
            if (j >= approx_block.vector_ids.size()) {
                continue;
            }

            exact_vector_ids.push_back(approx_block.vector_ids[j]);
        }

        if (exact_vector_ids.empty()) {
            return;
        }

        counters_.exact_distance_count += exact_vector_ids.size();
        counters_.exact_oram_reads += exact_vector_ids.size();
        std::vector<AnnExactVector> exact_reads = oram.read_exact_vectors(exact_vector_ids);
        std::unordered_map<uint32_t, std::vector<uint8_t>> exact_by_id;
        exact_by_id.reserve(exact_reads.size());
        for (AnnExactVector& read : exact_reads) {
            exact_by_id[read.vector_id] = std::move(read.payload);
        }

        for (uint32_t vector_id : exact_vector_ids) {
            auto exact_it = exact_by_id.find(vector_id);
            if (exact_it == exact_by_id.end()) {
                continue;
            }

            const std::vector<uint8_t>& exact_plain = exact_it->second;
            float exact_dist = 0.0f;
            if (!compute_exact_distance_from_payload(
                    query,
                    exact_centroid,
                    vector_id,
                    exact_plain,
                    exact_dist)) {
                continue;
            }
            if (exact_dist < dist_k) {
                knns.emplace(exact_dist, vector_id);
                if (knns.size() > k) {
                    knns.pop();
                }
                if (knns.size() == k) {
                    dist_k = knns.top().first;
                }
            }
        }
    }

    template <typename Candidate>
    void collect_double_oram_quantized_candidates(uint32_t cluster_id,
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

        for (uint32_t j = 0; j < active; ++j) {
            counters_.approximate_distance_count++;
            const Factor& f = factors[j];
            const float tmp_dist =
                f.sqr_x + sqr_y + f.factor_ppc * vl + (result[j] - sum_q) * f.factor_ip * width;
            if (j >= approx_block.vector_ids.size()) {
                continue;
            }
            const uint32_t vector_id = approx_block.vector_ids[j];
            if (vector_id == static_cast<uint32_t>(-1)) {
                continue;
            }

            Candidate candidate;
            candidate.quantized_distance = tmp_dist;
            candidate.cluster_id = cluster_id;
            candidate.local_vector_id = block_begin + j;
            candidate.vector_id = vector_id;
            candidates.push_back(candidate);
        }
    }

    void update_knn_from_exact_payload(const float* query,
                                       const float* exact_centroid,
                                       uint32_t vector_id,
                                       const std::vector<uint8_t>& exact_plain,
                                       float& dist_k,
                                       uint32_t k,
                                       ResultHeap& knns) {
        float exact_dist = 0.0f;
        if (!compute_exact_distance_from_payload(
                query,
                exact_centroid,
                vector_id,
                exact_plain,
                exact_dist)) {
            return;
        }

        if (exact_dist < dist_k) {
            knns.emplace(exact_dist, vector_id);
            if (knns.size() > k) {
                knns.pop();
            }
            if (knns.size() == k) {
                dist_k = knns.top().first;
            }
        }
    }

    bool compute_exact_distance_from_payload(const float* query,
                                             const float* exact_centroid,
                                             uint32_t expected_vector_id,
                                             const std::vector<uint8_t>& exact_plain,
                                             float& exact_dist) const {
        if (exact_payload_format_ == ExactPayloadFormat::RawUint8) {
            using RawLayout = RawUint8ExactOramBlockLayout<D, B>;
            if (exact_plain.size() < RawLayout::kPlainBytes) {
                return false;
            }
            uint32_t stored_vector_id = 0;
            std::memcpy(&stored_vector_id, exact_plain.data() + RawLayout::kVectorBytes, sizeof(stored_vector_id));
            if (stored_vector_id != expected_vector_id) {
                throw std::runtime_error("DoubleORAM raw exact block returned a mismatched vector id");
            }

            float dist = 0.0f;
            for (uint32_t d = 0; d < D; ++d) {
                const float diff = query[d] - static_cast<float>(exact_plain[d]);
                dist += diff * diff;
            }
            exact_dist = dist;
            return true;
        }

        if (exact_payload_format_ == ExactPayloadFormat::RawFloat32) {
            using RawLayout = RawFloat32ExactOramBlockLayout<D, B>;
            if (exact_plain.size() < RawLayout::kPlainBytes) {
                return false;
            }
            uint32_t stored_vector_id = 0;
            std::memcpy(
                &stored_vector_id,
                exact_plain.data() + RawLayout::kVectorBytes,
                sizeof(stored_vector_id));
            if (stored_vector_id != expected_vector_id) {
                throw std::runtime_error(
                    "DoubleORAM raw-float32 exact block returned a mismatched vector id");
            }
            float dist = 0.0f;
            for (uint32_t d = 0; d < D; ++d) {
                float stored = 0.0f;
                std::memcpy(&stored, exact_plain.data() + static_cast<size_t>(d) * sizeof(float),
                            sizeof(stored));
                const float difference = query[d] - stored;
                dist += difference * difference;
            }
            exact_dist = dist;
            return true;
        }

        using ExactLayout = ExactOramBlockLayout<D, B>;
        if (exact_plain.size() < ExactLayout::kPlainBytes) {
            return false;
        }

        auto [qr, used] = RaBitQPlus::QuantizeResult::deserialize(exact_plain.data(), ExactLayout::kVectorBytes);
        if (used == 0) {
            return false;
        }

        uint32_t stored_vector_id = 0;
        std::memcpy(&stored_vector_id, exact_plain.data() + ExactLayout::kVectorBytes, sizeof(stored_vector_id));
        if (stored_vector_id != expected_vector_id) {
            throw std::runtime_error("DoubleORAM exact block returned a mismatched vector id");
        }

        exact_dist = rabitq_plus_.compute_distance(query, exact_centroid, qr);
        return true;
    }
};
