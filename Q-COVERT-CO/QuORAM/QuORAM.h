#ifndef DOUBLE_ORAM_QU_ORAM_H
#define DOUBLE_ORAM_QU_ORAM_H

#include "OramInterface.h"
#include "RandForOramInterface.h"
#include "NetworkSimulator.h"
#include "RemoteRing.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace double_oram {

// Stash block locations returned after reading a QuORAM cluster.
struct QuORAMClusterRead {
    // ID of the requested cluster.
    int cluster_id;
    // Indices of the retrieved blocks within this cluster's QuORAM stash group.
    std::vector<int> stash_block_indices;
};

// A real block in the QuORAM stash, with its cluster ID and block offset.
struct QuORAMStashBlock {
    // ID of the cluster containing this block.
    int cluster_id;
    // Block offset within the cluster; also the PRF cluster_offset argument.
    int cluster_offset;
    // Leaf for the next writeback, derived using counter + 1 during the cluster read.
    int leaf;
    // ORAM block data.
    Block block;
};

// QuORAM bucket slot metadata, stored in plaintext at the client.
struct QuORAMSlotMeta {
    // Cluster ID of the real block in this slot; -1 for a dummy.
    int cluster_id = -1;
    // Block offset within the cluster; -1 for a dummy.
    int cluster_offset = -1;
    // Current path leaf of the real block; -1 for a dummy.
    int leaf = -1;
    // Whether the slot remains readable since the last reshuffle or eviction.
    bool valid = true;
};

// Clustered quantized blocks accessed through FuORAM-style path operations.
class QuORAM : public OramInterface {
public:
    // Remote storage using the RemoteRing encrypted block interface.
    RemoteRing* storage;
    // ORAM random generator interface, used for dummy paths and interface compatibility.
    RandForOramInterface* rand_gen;

    // Number of int slots in each block.
    int block_size;
    // Real block capacity of each bucket.
    int real_bucket_size;
    // Number of dummy slots in each bucket.
    int dummy_size;
    // Total slots per bucket: real_bucket_size + dummy_size.
    int bucket_size;
    // Number of accesses per eviction path.
    int evict_rate;

    // Number of levels in the ORAM tree.
    int num_levels;
    // Number of ORAM leaves.
    int num_leaves;
    // Total number of real quantized blocks.
    int num_blocks;
    // Number of ORAM buckets.
    int num_buckets;
    // Enable the treetop cache; server path operations skip num_cached_levels levels.
    bool cache_top;
    // Number of top levels cached at the client; zero disables caching.
    int num_cached_levels;
    // Number of buckets covered by the treetop cache.
    int num_cached_bucket;

    // Current reverse lexicographic eviction counter.
    int G;
    // Paths read since the last eviction, including real and dummy accesses.
    int cnt_delayed_leaves;

    // Number of blocks p_c in each cluster.
    std::vector<int> cluster_block_counts;
    // Per-cluster access counters, incremented after reads for leaf derivation and eviction.
    std::vector<uint64_t> cluster_counters;
    // Plaintext bucket metadata stored at the client.
    std::vector<std::vector<QuORAMSlotMeta>> metadata;
    // Read budget consumed by each bucket since its last reshuffle or eviction.
    std::vector<int> bucket_read_counts;
    // QuORAM stash grouped by cluster_id.
    std::map<int, std::vector<QuORAMStashBlock>> cluster_stash;
    // Map each cluster_offset to its stash index; -1 if absent.
    std::vector<std::vector<int>> cluster_stash_indices;

    // Per-query count of cluster batch accesses.
    uint64_t stat_access_invocations;
    // Per-query count of real cluster blocks read.
    uint64_t stat_real_block_indices;
    // Total real and dummy logical path accesses in the current query.
    uint64_t stat_access_paths;
    // Per-query count of server ciphertext blocks read.
    uint64_t stat_server_cipher_block_reads;
    // Per-query estimated communication volume.
    uint64_t stat_comm_bytes;
    // Per-query number of eviction calls.
    uint64_t stat_evict_invocations;
    // Per-query number of calls that perform an eviction.
    uint64_t stat_evict_effective;
    // Per-query cumulative access time in seconds.
    double stat_access_time_sec;
    // Per-query cumulative eviction call time in seconds.
    double stat_evict_time_sec;
    // Per-query cumulative time spent performing evictions in seconds.
    double stat_evict_effective_time_sec;
    // Per-query peak QuORAM stash size measured at eviction call completion.
    uint64_t stat_max_stash_blocks_after_evict;
    // Per-query simulated QuORAM network round trips.
    uint64_t stat_net_round_trips;
    // Per-query simulated network round trips during QuORAM access.
    uint64_t stat_access_net_round_trips;
    // Per-query simulated network round trips during QuORAM eviction.
    uint64_t stat_evict_net_round_trips;
    // Per-query round trips for final bucket writeback during eviction.
    uint64_t stat_evict_write_net_round_trips;
    // Per-query simulated transfer time determined by bandwidth, in seconds.
    double stat_net_transfer_time_sec;
    // Per-query simulated RTT delay in seconds.
    double stat_net_latency_time_sec;
    // Per-query total simulated network time in seconds.
    double stat_net_total_time_sec;

    QuORAM(
            RemoteRing* storage,
            RandForOramInterface* rand_gen,
            const RingOramConfig& config,
            int num_levels,
            const std::vector<int>& cluster_block_counts);

    // Reset the current query's access, writeback, timing, and communication statistics.
    void reset_query_access_stats();
    // Configure the network model used for QuORAM communication statistics.
    void set_network_profile(const NetworkProfile& profile);
    // Simulate metadata traffic for server storage; false keeps metadata at the client.
    void set_metadata_remote(bool remote);
    // Return whether metadata is accounted for as server-side storage.
    bool metadata_stored_on_server() const;

    // Derive the leaf of a cluster block for the specified access counter.
    int cluster_leaf(int cluster_id, uint64_t counter, int cluster_offset) const;
    // Derive all cluster block leaves for the specified access counter.
    std::vector<int> cluster_leaves(int cluster_id, uint64_t counter) const;
    // Read all blocks of the requested clusters; pad_to_blocks adds dummy paths to the batch.
    std::vector<QuORAMClusterRead> read_clusters_for_double_oram(
            const std::vector<int>& cluster_ids, size_t pad_to_blocks = 0);
    // Return a copy of the specified cluster's current stash.
    std::vector<QuORAMStashBlock> get_cluster_stash(int cluster_id) const;
    // Return the block's index within its cluster stash group; -1 if absent.
    int cluster_stash_index(int cluster_id, int cluster_offset) const;
    // Return a copy of a block previously loaded into the cluster stash.
    QuORAMStashBlock get_cluster_stash_block(int cluster_id, int cluster_offset) const;
    // Update a Block::data field in the specified cluster stash block.
    void update_cluster_stash_value(int cluster_id, int stash_block_index, int data_offset, int value);
    // Replace a stash block payload to update multiple ANN fields atomically.
    void replace_cluster_stash_block_data(
            int cluster_id,
            int cluster_offset,
            const std::vector<int>& data);
    // Return whether the physical tree can accommodate another real QuORAM block.
    bool can_append_cluster_block() const;
    // Append a quantized block to the cluster stash and return its cluster block offset.
    // The cluster must already have been accessed, and data must contain block_size ints.
    int append_cluster_block_to_stash(int cluster_id, const std::vector<int>& data);
    // Perform query-end eviction using the FuORAM reverse lexicographic schedule.
    void evict_and_write_back();
    // Return the specified cluster's current access counter.
    uint64_t cluster_counter(int cluster_id) const;

    int ReverseBits(int G, int bits_length);
    std::pair<int, int> Q(int leaf, int level);
    int P(int leaf, int level) override;
    int getNumLeaves() override;
    int getNumLevels() override;
    int getNumBlocks() override;
    int getNumBuckets() override;

private:
    // Current QuORAM network simulation settings.
    NetworkProfile network_profile_;
    // Account for additional metadata traffic as server-side state.
    bool metadata_remote_;
    // Account for one logical transfer in the current query's network statistics.
    void account_network_transfer(
        uint64_t bytes,
        uint64_t round_trips = 1,
        bool evict_phase = false,
        bool evict_write = false);
    // Serialized QuORAM metadata bytes per bucket.
    uint64_t metadata_bucket_bytes() const;
    // Account for reading metadata for a set of buckets when metadata is remote.
    void account_remote_metadata_transfer(size_t bucket_count, bool evict_phase = false);
    // Early reshuffle exhausted buckets and return the reshuffle read volume in bytes.
    uint64_t early_reshuffle(const std::vector<int>& buckets, bool account_read = true);
    // Count real blocks currently in the QuORAM stash.
    uint64_t current_stash_block_count() const;
    // Update peak stash statistics at eviction call completion.
    void update_max_stash_blocks_after_evict();
    // Validate the cluster ID and block offset against the configured bounds.
    void validate_cluster_block(int cluster_id, int cluster_offset) const;
    // Find a target block slot in the bucket metadata; return -1 if absent.
    int find_slot(int bucket_pos, int cluster_id, int cluster_offset) const;
    // Find a readable dummy slot in the bucket metadata; return -1 if absent.
    int find_dummy_slot(int bucket_pos) const;
    // Look up a stash index in the per-cluster index; return -1 if absent.
    int find_stash_index(int cluster_id, int cluster_offset) const;
    // Add a block and its next writeback leaf to the stash.
    void add_to_stash(int cluster_id, int cluster_offset, int leaf, Block* block);
};

} // namespace double_oram

#endif
