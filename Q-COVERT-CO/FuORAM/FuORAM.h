//
//
//

#ifndef DOUBLE_ORAM_FU_ORAM_H
#define DOUBLE_ORAM_FU_ORAM_H
#include "OramInterface.h"
#include "RandForOramInterface.h"
#include "NetworkSimulator.h"
// #include "UntrustedStorageInterface.h"
// #include "RemoteServerStorage.h"
#include "RemoteRing.h"
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <map>
#include <set>
#include <bitset>
#include <unordered_map>

namespace double_oram {

class OramMetaData {
public:
    uint8_t count;
    bitset<128> valids;
    vector<int> block_ids; // positive for
    // Current leaf of the real block in each block_ids slot; -1 for dummy or unknown slots.
    vector<int> block_leaves;

    // OramMetaData();

    int GetBlockOffset(int block_id, bool& real); 
    int GetBlockOffsetLeak(int block_id, bool& real); 
    void print_stat(); 
    // int ShuffleAndReset();
};

// DoubleORAM full-vector read result, including data and the caller-assigned new leaf.
struct FuORAMVectorRead {
    // Requested vector ID, also used as the FuORAM block ID.
    int vector_id;
    // Old leaf supplied by the caller.
    int old_leaf;
    // New leaf assigned by the caller for the state after this access.
    int new_leaf;
    // Retrieved full-vector data.
    vector<int> data;
};

class FuORAM : public OramInterface {
public:
    RemoteRing* storage;
    RandForOramInterface* rand_gen;
    int G;

    int block_size;
    int real_bucket_size;
    int dummy_size;
    int bucket_size;
    int evict_rate;

    int num_levels;
    int num_leaves;
    int num_blocks;
    int num_buckets;

    int cnt_delayed_leaves;

    bool cache_top;
    int num_cached_levels;
    int num_cached_bucket;
    map<int, Block*> cached_oram;
    int cnt_early_reshuffle;
    int cnt_q;

    /** Per-query ORAM statistics (reset by reset_query_access_stats(); snapshot after each search + gcache->reset()). */
    uint64_t stat_bmasr_invocations;
    uint64_t stat_real_block_indices;
    uint64_t stat_pad_slots;
    /** Ciphertext blocks read from server (sum of read batch sizes in Ring ORAM paths). */
    uint64_t stat_server_cipher_block_reads;
    /** Estimated client/server communication bytes for FuORAM reads and writebacks. */
    uint64_t stat_comm_bytes;

    /** `evict_and_write_back()` entries (including no-op when num_paths==0). */
    uint64_t stat_evict_invocations;
    /** Evictions that actually ran (num_paths > 0). */
    uint64_t stat_evict_effective;
    /** Wall time spent inside `evict_and_write_back` (seconds). */
    double stat_evict_time_sec;
    /** Wall time spent in effective FuORAM writebacks only (num_paths > 0). */
    double stat_evict_effective_time_sec;
    /** Wall time spent inside `access_without_position_map` (seconds, per call summed). */
    double stat_bmasr_time_sec;
    /** Per-query peak stash block count observed immediately after evict calls. */
    uint64_t stat_max_stash_blocks_after_evict;
    /** Per-query simulated network round trips for FuORAM operations. */
    uint64_t stat_net_round_trips;
    /** Per-query simulated network round trips for FuORAM access operations. */
    uint64_t stat_access_net_round_trips;
    /** Per-query simulated network round trips for FuORAM evict operations. */
    uint64_t stat_evict_net_round_trips;
    /** Per-query RTTs caused by final bucket writes in FuORAM evict operations. */
    uint64_t stat_evict_write_net_round_trips;
    /** Per-query simulated network transfer time from bandwidth, in seconds. */
    double stat_net_transfer_time_sec;
    /** Per-query simulated network latency time from RTT, in seconds. */
    double stat_net_latency_time_sec;
    /** Per-query total simulated network time, in seconds. */
    double stat_net_total_time_sec;

    void reset_query_access_stats();
    /** Configure the optional network simulator used by FuORAM communication accounting. */
    void set_network_profile(const NetworkProfile& profile);
    /** True when network simulation is enabled and already slept inside ORAM operations. */
    bool network_simulation_sleep_enabled() const;
    /** Configure whether bucket metadata is modeled as server-side state. */
    void set_metadata_remote(bool remote);
    /** True when bucket metadata is modeled as server-side state. */
    bool metadata_stored_on_server() const;

    bool batching;
    bool lazy_eveiction;

    vector<OramMetaData> metadata;

    map<std::pair<int, int>, Block*, PairComparator> mmstash;
    // DoubleORAM cache: vector_id -> new leaf assigned by the latest access without a position map.
    std::unordered_map<int, int> double_oram_leaf_cache;

    FuORAM(RemoteRing* storage,
            RandForOramInterface* rand_gen, RingOramConfig config, int num_levels, int cached_levels, bool batch = true, bool lazy = true);

    void evict_and_write_back();

    void init_cache_top();

    // DoubleORAM access with caller-supplied old and new leaves, bypassing the FuORAM position map.
    // Exhausted buckets are read using exactly Z slots and reshuffled at the end of the call.
    std::vector<FuORAMVectorRead> access_without_position_map(
            const std::vector<int>& vector_ids,
            const std::vector<int>& old_leaves,
            const std::vector<int>& new_leaves,
            int pad_l);
    // Stage a new full-vector block in the FuORAM stash and perform dummy_access_count
    // dummy path accesses with the normal query shape; return the new block's assigned leaf.
    int insert_block_to_stash(
            int vector_id,
            const std::vector<int>& data,
            int dummy_access_count);
    // Return whether vector_id already exists in FuORAM metadata or the stash.
    bool contains_block(int vector_id) const;
    // Read the temporary DoubleORAM full-vector leaf cache.
    const std::unordered_map<int, int>& double_oram_cached_leaves() const;
    // Clear the temporary DoubleORAM full-vector leaf cache.
    void clear_double_oram_leaf_cache();

    int globalG();
    int ReverseBits(int G, int bits_length);

    /** Reshuffle exhausted buckets and return the reshuffle-read communication bytes. */
    uint64_t early_reshuffle(vector<int> buckets, bool account_read = true);

    std::pair<int, int> Q(int leaf, int level);
    int P(int leaf, int level);
    void loadMetaDataFromFile(const char* fname);
    int getNumLeaves();
    int getNumLevels();
    int getNumBlocks();
    int getNumBuckets();

private:
    /** Active network simulation profile for this ORAM. */
    NetworkProfile network_profile_;
    /** Whether bucket metadata is counted as remote server-side state. */
    bool metadata_remote_;
    /** Add one logical FuORAM network transfer to the per-query network stats. */
    void account_network_transfer(
        uint64_t bytes,
        uint64_t round_trips = 1,
        bool evict_phase = false,
        bool evict_write = false);
    /** Persistent metadata bytes for one bucket. */
    uint64_t metadata_bucket_bytes() const;
    /** Account one logical remote metadata transfer when metadata is server-side. */
    void account_remote_metadata_transfer(size_t bucket_count, bool evict_phase = false);

};

} // namespace double_oram


#endif 
