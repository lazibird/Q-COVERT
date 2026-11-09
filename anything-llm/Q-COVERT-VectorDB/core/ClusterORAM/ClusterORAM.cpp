#include "ClusterORAM.h"
#include "evp.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iterator>
#include <random>
#include <stdexcept>
#include <numeric>

namespace double_oram {

namespace {

constexpr uint64_t kClusterOramPrfSeed = 0x9f4a7c15d2b8e681ULL;

using hires_clock = std::chrono::high_resolution_clock;

double elapsed_sec_since(hires_clock::time_point t0) {
    return std::chrono::duration<double>(hires_clock::now() - t0).count();
}

uint64_t ring_cipher_block_bytes(int block_size) {
    return static_cast<uint64_t>(compute_ctx_len((1 + block_size) * sizeof(int)));
}

uint64_t block_read_comm_bytes(size_t block_count, uint64_t cipher_block_bytes) {
    return static_cast<uint64_t>(sizeof(int) + sizeof(size_t) +
                                 block_count * 2 * sizeof(int) +
                                 block_count * cipher_block_bytes);
}

uint64_t xor_read_comm_bytes(
        size_t block_count,
        size_t real_block_count,
        uint64_t cipher_block_bytes) {
    return static_cast<uint64_t>(sizeof(int) + 2 * sizeof(size_t) +
                                 block_count * 2 * sizeof(int) +
                                 real_block_count * (cipher_block_bytes - 16) +
                                 block_count * 16);
}

uint64_t bucket_write_comm_bytes(size_t bucket_count, size_t bucket_size, uint64_t cipher_block_bytes) {
    return static_cast<uint64_t>(sizeof(int) + sizeof(size_t) +
                                 bucket_count * sizeof(int) +
                                 bucket_count * bucket_size * cipher_block_bytes);
}

} // namespace

ClusterORAM::ClusterORAM(
        RemoteRing* storage,
        RandForOramInterface* rand_gen,
        const RingOramConfig& config,
        int num_levels,
        const std::vector<int>& cluster_block_counts)
    : storage(storage),
      rand_gen(rand_gen),
      block_size(config.block_size),
      real_bucket_size(config.real_bucket_size),
      dummy_size(config.dummy_size),
      bucket_size(config.real_bucket_size + config.dummy_size),
      evict_rate(config.evict_rate),
      num_levels(num_levels),
      cache_top(false),
      num_cached_levels(0),
      num_cached_bucket(0),
      cluster_block_counts(cluster_block_counts),
      G(1),
      cnt_delayed_leaves(0),
      metadata_remote_(false) {
    if (cluster_block_counts.empty()) {
        throw std::invalid_argument("ClusterORAM needs at least one cluster");
    }
    if (num_levels <= 1) {
        throw std::invalid_argument("ClusterORAM num_levels must be greater than 1");
    }

    num_blocks = 0;
    for (int count : cluster_block_counts) {
        if (count < 0) {
            throw std::invalid_argument("cluster block count must be non-negative");
        }
        num_blocks += count;
    }

    num_buckets = static_cast<int>(std::pow(2, num_levels)) - 1;
    num_leaves = static_cast<int>(std::pow(2, num_levels - 1));
    num_cached_levels = std::min(std::max(0, config.cached_levels), num_levels - 1);
    cache_top = num_cached_levels > 0;
    num_cached_bucket = cache_top ? static_cast<int>(std::pow(2, num_cached_levels)) - 1 : 0;
    if (static_cast<long long>(num_buckets - num_cached_bucket) * real_bucket_size < num_blocks) {
        throw std::runtime_error("ClusterORAM tree does not have enough real block capacity");
    }

    if (rand_gen != nullptr) {
        rand_gen->setBound(num_leaves);
    }
    cluster_counters.assign(cluster_block_counts.size(), 0);
    metadata.assign(
        num_buckets,
        std::vector<ClusterORAMSlotMeta>(static_cast<size_t>(bucket_size)));
    bucket_read_counts.assign(num_buckets, 0);
    cluster_stash_indices.resize(cluster_block_counts.size());
    for (size_t cluster_id = 0; cluster_id < cluster_block_counts.size(); cluster_id++) {
        cluster_stash_indices[cluster_id].assign(cluster_block_counts[cluster_id], -1);
    }
    reset_query_access_stats();
}

void ClusterORAM::reset_query_access_stats() {
    stat_access_invocations = 0;
    stat_real_block_indices = 0;
    stat_server_cipher_block_reads = 0;
    stat_comm_bytes = 0;
    stat_evict_invocations = 0;
    stat_evict_effective = 0;
    stat_access_time_sec = 0.0;
    stat_evict_time_sec = 0.0;
    stat_evict_effective_time_sec = 0.0;
    stat_max_stash_blocks_after_evict = 0;
    stat_net_round_trips = 0;
    stat_access_net_round_trips = 0;
    stat_evict_net_round_trips = 0;
    stat_evict_write_net_round_trips = 0;
    stat_net_transfer_time_sec = 0.0;
    stat_net_latency_time_sec = 0.0;
    stat_net_total_time_sec = 0.0;
}

void ClusterORAM::set_network_profile(const NetworkProfile& profile) {
    network_profile_ = profile;
}

void ClusterORAM::set_metadata_remote(bool remote) {
    metadata_remote_ = remote;
}

bool ClusterORAM::metadata_stored_on_server() const {
    return metadata_remote_;
}

void ClusterORAM::account_network_transfer(
        uint64_t bytes,
        uint64_t round_trips,
        bool evict_phase,
        bool evict_write) {
    const NetworkSimulationResult result =
        simulate_network_transfer(network_profile_, bytes, round_trips);
    stat_net_round_trips += result.round_trips;
    if (evict_phase) {
        stat_evict_net_round_trips += result.round_trips;
        if (evict_write) {
            stat_evict_write_net_round_trips += result.round_trips;
        }
    } else {
        stat_access_net_round_trips += result.round_trips;
    }
    stat_net_transfer_time_sec += result.transfer_time_sec;
    stat_net_latency_time_sec += result.latency_time_sec;
    stat_net_total_time_sec += result.total_time_sec;
}

uint64_t ClusterORAM::metadata_bucket_bytes() const {
    const uint64_t slot_bytes =
        static_cast<uint64_t>(3 * sizeof(int) + sizeof(bool));
    return static_cast<uint64_t>(bucket_size) * slot_bytes + sizeof(int);
}

void ClusterORAM::account_remote_metadata_transfer(size_t bucket_count, bool evict_phase) {
    if (!metadata_remote_ || bucket_count == 0) {
        return;
    }
    const uint64_t bytes =
        static_cast<uint64_t>(bucket_count) * metadata_bucket_bytes();
    stat_comm_bytes += bytes;
    account_network_transfer(bytes, 1, evict_phase);
}

uint64_t ClusterORAM::early_reshuffle(const std::vector<int>& buckets, bool account_read) {
    if (buckets.empty()) {
        return 0;
    }

    std::vector<int> unique_buckets;
    unique_buckets.reserve(buckets.size());
    std::set<int> seen_buckets;
    for (int pos : buckets) {
        if (pos < 0 || pos >= num_buckets) {
            throw std::out_of_range("ClusterORAM early reshuffle bucket is outside range");
        }
        if (seen_buckets.insert(pos).second) {
            unique_buckets.push_back(pos);
        }
    }
    account_remote_metadata_transfer(unique_buckets.size());

    std::vector<int> positions;
    std::vector<int> offsets;
    std::vector<bool> valids;
    std::vector<std::pair<int, int>> selected_identities;
    std::vector<int> selected_leaves;
    positions.reserve(unique_buckets.size() * static_cast<size_t>(real_bucket_size));
    offsets.reserve(unique_buckets.size() * static_cast<size_t>(real_bucket_size));
    valids.reserve(unique_buckets.size() * static_cast<size_t>(real_bucket_size));
    selected_identities.reserve(unique_buckets.size() * static_cast<size_t>(real_bucket_size));
    selected_leaves.reserve(unique_buckets.size() * static_cast<size_t>(real_bucket_size));

    for (int pos : unique_buckets) {
        int selected = 0;
        for (int slot = 0; slot < bucket_size && selected < real_bucket_size; slot++) {
            const ClusterORAMSlotMeta& meta = metadata[pos][slot];
            if (meta.cluster_id == -1 || !meta.valid) {
                continue;
            }
            if (meta.leaf < 0 || meta.leaf >= num_leaves) {
                throw std::runtime_error("ClusterORAM metadata is missing a valid block leaf");
            }
            positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            selected_identities.push_back({meta.cluster_id, meta.cluster_offset});
            selected_leaves.push_back(meta.leaf);
            selected++;
        }
        for (int slot = 0; slot < bucket_size && selected < real_bucket_size; slot++) {
            const ClusterORAMSlotMeta& meta = metadata[pos][slot];
            if (meta.cluster_id != -1 || !meta.valid) {
                continue;
            }
            positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            selected_identities.push_back({-1, -1});
            selected_leaves.push_back(-1);
            selected++;
        }
        if (selected != real_bucket_size) {
            throw std::runtime_error("ClusterORAM early reshuffle cannot find enough readable slots");
        }
    }

    const uint64_t cipher_block_bytes = ring_cipher_block_bytes(block_size);
    std::vector<Block*> blocks;
    const uint64_t read_comm_bytes =
        block_read_comm_bytes(positions.size(), cipher_block_bytes);
    if (account_read) {
        stat_comm_bytes += read_comm_bytes;
        account_network_transfer(read_comm_bytes);
    }
    storage->ReadBlockBatchAsBlockRing(positions, offsets, blocks, valids, true);
    stat_server_cipher_block_reads += static_cast<uint64_t>(positions.size());

    std::random_device rd;
    std::mt19937 rng(rd());
    std::map<int, std::vector<Block*>> reshuffled_storage;
    for (size_t bucket_index = 0; bucket_index < unique_buckets.size(); bucket_index++) {
        const int pos = unique_buckets[bucket_index];
        std::vector<Block*> bucket;
        std::vector<std::pair<int, int>> identities;
        std::vector<int> leaves;
        bucket.reserve(static_cast<size_t>(bucket_size));
        identities.reserve(static_cast<size_t>(bucket_size));
        leaves.reserve(static_cast<size_t>(bucket_size));

        const size_t base = bucket_index * static_cast<size_t>(real_bucket_size);
        for (int i = 0; i < real_bucket_size; i++) {
            const size_t idx = base + static_cast<size_t>(i);
            Block* block = blocks[idx];
            const std::pair<int, int> identity = selected_identities[idx];
            if (identity.first == -1 || block->index == -1) {
                delete block;
                bucket.push_back(new Block(block_size));
                identities.push_back({-1, -1});
                leaves.push_back(-1);
            } else {
                bucket.push_back(block);
                identities.push_back(identity);
                leaves.push_back(selected_leaves[idx]);
            }
        }

        while (static_cast<int>(bucket.size()) < bucket_size) {
            bucket.push_back(new Block(block_size));
            identities.push_back({-1, -1});
            leaves.push_back(-1);
        }

        std::vector<int> order(bucket.size());
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), rng);
        std::vector<Block*> shuffled_bucket;
        std::vector<std::pair<int, int>> shuffled_identities;
        std::vector<int> shuffled_leaves;
        shuffled_bucket.reserve(bucket.size());
        shuffled_identities.reserve(bucket.size());
        shuffled_leaves.reserve(bucket.size());
        for (int index : order) {
            shuffled_bucket.push_back(bucket[index]);
            shuffled_identities.push_back(identities[index]);
            shuffled_leaves.push_back(leaves[index]);
        }
        bucket.swap(shuffled_bucket);
        identities.swap(shuffled_identities);
        leaves.swap(shuffled_leaves);

        for (int slot = 0; slot < bucket_size; slot++) {
            if (identities[slot].first == -1) {
                metadata[pos][slot] = ClusterORAMSlotMeta{};
            } else {
                metadata[pos][slot].cluster_id = identities[slot].first;
                metadata[pos][slot].cluster_offset = identities[slot].second;
                metadata[pos][slot].leaf = leaves[slot];
                metadata[pos][slot].valid = true;
            }
        }
        bucket_read_counts[pos] = 0;
        reshuffled_storage[pos] = std::move(bucket);
    }

    const uint64_t write_comm_bytes = bucket_write_comm_bytes(
        reshuffled_storage.size(),
        static_cast<size_t>(bucket_size),
        cipher_block_bytes);
    stat_comm_bytes += write_comm_bytes;
    account_network_transfer(write_comm_bytes);
    storage->WriteBucketBatchMapAsBlockRing(reshuffled_storage, bucket_size, true);
    return read_comm_bytes;
}

int ClusterORAM::cluster_leaf(int cluster_id, uint64_t counter, int cluster_offset) const {
    validate_cluster_block(cluster_id, cluster_offset);
    return double_oram_prf_leaf(
        kClusterOramPrfSeed,
        cluster_id,
        counter,
        cluster_offset,
        0,
        num_leaves);
}

std::vector<int> ClusterORAM::cluster_leaves(int cluster_id, uint64_t counter) const {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(cluster_block_counts.size())) {
        throw std::out_of_range("cluster_id is outside ClusterORAM range");
    }

    std::vector<int> leaves;
    leaves.reserve(cluster_block_counts[cluster_id]);
    for (int cluster_offset = 0; cluster_offset < cluster_block_counts[cluster_id]; cluster_offset++) {
        leaves.push_back(cluster_leaf(cluster_id, counter, cluster_offset));
    }
    return leaves;
}

std::vector<ClusterORAMClusterRead> ClusterORAM::read_clusters_for_double_oram(
        const std::vector<int>& cluster_ids) {
    if (cluster_ids.empty()) {
        return {};
    }

    const hires_clock::time_point t0 = hires_clock::now();
    const double net_time0 = stat_net_total_time_sec;
    stat_access_invocations++;

    std::set<int> requested_clusters;
    std::vector<ClusterORAMClusterRead> reads;
    reads.reserve(cluster_ids.size());

    struct RequestedBlock {
        int cluster_id;
        int cluster_offset;
        int new_leaf;
        std::vector<int> path_positions;
    };
    struct ReadLabel {
        bool trigger_bucket;
        int bucket_pos;
        std::pair<int, int> identity;
        int leaf;
        int new_leaf;
    };
    struct BucketBlock {
        Block* block;
        std::pair<int, int> identity;
        int leaf;
    };

    std::vector<RequestedBlock> requested_blocks;
    std::set<std::pair<int, int>> requested_block_set;
    std::map<std::pair<int, int>, int> requested_new_leaf;
    std::set<std::pair<int, int>> found_blocks;
    const uint64_t cipher_block_bytes = ring_cipher_block_bytes(block_size);
    std::map<int, int> access_counts_by_bucket;
    std::set<int> reshuffle_buckets;

    for (int cluster_id : cluster_ids) {
        if (cluster_id < 0 || cluster_id >= static_cast<int>(cluster_block_counts.size())) {
            throw std::out_of_range("cluster_id is outside ClusterORAM range");
        }
        if (!requested_clusters.insert(cluster_id).second) {
            throw std::invalid_argument("duplicate cluster_id in one ClusterORAM access batch");
        }

        ClusterORAMClusterRead read;
        read.cluster_id = cluster_id;
        reads.push_back(read);

        const uint64_t old_counter = cluster_counters[cluster_id];
        const uint64_t new_counter = old_counter + 1;
        const std::vector<int> old_leaves = cluster_leaves(cluster_id, old_counter);
        const std::vector<int> new_leaves = cluster_leaves(cluster_id, new_counter);
        for (int cluster_offset = 0; cluster_offset < cluster_block_counts[cluster_id]; cluster_offset++) {
            const int leaf = old_leaves[cluster_offset];
            RequestedBlock requested;
            requested.cluster_id = cluster_id;
            requested.cluster_offset = cluster_offset;
            requested.new_leaf = new_leaves[cluster_offset];
            requested.path_positions.reserve(static_cast<size_t>(num_levels - num_cached_levels));

            for (int level = num_levels - 1; level >= num_cached_levels; level--) {
                const int pos = P(leaf, level);
                if (pos < 0 || pos >= num_buckets) {
                    throw std::out_of_range("ClusterORAM bucket position is outside range");
                }
                requested.path_positions.push_back(pos);
                access_counts_by_bucket[pos]++;
            }
            requested_block_set.insert({cluster_id, cluster_offset});
            requested_new_leaf[{cluster_id, cluster_offset}] = new_leaves[cluster_offset];
            requested_blocks.push_back(std::move(requested));
        }
        cluster_counters[cluster_id] = new_counter;
    }

    cnt_delayed_leaves += static_cast<int>(requested_blocks.size());
    stat_real_block_indices += static_cast<uint64_t>(requested_blocks.size());

    for (const auto& count_pair : access_counts_by_bucket) {
        const int pos = count_pair.first;
        const int count = count_pair.second;
        if (bucket_read_counts[pos] + count > dummy_size) {
            reshuffle_buckets.insert(pos);
        }
    }
    account_remote_metadata_transfer(access_counts_by_bucket.size());
    for (const auto& count_pair : access_counts_by_bucket) {
        bucket_read_counts[count_pair.first] += count_pair.second;
    }

    std::vector<int> positions;
    std::vector<int> offsets;
    std::vector<bool> valids;
    std::vector<ReadLabel> read_labels;
    std::map<int, std::vector<BucketBlock>> reshuffle_bucket_blocks;

    for (int pos : reshuffle_buckets) {
        std::vector<bool> selected_slots(static_cast<size_t>(bucket_size), false);
        int selected = 0;
        for (int slot = 0; slot < bucket_size && selected < real_bucket_size; slot++) {
            const ClusterORAMSlotMeta& meta = metadata[pos][slot];
            if (meta.cluster_id == -1 || !meta.valid) {
                continue;
            }
            positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            read_labels.push_back({true, pos, {meta.cluster_id, meta.cluster_offset}, meta.leaf, -1});
            selected_slots[slot] = true;
            selected++;
        }
        if (selected > real_bucket_size) {
            throw std::runtime_error("ClusterORAM bucket has more valid real blocks than Z");
        }
        for (int slot = 0; slot < bucket_size && selected < real_bucket_size; slot++) {
            if (selected_slots[slot]) {
                continue;
            }
            positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            read_labels.push_back({true, pos, {-1, -1}, -1, -1});
            selected_slots[slot] = true;
            selected++;
        }
        if (selected != real_bucket_size) {
            throw std::runtime_error("ClusterORAM early reshuffle cannot select fixed Z slots");
        }
        reshuffle_bucket_blocks[pos] = {};
    }

    uint64_t normal_positions = 0;
    uint64_t normal_groups = 0;
    for (const RequestedBlock& requested : requested_blocks) {
        bool selected_any_normal_slot = false;
        for (int pos : requested.path_positions) {
            if (reshuffle_buckets.find(pos) != reshuffle_buckets.end()) {
                continue;
            }
            const int slot = find_slot(pos, requested.cluster_id, requested.cluster_offset);
            const bool real_slot = slot >= 0;
            const int selected_slot = real_slot ? slot : find_dummy_slot(pos);
            if (selected_slot < 0) {
                throw std::runtime_error("ClusterORAM bucket has no readable dummy slot");
            }

            positions.push_back(pos);
            offsets.push_back(selected_slot);
            valids.push_back(true);
            read_labels.push_back({
                false,
                pos,
                real_slot ? std::make_pair(requested.cluster_id, requested.cluster_offset) : std::make_pair(-1, -1),
                -1,
                real_slot ? requested.new_leaf : -1});
            metadata[pos][selected_slot].valid = false;
            selected_any_normal_slot = true;
            normal_positions++;
        }
        if (selected_any_normal_slot) {
            normal_groups++;
        }
    }

    auto add_or_update_stash = [&](int cluster_id, int cluster_offset, int leaf, Block* block) {
        const int stash_index = find_stash_index(cluster_id, cluster_offset);
        if (stash_index >= 0) {
            auto stash_it = cluster_stash.find(cluster_id);
            if (stash_it == cluster_stash.end() ||
                    stash_index >= static_cast<int>(stash_it->second.size())) {
                delete block;
                throw std::runtime_error("ClusterORAM stash index is inconsistent");
            }
            stash_it->second[stash_index].leaf = leaf;
            delete block;
        } else {
            add_to_stash(cluster_id, cluster_offset, leaf, block);
        }
        found_blocks.insert({cluster_id, cluster_offset});
    };

    const uint64_t reshuffle_read_bytes =
        reshuffle_buckets.empty()
            ? 0
            : block_read_comm_bytes(
                reshuffle_buckets.size() * static_cast<size_t>(real_bucket_size),
                cipher_block_bytes);
    const uint64_t normal_read_bytes =
        normal_groups == 0
            ? 0
            : xor_read_comm_bytes(
                static_cast<size_t>(normal_positions),
                static_cast<size_t>(normal_groups),
                cipher_block_bytes);
    if (!positions.empty()) {
        std::vector<Block*> blocks;
        const uint64_t read_comm_bytes = reshuffle_read_bytes + normal_read_bytes;
        stat_comm_bytes += read_comm_bytes;
        account_network_transfer(read_comm_bytes);
        storage->ReadBlockBatchAsBlockRing(positions, offsets, blocks, valids, false);
        stat_server_cipher_block_reads += static_cast<uint64_t>(positions.size());

        for (size_t i = 0; i < blocks.size(); i++) {
            Block* block = blocks[i];
            const ReadLabel& label = read_labels[i];
            if (block->index == -1) {
                delete block;
                continue;
            }
            if (label.identity.first == -1) {
                delete block;
                continue;
            }
            validate_cluster_block(label.identity.first, label.identity.second);
            if (requested_block_set.find(label.identity) != requested_block_set.end()) {
                add_or_update_stash(
                    label.identity.first,
                    label.identity.second,
                    requested_new_leaf[label.identity],
                    block);
            } else if (label.trigger_bucket) {
                reshuffle_bucket_blocks[label.bucket_pos].push_back(
                    {block, label.identity, label.leaf});
            } else {
                delete block;
                throw std::runtime_error("ClusterORAM normal read returned an unrequested real block");
            }
        }
    }

    for (const RequestedBlock& requested : requested_blocks) {
        const std::pair<int, int> identity = {requested.cluster_id, requested.cluster_offset};
        if (found_blocks.find(identity) != found_blocks.end()) {
            continue;
        }

        const int stash_index = find_stash_index(requested.cluster_id, requested.cluster_offset);
        if (stash_index < 0) {
            throw std::runtime_error("ClusterORAM target block was not found on path or in stash");
        }
        cluster_stash[requested.cluster_id][stash_index].leaf = requested.new_leaf;
        found_blocks.insert(identity);
    }

    if (!reshuffle_buckets.empty()) {
        std::random_device rd;
        std::mt19937 rng(rd());
        std::map<int, std::vector<Block*>> reshuffled_storage;
        std::map<int, std::vector<std::pair<int, int>>> reshuffled_identities;
        std::map<int, std::vector<int>> reshuffled_leaves;
        for (int pos : reshuffle_buckets) {
            std::vector<Block*>& bucket = reshuffled_storage[pos];
            std::vector<std::pair<int, int>>& identities = reshuffled_identities[pos];
            std::vector<int>& leaves = reshuffled_leaves[pos];
            for (BucketBlock& bucket_block : reshuffle_bucket_blocks[pos]) {
                bucket.push_back(bucket_block.block);
                identities.push_back(bucket_block.identity);
                leaves.push_back(bucket_block.leaf);
            }
            if (static_cast<int>(bucket.size()) > real_bucket_size) {
                throw std::runtime_error("ClusterORAM reshuffle bucket has more than Z real blocks");
            }
            while (static_cast<int>(bucket.size()) < bucket_size) {
                bucket.push_back(new Block(block_size));
                identities.push_back({-1, -1});
                leaves.push_back(-1);
            }

            std::vector<int> order(bucket.size());
            std::iota(order.begin(), order.end(), 0);
            std::shuffle(order.begin(), order.end(), rng);
            std::vector<Block*> shuffled_bucket;
            std::vector<std::pair<int, int>> shuffled_identities;
            std::vector<int> shuffled_leaves;
            shuffled_bucket.reserve(bucket.size());
            shuffled_identities.reserve(bucket.size());
            shuffled_leaves.reserve(bucket.size());
            for (int index : order) {
                shuffled_bucket.push_back(bucket[index]);
                shuffled_identities.push_back(identities[index]);
                shuffled_leaves.push_back(leaves[index]);
            }
            bucket.swap(shuffled_bucket);
            identities.swap(shuffled_identities);
            leaves.swap(shuffled_leaves);

            for (int slot = 0; slot < bucket_size; slot++) {
                if (identities[slot].first == -1) {
                    metadata[pos][slot] = ClusterORAMSlotMeta{};
                } else {
                    metadata[pos][slot].cluster_id = identities[slot].first;
                    metadata[pos][slot].cluster_offset = identities[slot].second;
                    metadata[pos][slot].leaf = leaves[slot];
                    metadata[pos][slot].valid = true;
                }
            }
            bucket_read_counts[pos] = 0;
        }

        const uint64_t write_comm_bytes = bucket_write_comm_bytes(
            reshuffled_storage.size(),
            static_cast<size_t>(bucket_size),
            cipher_block_bytes);
        stat_comm_bytes += write_comm_bytes;
        account_network_transfer(write_comm_bytes);
        storage->WriteBucketBatchMapAsBlockRing(reshuffled_storage, bucket_size, true);
    }

    for (ClusterORAMClusterRead& read : reads) {
        read.stash_block_indices.clear();
        read.stash_block_indices.reserve(cluster_block_counts[read.cluster_id]);
        for (int cluster_offset = 0; cluster_offset < cluster_block_counts[read.cluster_id]; cluster_offset++) {
            const int stash_index = find_stash_index(read.cluster_id, cluster_offset);
            if (stash_index < 0) {
                throw std::runtime_error("ClusterORAM requested block is missing from stash index");
            }
            read.stash_block_indices.push_back(stash_index);
        }
    }

    double elapsed_sec = elapsed_sec_since(t0);
    if (network_profile_.enabled && !network_profile_.sleep) {
        elapsed_sec += stat_net_total_time_sec - net_time0;
    }
    stat_access_time_sec += elapsed_sec;
    return reads;
}

std::vector<ClusterORAMStashBlock> ClusterORAM::get_cluster_stash(int cluster_id) const {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(cluster_block_counts.size())) {
        throw std::out_of_range("cluster_id is outside ClusterORAM range");
    }
    auto it = cluster_stash.find(cluster_id);
    if (it == cluster_stash.end()) {
        return {};
    }
    return it->second;
}

int ClusterORAM::cluster_stash_index(int cluster_id, int cluster_offset) const {
    return find_stash_index(cluster_id, cluster_offset);
}

ClusterORAMStashBlock ClusterORAM::get_cluster_stash_block(
        int cluster_id,
        int cluster_offset) const {
    const int stash_index = find_stash_index(cluster_id, cluster_offset);
    if (stash_index < 0) {
        throw std::out_of_range("ClusterORAM block is not present in stash");
    }
    auto stash_it = cluster_stash.find(cluster_id);
    if (stash_it == cluster_stash.end() ||
            stash_index >= static_cast<int>(stash_it->second.size())) {
        throw std::runtime_error("ClusterORAM stash index is inconsistent");
    }
    return stash_it->second[stash_index];
}

void ClusterORAM::update_cluster_stash_value(
        int cluster_id,
        int stash_block_index,
        int data_offset,
        int value) {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(cluster_block_counts.size())) {
        throw std::out_of_range("cluster_id is outside ClusterORAM range");
    }
    auto stash_it = cluster_stash.find(cluster_id);
    if (stash_it == cluster_stash.end()) {
        throw std::out_of_range("cluster_id does not currently have ClusterORAM stash blocks");
    }
    std::vector<ClusterORAMStashBlock>& blocks = stash_it->second;
    if (stash_block_index < 0 || stash_block_index >= static_cast<int>(blocks.size())) {
        throw std::out_of_range("stash_block_index is outside ClusterORAM stash range");
    }
    Block& block = blocks[stash_block_index].block;
    if (data_offset < 0 || data_offset >= block.block_size ||
            data_offset >= static_cast<int>(block.data.size())) {
        throw std::out_of_range("data_offset is outside ClusterORAM block payload");
    }
    block.data[data_offset] = value;
}

void ClusterORAM::replace_cluster_stash_block_data(
        int cluster_id,
        int cluster_offset,
        const std::vector<int>& data) {
    validate_cluster_block(cluster_id, cluster_offset);
    if (static_cast<int>(data.size()) != block_size) {
        throw std::invalid_argument("ClusterORAM replacement payload has the wrong block_size");
    }
    const int stash_index = find_stash_index(cluster_id, cluster_offset);
    if (stash_index < 0) {
        throw std::out_of_range("ClusterORAM block is not present in stash");
    }
    auto stash_it = cluster_stash.find(cluster_id);
    if (stash_it == cluster_stash.end() ||
            stash_index >= static_cast<int>(stash_it->second.size())) {
        throw std::runtime_error("ClusterORAM stash index is inconsistent");
    }
    stash_it->second[stash_index].block.data = data;
}

bool ClusterORAM::can_append_cluster_block() const {
    const long long capacity =
        static_cast<long long>(num_buckets - num_cached_bucket) * real_bucket_size;
    return static_cast<long long>(num_blocks) < capacity;
}

int ClusterORAM::append_cluster_block_to_stash(
        int cluster_id,
        const std::vector<int>& data) {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(cluster_block_counts.size())) {
        throw std::out_of_range("cluster_id is outside ClusterORAM range");
    }
    if (static_cast<int>(data.size()) != block_size) {
        throw std::invalid_argument("new ClusterORAM block payload has the wrong block_size");
    }
    if (!can_append_cluster_block()) {
        throw std::runtime_error("ClusterORAM physical tree capacity is exhausted; rebuild the generation");
    }

    const int cluster_offset = cluster_block_counts[cluster_id];
    const int logical_block_id = num_blocks;
    cluster_block_counts[cluster_id]++;
    cluster_stash_indices[cluster_id].push_back(-1);
    num_blocks++;

    try {
        const int leaf = cluster_leaf(cluster_id, cluster_counters[cluster_id], cluster_offset);
        Block* block = new Block(block_size);
        block->index = logical_block_id;
        block->data = data;
        add_to_stash(cluster_id, cluster_offset, leaf, block);
        return cluster_offset;
    } catch (...) {
        // Roll back all shape metadata if no usable new block could be staged.
        num_blocks--;
        cluster_block_counts[cluster_id]--;
        cluster_stash_indices[cluster_id].pop_back();
        throw;
    }
}

void ClusterORAM::evict_and_write_back() {
    const hires_clock::time_point t0 = hires_clock::now();
    const double net_time0 = stat_net_total_time_sec;
    stat_evict_invocations++;

    const int num_paths = cnt_delayed_leaves / evict_rate;
    if (num_paths == 0) {
        update_max_stash_blocks_after_evict();
        double elapsed_sec = elapsed_sec_since(t0);
        if (network_profile_.enabled && !network_profile_.sleep) {
            elapsed_sec += stat_net_total_time_sec - net_time0;
        }
        stat_evict_time_sec += elapsed_sec;
        return;
    }

    stat_evict_effective++;
    const hires_clock::time_point t_effective0 = hires_clock::now();
    const double net_effective_time0 = stat_net_total_time_sec;

    std::vector<int> leaves;
    leaves.reserve(num_paths);
    for (int i = 0; i < num_paths; i++) {
        leaves.push_back(ReverseBits(G, num_levels - 1));
        G++;
    }

    std::vector<int> path_positions;
    std::set<int> seen_positions;
    for (int level = num_levels - 1; level >= num_cached_levels; level--) {
        for (int leaf : leaves) {
            const int pos = P(leaf, level);
            if (seen_positions.insert(pos).second) {
                path_positions.push_back(pos);
            }
        }
    }
    account_remote_metadata_transfer(path_positions.size(), true);

    std::vector<int> read_positions;
    std::vector<int> offsets;
    std::vector<bool> valids;
    std::vector<std::pair<int, int>> read_identities;
    std::vector<int> read_leaves;
    for (int pos : path_positions) {
        for (int slot = 0; slot < bucket_size; slot++) {
            if (metadata[pos][slot].cluster_id == -1 || !metadata[pos][slot].valid) {
                continue;
            }
            read_positions.push_back(pos);
            offsets.push_back(slot);
            valids.push_back(true);
            read_identities.push_back({metadata[pos][slot].cluster_id, metadata[pos][slot].cluster_offset});
            read_leaves.push_back(metadata[pos][slot].leaf);
        }
    }

    const uint64_t cipher_block_bytes = ring_cipher_block_bytes(block_size);
    std::vector<Block*> read_blocks;
    if (!read_positions.empty()) {
        const uint64_t read_comm_bytes =
            block_read_comm_bytes(read_positions.size(), cipher_block_bytes);
        stat_comm_bytes += read_comm_bytes;
        account_network_transfer(read_comm_bytes, 1, true);
        storage->ReadBlockBatchAsBlockRing(read_positions, offsets, read_blocks, valids, false);
        stat_server_cipher_block_reads += static_cast<uint64_t>(read_positions.size());
    }

    for (size_t i = 0; i < read_blocks.size(); i++) {
        Block* block = read_blocks[i];
        if (block->index == -1) {
            delete block;
            continue;
        }
        add_to_stash(read_identities[i].first, read_identities[i].second, read_leaves[i], block);
    }

    for (int pos : path_positions) {
        for (ClusterORAMSlotMeta& meta : metadata[pos]) {
            meta.cluster_id = -1;
            meta.cluster_offset = -1;
            meta.leaf = -1;
            meta.valid = true;
        }
        bucket_read_counts[pos] = 0;
    }

    std::multimap<int, std::pair<int, int>> stash_by_leaf;
    for (auto& stash_pair : cluster_stash) {
        const int cluster_id = stash_pair.first;
        std::vector<ClusterORAMStashBlock>& stash_blocks = stash_pair.second;
        for (int i = 0; i < static_cast<int>(stash_blocks.size()); i++) {
            stash_by_leaf.insert({stash_blocks[i].leaf, {cluster_id, stash_blocks[i].cluster_offset}});
        }
    }

    std::map<int, std::vector<Block*>> evicted_storage;
    std::map<int, std::vector<std::pair<int, int>>> evicted_identities;
    std::map<int, std::vector<int>> evicted_leaves;
    for (int pos : path_positions) {
        evicted_storage[pos] = {};
        evicted_identities[pos] = {};
        evicted_leaves[pos] = {};
    }

    for (int level = num_levels - 1; level >= num_cached_levels; level--) {
        std::set<int> visited_positions_at_level;
        for (int leaf : leaves) {
            const int pos = P(leaf, level);
            if (!visited_positions_at_level.insert(pos).second) {
                continue;
            }
            auto storage_it = evicted_storage.find(pos);
            if (storage_it == evicted_storage.end()) {
                continue;
            }
            std::vector<Block*>& bucket = storage_it->second;
            std::vector<std::pair<int, int>>& identities = evicted_identities[pos];
            std::vector<int>& bucket_leaves = evicted_leaves[pos];
            const std::pair<int, int> leaf_range = Q(leaf, level);
            for (auto it = stash_by_leaf.lower_bound(leaf_range.first);
                    it != stash_by_leaf.end() &&
                    it->first < leaf_range.second &&
                    static_cast<int>(bucket.size()) < real_bucket_size;) {
                const int block_leaf = it->first;
                auto next_it = std::next(it);
                if (P(block_leaf, level) != pos) {
                    it = next_it;
                    continue;
                }

                const int cluster_id = it->second.first;
                const int cluster_offset = it->second.second;
                const int stash_index = find_stash_index(cluster_id, cluster_offset);
                auto stash_map_it = cluster_stash.find(cluster_id);
                if (stash_index < 0 || stash_map_it == cluster_stash.end()) {
                    stash_by_leaf.erase(it);
                    it = next_it;
                    continue;
                }

                std::vector<ClusterORAMStashBlock>& stash_blocks = stash_map_it->second;
                if (stash_index >= static_cast<int>(stash_blocks.size()) ||
                        stash_blocks[stash_index].cluster_offset != cluster_offset ||
                        stash_blocks[stash_index].cluster_id != cluster_id ||
                        stash_blocks[stash_index].leaf != block_leaf) {
                    stash_by_leaf.erase(it);
                    it = next_it;
                    continue;
                }

                const ClusterORAMStashBlock stash_block = stash_blocks[stash_index];
                bucket.push_back(new Block(stash_block.block));
                identities.push_back({stash_block.cluster_id, stash_block.cluster_offset});
                bucket_leaves.push_back(stash_block.leaf);

                cluster_stash_indices[cluster_id][cluster_offset] = -1;
                const int last_index = static_cast<int>(stash_blocks.size()) - 1;
                if (stash_index != last_index) {
                    stash_blocks[stash_index] = std::move(stash_blocks.back());
                    cluster_stash_indices[cluster_id][stash_blocks[stash_index].cluster_offset] = stash_index;
                }
                stash_blocks.pop_back();
                if (stash_blocks.empty()) {
                    cluster_stash.erase(cluster_id);
                }
                stash_by_leaf.erase(it);
                it = next_it;
            }
        }
    }

    std::random_device rd;
    std::mt19937 rng(rd());
    for (auto& pair : evicted_storage) {
        const int pos = pair.first;
        std::vector<Block*>& bucket = pair.second;
        while (static_cast<int>(bucket.size()) < bucket_size) {
            bucket.push_back(new Block(block_size));
            evicted_identities[pos].push_back({-1, -1});
            evicted_leaves[pos].push_back(-1);
        }
        std::vector<int> order(bucket.size());
        std::iota(order.begin(), order.end(), 0);
        std::shuffle(order.begin(), order.end(), rng);
        std::vector<Block*> shuffled_bucket;
        std::vector<std::pair<int, int>> shuffled_identities;
        std::vector<int> shuffled_leaves;
        shuffled_bucket.reserve(bucket.size());
        shuffled_identities.reserve(bucket.size());
        shuffled_leaves.reserve(bucket.size());
        for (int index : order) {
            shuffled_bucket.push_back(bucket[index]);
            shuffled_identities.push_back(evicted_identities[pos][index]);
            shuffled_leaves.push_back(evicted_leaves[pos][index]);
        }
        bucket.swap(shuffled_bucket);
        evicted_identities[pos].swap(shuffled_identities);
        evicted_leaves[pos].swap(shuffled_leaves);
        for (int slot = 0; slot < bucket_size; slot++) {
            const std::pair<int, int> identity = evicted_identities[pos][slot];
            if (identity.first == -1) {
                metadata[pos][slot] = ClusterORAMSlotMeta{};
            } else {
                metadata[pos][slot].cluster_id = identity.first;
                metadata[pos][slot].cluster_offset = identity.second;
                metadata[pos][slot].leaf = evicted_leaves[pos][slot];
                metadata[pos][slot].valid = true;
            }
        }
    }

    const uint64_t write_comm_bytes = bucket_write_comm_bytes(
        evicted_storage.size(),
        static_cast<size_t>(bucket_size),
        cipher_block_bytes);
    stat_comm_bytes += write_comm_bytes;
    account_network_transfer(write_comm_bytes, 1, true, true);
    storage->WriteBucketBatchMapAsBlockRing(evicted_storage, bucket_size, false);

    cnt_delayed_leaves -= num_paths * evict_rate;
    update_max_stash_blocks_after_evict();
    double effective_elapsed_sec = elapsed_sec_since(t_effective0);
    double elapsed_sec = elapsed_sec_since(t0);
    if (network_profile_.enabled && !network_profile_.sleep) {
        effective_elapsed_sec += stat_net_total_time_sec - net_effective_time0;
        elapsed_sec += stat_net_total_time_sec - net_time0;
    }
    stat_evict_effective_time_sec += effective_elapsed_sec;
    stat_evict_time_sec += elapsed_sec;
}

uint64_t ClusterORAM::cluster_counter(int cluster_id) const {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(cluster_counters.size())) {
        throw std::out_of_range("cluster_id is outside ClusterORAM range");
    }
    return cluster_counters[cluster_id];
}

int ClusterORAM::ReverseBits(int g, int bits_length) {
    int g_mod = g % num_leaves;
    int reverse = 0;
    while (g_mod) {
        reverse <<= 1;
        reverse |= g_mod & 1;
        g_mod >>= 1;
        bits_length--;
    }
    reverse <<= bits_length;
    return reverse;
}

std::pair<int, int> ClusterORAM::Q(int leaf, int level) {
    int l = num_levels - 1 - level;
    int left = leaf >> l;
    left = left << l;
    int right = left + (1 << l);
    return std::make_pair(left, right);
}

int ClusterORAM::P(int leaf, int level) {
    return (1 << level) - 1 + (leaf >> (num_levels - level - 1));
}

int ClusterORAM::getNumLeaves() {
    return num_leaves;
}

int ClusterORAM::getNumLevels() {
    return num_levels;
}

int ClusterORAM::getNumBlocks() {
    return num_blocks;
}

int ClusterORAM::getNumBuckets() {
    return num_buckets;
}

uint64_t ClusterORAM::current_stash_block_count() const {
    uint64_t count = 0;
    for (const auto& stash_pair : cluster_stash) {
        count += static_cast<uint64_t>(stash_pair.second.size());
    }
    return count;
}

void ClusterORAM::update_max_stash_blocks_after_evict() {
    stat_max_stash_blocks_after_evict = std::max(
        stat_max_stash_blocks_after_evict,
        current_stash_block_count());
}

void ClusterORAM::validate_cluster_block(int cluster_id, int cluster_offset) const {
    if (cluster_id < 0 || cluster_id >= static_cast<int>(cluster_block_counts.size())) {
        throw std::out_of_range("cluster_id is outside ClusterORAM range");
    }
    if (cluster_offset < 0 || cluster_offset >= cluster_block_counts[cluster_id]) {
        throw std::out_of_range("cluster_offset is outside ClusterORAM cluster range");
    }
}

int ClusterORAM::find_slot(int bucket_pos, int cluster_id, int cluster_offset) const {
    if (bucket_pos < 0 || bucket_pos >= static_cast<int>(metadata.size())) {
        throw std::out_of_range("bucket_pos is outside ClusterORAM metadata range");
    }
    const std::vector<ClusterORAMSlotMeta>& bucket = metadata[bucket_pos];
    for (int slot = 0; slot < static_cast<int>(bucket.size()); slot++) {
        if (!bucket[slot].valid) {
            continue;
        }
        if (bucket[slot].cluster_id == cluster_id &&
                bucket[slot].cluster_offset == cluster_offset) {
            return slot;
        }
    }
    return -1;
}

int ClusterORAM::find_dummy_slot(int bucket_pos) const {
    if (bucket_pos < 0 || bucket_pos >= static_cast<int>(metadata.size())) {
        throw std::out_of_range("bucket_pos is outside ClusterORAM metadata range");
    }
    const std::vector<ClusterORAMSlotMeta>& bucket = metadata[bucket_pos];
    for (int slot = 0; slot < static_cast<int>(bucket.size()); slot++) {
        if (bucket[slot].valid && bucket[slot].cluster_id == -1) {
            return slot;
        }
    }
    return -1;
}

int ClusterORAM::find_stash_index(int cluster_id, int cluster_offset) const {
    validate_cluster_block(cluster_id, cluster_offset);
    if (cluster_id >= static_cast<int>(cluster_stash_indices.size()) ||
            cluster_offset >= static_cast<int>(cluster_stash_indices[cluster_id].size())) {
        return -1;
    }
    return cluster_stash_indices[cluster_id][cluster_offset];
}

void ClusterORAM::add_to_stash(int cluster_id, int cluster_offset, int leaf, Block* block) {
    validate_cluster_block(cluster_id, cluster_offset);
    if (leaf < 0 || leaf >= num_leaves) {
        delete block;
        throw std::out_of_range("ClusterORAM stash leaf is outside the tree");
    }
    if (find_stash_index(cluster_id, cluster_offset) >= 0) {
        delete block;
        throw std::runtime_error("ClusterORAM duplicate block inserted into stash");
    }
    ClusterORAMStashBlock stash_block;
    stash_block.cluster_id = cluster_id;
    stash_block.cluster_offset = cluster_offset;
    stash_block.leaf = leaf;
    stash_block.block = Block(*block);
    std::vector<ClusterORAMStashBlock>& stash_blocks = cluster_stash[cluster_id];
    stash_blocks.push_back(std::move(stash_block));
    cluster_stash_indices[cluster_id][cluster_offset] = static_cast<int>(stash_blocks.size()) - 1;
    delete block;
}

} // namespace double_oram
