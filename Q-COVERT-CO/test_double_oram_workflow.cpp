#include "Bucket.h"
#include "QuORAM.h"
#include "DoubleORAM.h"
#include "RandomForOram.h"
#include "RemoteRing.h"
#include "FuORAM.h"
#include "evp.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <vector>

using namespace double_oram;

namespace {

constexpr int kNumClusters = 100;
constexpr int kVectorsPerCluster = 100;
constexpr int kVectorsPerQuantBlock = 20;
constexpr int kQuantizedCodeBytes = 4;
constexpr int kVectorDimension = 1;
constexpr int kPathsPerCluster = 2;
constexpr int kClusterCachedLevels = 4;
constexpr int kPathNumBlocks = kNumClusters * (kVectorsPerCluster / kVectorsPerQuantBlock);
constexpr int kRingRealBucketSize = 32;
constexpr int kRingDummySize = 48;
constexpr int kRingEvictRate = 48;
constexpr int kNumSearches = 10;
constexpr int kClustersPerSearch = 5;
constexpr int kRingReadsPerCluster = kRingDummySize + 16;

int quantized_block_size() {
    return 1 + kVectorsPerQuantBlock * (2 + kQuantizedCodeBytes);
}

int total_vectors() {
    return kNumClusters * kVectorsPerCluster;
}

int quant_blocks_per_cluster() {
    return kVectorsPerCluster / kVectorsPerQuantBlock;
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

std::vector<int> make_payload(int block_size, int value) {
    std::vector<int> data(block_size, value);
    data[0] = value;
    return data;
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

void encode_id(int vector_id, std::vector<int>& data, int offset) {
    for (int i = 0; i < kQuantizedCodeBytes; i++) {
        data[offset + i] = (vector_id >> (8 * i)) & 0xff;
    }
}

int decode_id(const std::vector<uint8_t>& code) {
    int value = 0;
    for (int i = 0; i < static_cast<int>(code.size()); i++) {
        value |= static_cast<int>(code[i]) << (8 * i);
    }
    return value;
}

std::vector<int> make_quantized_block_payload(
        int cluster_id,
        int quant_block_index,
        const std::vector<int>& latest_ring_leaf) {
    std::vector<int> data(quantized_block_size(), -1);
    data[0] = cluster_id;

    int first_vector = cluster_id * kVectorsPerCluster + quant_block_index * kVectorsPerQuantBlock;
    int entry_ints = 2 + kQuantizedCodeBytes;
    for (int slot = 0; slot < kVectorsPerQuantBlock; slot++) {
        int vector_id = first_vector + slot;
        int base = 1 + slot * entry_ints;
        data[base] = vector_id;
        data[base + 1] = latest_ring_leaf[vector_id];
        encode_id(vector_id, data, base + 2);
    }

    return data;
}

void place_cluster_block(
        QuORAM& cluster_oram,
        std::map<int, std::vector<Block*>>& buckets,
        std::vector<int>& used,
        int cluster_id,
        int quant_block_index,
        const std::vector<int>& latest_ring_leaf) {
    const int leaf = cluster_oram.cluster_leaf(cluster_id, 0, quant_block_index);
    std::vector<int> data = make_quantized_block_payload(cluster_id, quant_block_index, latest_ring_leaf);
    int block_index = cluster_id * quant_blocks_per_cluster() + quant_block_index;

    for (int level = cluster_oram.getNumLevels() - 1; level >= cluster_oram.num_cached_levels; level--) {
        int pos = cluster_oram.P(leaf, level);
        if (used[pos] < cluster_oram.real_bucket_size) {
            int slot = used[pos]++;
            delete buckets[pos][slot];
            buckets[pos][slot] = make_block(quantized_block_size(), block_index, data);
            cluster_oram.metadata[pos][slot].cluster_id = cluster_id;
            cluster_oram.metadata[pos][slot].cluster_offset = quant_block_index;
            cluster_oram.metadata[pos][slot].leaf = leaf;
            cluster_oram.metadata[pos][slot].valid = true;
            return;
        }
    }

    throw std::runtime_error("failed to place QuORAM quantized block");
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

void place_ring_blocks(
        FuORAM& ring_oram,
        std::map<int, std::vector<Block*>>& buckets,
        std::vector<int>& latest_ring_leaf,
        std::mt19937& rng) {
    std::uniform_int_distribution<int> leaf_dist(0, ring_oram.getNumLeaves() - 1);
    std::vector<int> used(ring_oram.getNumBuckets(), 0);

    for (int vector_id = 0; vector_id < total_vectors(); vector_id++) {
        bool placed = false;
        for (int attempt = 0; attempt < 100000 && !placed; attempt++) {
            int leaf = leaf_dist(rng);
            for (int level = ring_oram.getNumLevels() - 1; level >= 0; level--) {
                int pos = ring_oram.P(leaf, level);
                if (used[pos] < kRingRealBucketSize) {
                    int slot = used[pos]++;
                    delete buckets[pos][slot];
                    buckets[pos][slot] = make_block(
                        quantized_block_size(),
                        vector_id,
                        make_payload(quantized_block_size(), vector_id));
                    latest_ring_leaf[vector_id] = leaf;
                    placed = true;
                    break;
                }
            }
        }

        if (!placed) {
            throw std::runtime_error("failed to place FuORAM vector block");
        }
    }
}

std::vector<int> sample_unique(std::mt19937& rng, int begin, int count, int sample_size) {
    std::vector<int> values(count);
    std::iota(values.begin(), values.end(), begin);
    std::shuffle(values.begin(), values.end(), rng);
    values.resize(sample_size);
    return values;
}

void verify_cluster_read(
        int cluster_id,
        const std::vector<ClusterQuantizedRead>& reads,
        const std::vector<int>& latest_ring_leaf) {
    if (reads.size() != 1 || reads[0].cluster_id != cluster_id) {
        throw std::runtime_error("unexpected QuORAM cluster read shape");
    }

    std::unordered_map<int, QuantizedVectorEntry> entries;
    for (const QuantizedVectorEntry& entry : reads[0].vectors) {
        entries[entry.vector_id] = entry;
    }

    if (entries.size() != kVectorsPerCluster) {
        throw std::runtime_error("cluster read did not return exactly 100 vectors");
    }

    int first_vector = cluster_id * kVectorsPerCluster;
    for (int offset = 0; offset < kVectorsPerCluster; offset++) {
        int vector_id = first_vector + offset;
        auto it = entries.find(vector_id);
        if (it == entries.end()) {
            throw std::runtime_error("cluster read missed a vector id");
        }
        if (it->second.ring_leaf != latest_ring_leaf[vector_id]) {
            throw std::runtime_error("QuORAM returned a stale FuORAM leaf");
        }
        if (decode_id(it->second.quantized_code) != vector_id) {
            throw std::runtime_error("quantized code did not match vector id");
        }
    }
}

void verify_ring_reads(
        const std::vector<int>& vector_ids,
        const std::vector<RingVectorRead>& reads,
        const std::vector<int>& latest_ring_leaf) {
    if (reads.size() != vector_ids.size()) {
        throw std::runtime_error("unexpected FuORAM read count");
    }

    for (size_t i = 0; i < reads.size(); i++) {
        const RingVectorRead& read = reads[i];
        if (read.vector_id != vector_ids[i]) {
            throw std::runtime_error("FuORAM read returned vectors out of order");
        }
        if (read.old_leaf != latest_ring_leaf[read.vector_id]) {
            throw std::runtime_error("FuORAM read used a stale old leaf");
        }
        if (read.data.empty() || read.data[0] != read.vector_id) {
            throw std::runtime_error("FuORAM high precision data did not match vector id");
        }
    }
}

void verify_direct_ring_reads(
        const std::vector<int>& vector_ids,
        const std::vector<int>& old_leaves,
        const std::vector<int>& new_leaves,
        const std::vector<FuORAMVectorRead>& reads) {
    if (reads.size() != vector_ids.size()) {
        throw std::runtime_error("unexpected direct FuORAM read count");
    }

    for (size_t i = 0; i < reads.size(); i++) {
        const FuORAMVectorRead& read = reads[i];
        if (read.vector_id != vector_ids[i]) {
            throw std::runtime_error("direct FuORAM read returned vectors out of order");
        }
        if (read.old_leaf != old_leaves[i] || read.new_leaf != new_leaves[i]) {
            throw std::runtime_error("direct FuORAM read returned wrong leaf metadata");
        }
        if (read.data.empty() || read.data[0] != read.vector_id) {
            throw std::runtime_error("direct FuORAM data did not match vector id");
        }
    }
}

} // namespace

int main() {
    try {
        const int block_size = quantized_block_size();
        const int ring_num_levels = choose_ring_levels_with_load_slack(total_vectors(), kRingRealBucketSize);

        Bucket::resetState();
        SBucket::resetState();

        std::vector<int> cluster_block_counts(kNumClusters, quant_blocks_per_cluster());
        const int cluster_num_levels = choose_cluster_levels(kPathNumBlocks, kRingRealBucketSize);
        const int cluster_cached_levels = std::min(kClusterCachedLevels, std::max(0, cluster_num_levels - 2));
        RingOramConfig cluster_config(
            block_size,
            kRingRealBucketSize,
            kRingDummySize,
            kRingEvictRate,
            kPathNumBlocks,
            cluster_num_levels,
            cluster_cached_levels);
        RingOramConfig ring_config(
            block_size,
            kRingRealBucketSize,
            kRingDummySize,
            kRingEvictRate,
            total_vectors(),
            ring_num_levels,
            0);

        RandomForOram random;
        RemoteRing cluster_server(nullptr, cluster_config, true, true, false);
        RemoteRing cluster_client(&cluster_server, cluster_config, false);
        QuORAM cluster_oram(&cluster_client, &random, cluster_config, cluster_num_levels, cluster_block_counts);

        RemoteRing ring_server(nullptr, ring_config, true, true, false);
        RemoteRing ring_client(&ring_server, ring_config, false);
        FuORAM ring_oram(&ring_client, &random, ring_config, ring_num_levels, 0, true, true);

        std::mt19937 init_rng(20260608);
        std::vector<int> latest_ring_leaf(total_vectors(), -1);
        std::map<int, std::vector<Block*>> ring_buckets =
            make_dummy_buckets(ring_config.num_buckets, ring_config.bucket_size, block_size);
        place_ring_blocks(ring_oram, ring_buckets, latest_ring_leaf, init_rng);
        install_ring_metadata(ring_oram, ring_buckets, latest_ring_leaf);
        ring_client.WriteBucketBatchMapAsBlockRing(ring_buckets, ring_config.bucket_size, false);

        {
            NetworkProfile test_network;
            test_network.enabled = true;
            test_network.sleep = false;
            test_network.bandwidth_mbps = 3000.0;
            test_network.rtt_ms = 1.0;
            ring_oram.set_network_profile(test_network);
            ring_oram.reset_query_access_stats();

            const int oversized_batch = kRingDummySize + 16;
            std::vector<int> vector_ids(oversized_batch);
            std::iota(vector_ids.begin(), vector_ids.end(), 0);

            std::vector<int> old_leaves;
            std::vector<int> new_leaves;
            old_leaves.reserve(vector_ids.size());
            new_leaves.reserve(vector_ids.size());
            std::uniform_int_distribution<int> leaf_dist(0, ring_oram.getNumLeaves() - 1);
            for (int vector_id : vector_ids) {
                old_leaves.push_back(latest_ring_leaf[vector_id]);
                new_leaves.push_back(leaf_dist(init_rng));
            }

            std::vector<FuORAMVectorRead> direct_reads =
                ring_oram.access_without_position_map(vector_ids, old_leaves, new_leaves, oversized_batch);
            verify_direct_ring_reads(vector_ids, old_leaves, new_leaves, direct_reads);
            if (ring_oram.stat_bmasr_invocations != 1) {
                throw std::runtime_error("oversized FuORAM batch changed the external access count");
            }
            if (ring_oram.stat_net_round_trips != 2) {
                throw std::runtime_error("oversized FuORAM batch should use one read RTT and one reshuffle write RTT");
            }

            for (size_t i = 0; i < vector_ids.size(); i++) {
                latest_ring_leaf[vector_ids[i]] = new_leaves[i];
            }
            ring_oram.set_network_profile(NetworkProfile());
            ring_oram.reset_query_access_stats();
        }

        std::map<int, std::vector<Block*>> cluster_buckets =
            make_dummy_buckets(cluster_config.num_buckets, cluster_config.bucket_size, block_size);
        std::vector<int> cluster_used(cluster_config.num_buckets, 0);
        for (int cluster_id = 0; cluster_id < kNumClusters; cluster_id++) {
            for (int quant_block = 0; quant_block < quant_blocks_per_cluster(); quant_block++) {
                place_cluster_block(cluster_oram, cluster_buckets, cluster_used, cluster_id, quant_block, latest_ring_leaf);
            }
        }
        cluster_client.WriteBucketBatchMapAsBlockRing(cluster_buckets, cluster_config.bucket_size, false);

        DoubleORAMConfig config(
            kNumClusters,
            kQuantizedCodeBytes,
            kVectorDimension,
            kVectorsPerQuantBlock,
            kPathsPerCluster,
            0);
        DoubleORAM double_oram(cluster_oram, ring_oram, config);

        std::mt19937 search_rng(20260609);
        std::set<int> touched_clusters;
        for (int search = 0; search < kNumSearches; search++) {
            std::vector<int> clusters = sample_unique(search_rng, 0, kNumClusters, kClustersPerSearch);
            for (int cluster_id : clusters) {
                touched_clusters.insert(cluster_id);

                std::vector<ClusterQuantizedRead> cluster_reads = double_oram.read_clusters({cluster_id});
                verify_cluster_read(cluster_id, cluster_reads, latest_ring_leaf);

                int first_vector = cluster_id * kVectorsPerCluster;
                std::vector<int> vector_ids =
                    sample_unique(search_rng, first_vector, kVectorsPerCluster, kRingReadsPerCluster);

                uint64_t ring_accesses_before = ring_oram.stat_bmasr_invocations;
                std::vector<RingVectorRead> ring_reads = double_oram.read_ring_vectors(vector_ids);
                if (ring_oram.stat_bmasr_invocations != ring_accesses_before + 1) {
                    throw std::runtime_error("DoubleORAM split one FuORAM vector read batch into multiple accesses");
                }
                verify_ring_reads(vector_ids, ring_reads, latest_ring_leaf);

                for (const RingVectorRead& read : ring_reads) {
                    latest_ring_leaf[read.vector_id] = read.new_leaf;
                }
                double_oram.update_path_stash_ring_leaves(ring_reads);
            }

            double_oram.evict_cluster();
            double_oram.evict_ring_and_clear_cache();
        }

        for (int cluster_id : touched_clusters) {
            std::vector<ClusterQuantizedRead> cluster_reads = double_oram.read_clusters({cluster_id});
            verify_cluster_read(cluster_id, cluster_reads, latest_ring_leaf);
        }
        double_oram.evict_cluster();

        std::cout << "DoubleORAM workflow test passed: "
                  << kNumSearches << " searches, "
                  << touched_clusters.size() << " touched clusters, "
                  << total_vectors() << " vectors\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "test_double_oram_workflow failed: " << e.what() << "\n";
        return 1;
    } catch (const std::runtime_error* e) {
        std::cerr << "test_double_oram_workflow failed: " << e->what() << "\n";
        delete e;
        return 1;
    }
}
