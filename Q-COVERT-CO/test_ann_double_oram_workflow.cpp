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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

using namespace double_oram;

namespace {

constexpr uint32_t kD = 8;
constexpr uint32_t kB = 64;
constexpr uint32_t kQueryBits = 4;
constexpr uint32_t kClusters = 4;
constexpr uint32_t kVectors = 64;
constexpr uint32_t kPathsPerCluster = 2;
constexpr int kClusterCachedLevels = 4;
constexpr int kRingRealBucketSize = 32;
constexpr int kRingDummySize = 48;
constexpr int kRingEvictRate = 48;

using Index = IvfRabitQIndex<kD, kB, kQueryBits>;
using ApproxLayout = ApproxOramBlockLayout<kD, kB>;
using ExactLayout = ExactOramBlockLayout<kD, kB>;

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

void place_ring_blocks(
        FuORAM& ring_oram,
        std::map<int, std::vector<Block*>>& buckets,
        std::vector<int>& latest_ring_leaf,
        const std::vector<std::vector<uint8_t>>& exact_payloads,
        int ring_block_size,
        std::mt19937& rng) {
    std::uniform_int_distribution<int> leaf_dist(0, ring_oram.getNumLeaves() - 1);
    std::vector<int> used(ring_oram.getNumBuckets(), 0);

    for (uint32_t vector_id = 0; vector_id < exact_payloads.size(); vector_id++) {
        bool placed = false;
        for (int attempt = 0; attempt < 100000 && !placed; attempt++) {
            int leaf = leaf_dist(rng);
            for (int level = ring_oram.getNumLevels() - 1; level >= 0; level--) {
                int pos = ring_oram.P(leaf, level);
                if (used[pos] < kRingRealBucketSize) {
                    int slot = used[pos]++;
                    delete buckets[pos][slot];
                    buckets[pos][slot] = make_block(
                        ring_block_size,
                        static_cast<int>(vector_id),
                        bytes_to_block_data(ring_block_size, exact_payloads[vector_id]));
                    latest_ring_leaf[vector_id] = leaf;
                    placed = true;
                    break;
                }
            }
        }

        if (!placed) {
            throw std::runtime_error("failed to place ANN FuORAM vector block");
        }
    }
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

Matrix<float> make_base_vectors() {
    Matrix<float> x(kVectors, kD);
    for (uint32_t i = 0; i < kVectors; i++) {
        for (uint32_t d = 0; d < kD; d++) {
            x.row(i)[d] = static_cast<float>((i % 17) * 0.1 + d * 0.03 + (i / kClusters) * 0.01);
        }
    }
    return x;
}

Index make_index(const Matrix<float>& x) {
    Matrix<float> randomized_centroid(kClusters, kB);
    Matrix<float> dist_to_centroid(kVectors, 1);
    Matrix<float> x0(kVectors, 1);
    Matrix<uint32_t> cluster_id(kVectors, 1);
    Matrix<uint64_t> binary(kVectors, kB / 64);

    for (uint32_t c = 0; c < kClusters; c++) {
        for (uint32_t b = 0; b < kB; b++) {
            randomized_centroid.row(c)[b] = static_cast<float>(c * 0.2 + b * 0.001);
        }
    }

    for (uint32_t i = 0; i < kVectors; i++) {
        uint32_t c = i % kClusters;
        cluster_id.row(i)[0] = c;
        dist_to_centroid.row(i)[0] = 1.0f + static_cast<float>(i % 5) * 0.1f;
        x0.row(i)[0] = 2.0f + static_cast<float>(i % 3) * 0.1f;
        for (uint32_t word = 0; word < kB / 64; word++) {
            binary.row(i)[word] = 0x9e3779b97f4a7c15ULL ^ (static_cast<uint64_t>(i) << 17) ^ word;
        }
    }

    return Index(x, randomized_centroid, dist_to_centroid, x0, cluster_id, binary);
}

std::vector<std::vector<uint8_t>> build_exact_payloads(const Index& index) {
    std::vector<std::vector<uint8_t>> exact_payloads(index.n);
    for (uint32_t c = 0; c < index.num_clusters; c++) {
        for (uint32_t local_id = 0; local_id < index.len[c]; local_id++) {
            uint32_t vector_id = index.ids[index.start[c] + local_id];
            exact_payloads[vector_id] = build_exact_oram_payload(index, c, local_id);
        }
    }
    return exact_payloads;
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

std::set<uint32_t> heap_ids(ResultHeap heap) {
    std::set<uint32_t> ids;
    while (!heap.empty()) {
        ids.insert(heap.top().second);
        heap.pop();
    }
    return ids;
}

void print_heap(const char* name, ResultHeap heap) {
    std::vector<Result> values;
    while (!heap.empty()) {
        values.push_back(heap.top());
        heap.pop();
    }
    std::sort(values.begin(), values.end());
    std::cerr << name << ":";
    for (const Result& result : values) {
        std::cerr << " (" << result.second << "," << result.first << ")";
    }
    std::cerr << "\n";
}

} // namespace

int main() {
    try {
        Matrix<float> base = make_base_vectors();
        Index index = make_index(base);
        bool rejected_fractional_uint8 = false;
        try {
            build_raw_uint8_exact_oram_payload(index, 0, 0);
        } catch (const std::invalid_argument&) {
            rejected_fractional_uint8 = true;
        }
        if (!rejected_fractional_uint8) {
            throw std::runtime_error("Raw uint8 storage accepted fractional coordinates");
        }
        std::vector<std::vector<uint8_t>> exact_payloads = build_exact_payloads(index);

        const int cluster_block_size = ann_path_block_size();
        const int ring_block_size = packed_byte_int_count(static_cast<int>(ExactLayout::kPlainBytes));
        const int cluster_num_blocks = std::max(8, total_path_blocks(index));
        std::vector<int> cluster_block_counts(index.num_clusters, 0);
        for (uint32_t c = 0; c < index.num_clusters; c++) {
            cluster_block_counts[c] = static_cast<int>(index.cluster_num_blocks(c));
        }
        const int cluster_num_levels = choose_cluster_levels(cluster_num_blocks, kRingRealBucketSize);
        const int cluster_cached_levels = std::min(kClusterCachedLevels, std::max(0, cluster_num_levels - 2));
        // Reserve additional FuORAM block IDs so this workflow can exercise
        // online insertion without rebuilding the physical tree.
        const int ring_block_capacity = static_cast<int>(index.n) + 32;
        const int ring_num_levels = choose_ring_levels_with_load_slack(ring_block_capacity, kRingRealBucketSize);

        Bucket::resetState();
        SBucket::resetState();

        RingOramConfig cluster_config(
            cluster_block_size,
            kRingRealBucketSize,
            kRingDummySize,
            kRingEvictRate,
            cluster_num_blocks,
            cluster_num_levels,
            cluster_cached_levels);
        RingOramConfig ring_config(
            ring_block_size,
            kRingRealBucketSize,
            kRingDummySize,
            kRingEvictRate,
            ring_block_capacity,
            ring_num_levels,
            0);

        RandomForOram random;
        RemoteRing cluster_server(nullptr, cluster_config, true, true, false);
        RemoteRing cluster_client(&cluster_server, cluster_config, false);
        QuORAM cluster_oram(&cluster_client, &random, cluster_config, cluster_num_levels, cluster_block_counts);

        RemoteRing ring_server(nullptr, ring_config, true, true, false);
        RemoteRing ring_client(&ring_server, ring_config, false);
        FuORAM ring_oram(&ring_client, &random, ring_config, ring_num_levels, 0, true, true);

        std::mt19937 init_rng(20260623);
        std::vector<int> latest_ring_leaf(index.n, -1);
        std::map<int, std::vector<Block*>> ring_buckets =
            make_dummy_buckets(ring_config.num_buckets, ring_config.bucket_size, ring_block_size);
        place_ring_blocks(ring_oram, ring_buckets, latest_ring_leaf, exact_payloads, ring_block_size, init_rng);
        install_ring_metadata(ring_oram, ring_buckets, latest_ring_leaf);
        ring_client.WriteBucketBatchMapAsBlockRing(ring_buckets, ring_config.bucket_size, false);

        std::map<int, std::vector<Block*>> cluster_buckets =
            make_dummy_buckets(cluster_config.num_buckets, cluster_config.bucket_size, cluster_block_size);
        std::vector<int> cluster_used(cluster_config.num_buckets, 0);
        uint32_t logical_address = 0;
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
        }
        cluster_client.WriteBucketBatchMapAsBlockRing(cluster_buckets, cluster_config.bucket_size, false);

        DoubleORAMConfig config(
            static_cast<int>(index.num_clusters),
            static_cast<int>(ApproxLayout::kPlainBytes),
            kD,
            static_cast<int>(Index::kBlockVectors),
            kPathsPerCluster,
            0,
            DoubleORAMPathBlockLayout::AnnBatch);
        DoubleORAM double_oram(cluster_oram, ring_oram, config);
        // Each cluster has one real block; query with two paths per cluster.
        DoubleOramAnnReader ann_oram(double_oram, static_cast<int>(ExactLayout::kPlainBytes), 2);

        PrivateIvfRabitQSearcher<kD, kB, kQueryBits> local_searcher(index);
        PrivateIvfRabitQSearcher<kD, kB, kQueryBits> oram_searcher(index);
        LocalPlainBlockReader<kD, kB, kQueryBits> local_reader(index);

        // Eight queries force the padded paths through an early reshuffle.
        Matrix<float> query(8, kD);
        for (uint32_t qi = 0; qi < query.n; qi++) {
            std::memcpy(query.row(qi), base.row(qi * 7), sizeof(float) * kD);
        }
        Matrix<float> randomized_query(query.n, kB);
        for (uint32_t qi = 0; qi < query.n; qi++) {
            for (uint32_t b = 0; b < kB; b++) {
                randomized_query.row(qi)[b] = static_cast<float>(0.01 * b + qi * 0.05);
            }
        }

        for (uint32_t qi = 0; qi < query.n; qi++) {
            ResultHeap local = local_searcher.search_local_plain(
                query.row(qi),
                randomized_query.row(qi),
                5,
                index.num_clusters,
                local_reader);
            cluster_oram.reset_query_access_stats();
            ring_oram.reset_query_access_stats();
            ResultHeap oram = oram_searcher.search_double_oram(
                query.row(qi),
                randomized_query.row(qi),
                5,
                index.num_clusters,
                ann_oram,
                index.num_clusters,
                index.n);
            ResultHeap expected;
            for (uint32_t id = 0; id < base.n; ++id) {
                double distance = 0;
                for (uint32_t d = 0; d < kD; ++d) {
                    const double delta = double(query.row(qi)[d]) - double(base.row(id)[d]);
                    distance += delta * delta;
                }
                expected.emplace(distance, id);
                if (expected.size() > 5) expected.pop();
            }
            auto actual = oram;
            if (actual.size() != expected.size()) {
                throw std::runtime_error("Raw-coordinate reranking returned the wrong result count");
            }
            while (!actual.empty()) {
                if (actual.top() != expected.top()) {
                    throw std::runtime_error("Raw-coordinate reranking differs from brute-force squared L2");
                }
                actual.pop();
                expected.pop();
            }
            if (cluster_oram.stat_access_paths != 2 * index.num_clusters ||
                    cluster_oram.stat_real_block_indices != index.num_clusters) {
                throw std::runtime_error("ANN QuORAM padding did not preserve real blocks and fixed path count");
            }
            if (ring_oram.stat_bmasr_invocations != 1) {
                throw std::runtime_error("ANN DoubleORAM search should perform exactly one FuORAM access per query");
            }

            if (local.empty() || oram.empty()) {
                print_heap("local heap", local);
                print_heap("oram heap", oram);
                throw std::runtime_error("ANN DoubleORAM search returned an empty result unexpectedly");
            }
        }

        // Cluster 0 starts with 16 live entries in its only 32-vector block.
        // Insert 17 records so ids 64..79 fill that block and id 80 creates a
        // second QuORAM approximate block. The index itself stays static;
        // this validates the ORAM insertion protocol and persistence only.
        constexpr int kFirstInsertedId = static_cast<int>(kVectors);
        constexpr int kInsertedCount = 17;
        std::vector<uint8_t> factor_bytes(sizeof(Index::Factor));
        std::memcpy(factor_bytes.data(), &index.factors[index.start[0]], factor_bytes.size());
        std::vector<uint8_t> binary_code_bytes(kB / 8);
        std::memcpy(
            binary_code_bytes.data(),
            index.binary_code.data() + static_cast<size_t>(index.start[0]) * (kB / 64),
            binary_code_bytes.size());
        const AnnQuantizedVectorPayload inserted_quantized{
            factor_bytes,
            binary_code_bytes,
        };

        cluster_oram.reset_query_access_stats();
        ring_oram.reset_query_access_stats();
        for (int i = 0; i < kInsertedCount; ++i) {
            const int vector_id = kFirstInsertedId + i;
            std::vector<uint8_t> exact_payload = exact_payloads[0];
            const uint32_t encoded_id = static_cast<uint32_t>(vector_id);
            std::memcpy(
                exact_payload.data() + ExactLayout::kVectorBytes,
                &encoded_id,
                sizeof(encoded_id));

            const DoubleORAMInsertResult inserted = double_oram.insert_ann_vector(
                0,
                {0, 1},
                vector_id,
                inserted_quantized,
                exact_payload,
                2);
            if (inserted.cluster_id != 0 || inserted.vector_slot < 0 ||
                    inserted.ring_leaf < 0 || inserted.ring_leaf >= ring_oram.getNumLeaves()) {
                throw std::runtime_error("ANN insertion returned an invalid placement");
            }
            if (i < 16 && inserted.cluster_block_id != 0) {
                throw std::runtime_error("ANN insertion should fill the existing final cluster block first");
            }
            if (i == 16 && (inserted.cluster_block_id != 1 || inserted.vector_slot != 0)) {
                throw std::runtime_error("ANN insertion did not append a new QuORAM block correctly");
            }
        }
        if (cluster_oram.cluster_block_counts[0] != 2) {
            throw std::runtime_error("ANN insertion did not update the cluster block count");
        }
        if (ring_oram.stat_bmasr_invocations != kInsertedCount ||
                ring_oram.stat_real_block_indices != 0 ||
                ring_oram.stat_pad_slots != 2 * kInsertedCount) {
            throw std::runtime_error("ANN insertion did not use the expected FuORAM dummy-access shape");
        }

        // Flush enough fixed dummy paths to make the delayed FuORAM eviction
        // write all staged insert blocks when their paths intersect.
        ring_oram.access_without_position_map({}, {}, {}, kRingEvictRate);
        double_oram.evict_cluster();
        double_oram.evict_ring_and_clear_cache();

        const int inserted_id = kFirstInsertedId + kInsertedCount - 1;
        std::vector<AnnClusterApproxRead> inserted_cluster = double_oram.read_ann_clusters({0});
        bool found_inserted_id = false;
        const AnnApproxBlockRead* appended_block = nullptr;
        for (const AnnApproxBlockRead& block : inserted_cluster[0].blocks) {
            for (int id : block.vector_ids) {
                found_inserted_id = found_inserted_id || id == inserted_id;
            }
            if (block.block_id == 1) {
                appended_block = &block;
            }
        }
        if (!found_inserted_id || appended_block == nullptr) {
            throw std::runtime_error("inserted vector is missing after QuORAM writeback");
        }
        if (!std::equal(
                factor_bytes.begin(),
                factor_bytes.end(),
                appended_block->payload.begin())) {
            throw std::runtime_error("inserted ANN factor was not written into the new block");
        }
        std::array<uint64_t, kB / 64> inserted_code_words{};
        std::memcpy(
            inserted_code_words.data(),
            binary_code_bytes.data(),
            binary_code_bytes.size());
        std::vector<uint8_t> expected_fast_scan_codes(Index::kBlockVectors * kB / 8, 0);
        pack_codes_binary<kB>(inserted_code_words.data(), 1, expected_fast_scan_codes.data());
        if (!std::equal(
                expected_fast_scan_codes.begin(),
                expected_fast_scan_codes.end(),
                appended_block->payload.begin() + ApproxLayout::kFactorBytes)) {
            throw std::runtime_error("inserted ANN binary code has an invalid FastScan layout");
        }
        std::vector<RingVectorRead> inserted_read = double_oram.read_ring_vectors({inserted_id});
        if (inserted_read.size() != 1 ||
                unpack_bytes_from_ints(
                    inserted_read[0].data, 0, static_cast<int>(ExactLayout::kPlainBytes)) !=
                    [&]() {
                        std::vector<uint8_t> expected = exact_payloads[0];
                        const uint32_t encoded_id = static_cast<uint32_t>(inserted_id);
                        std::memcpy(
                            expected.data() + ExactLayout::kVectorBytes,
                            &encoded_id,
                            sizeof(encoded_id));
                        return expected;
                    }()) {
            throw std::runtime_error("inserted vector exact payload is missing after FuORAM writeback");
        }
        double_oram.update_path_stash_ring_leaves(inserted_read);
        double_oram.evict_cluster();
        double_oram.evict_ring_and_clear_cache();

        std::cout << "ANN DoubleORAM workflow test passed: "
                  << query.n << " queries, "
                  << index.num_clusters << " clusters, "
                  << index.n << " vectors, "
                  << "cluster_block_size " << cluster_block_size << ", "
                  << "ring_block_size " << ring_block_size << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_ann_double_oram_workflow failed: " << e.what() << "\n";
        return 1;
    } catch (const std::runtime_error* e) {
        std::cerr << "test_ann_double_oram_workflow failed: " << e->what() << "\n";
        delete e;
        return 1;
    }
}
