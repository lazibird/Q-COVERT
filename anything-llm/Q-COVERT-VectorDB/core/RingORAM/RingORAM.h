//
//
//

#ifndef DOUBLE_ORAM_RING_ORAM_H
#define DOUBLE_ORAM_RING_ORAM_H
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
    // block_ids 同槽位真实块当前对应的 leaf；dummy 或未知槽位为 -1。
    vector<int> block_leaves;

    // OramMetaData();

    int GetBlockOffset(int block_id, bool& real); 
    int GetBlockOffsetLeak(int block_id, bool& real); 
    void print_stat(); 
    // int ShuffleAndReset();
};

// DoubleORAM Ring 侧读取结果：数据和调用方分配的新 leaf 一起返回。
struct RingORAMVectorRead {
    // 被访问的向量 id，也是 RingORAM block id。
    int vector_id;
    // 调用方提供的旧 leaf。
    int old_leaf;
    // 调用方为本次访问后状态分配的新 leaf。
    int new_leaf;
    // 读取到的高精度向量数据。
    vector<int> data;
};

class RingORAM : public OramInterface {
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
    /** Estimated client/server communication bytes for RingORAM reads and writebacks. */
    uint64_t stat_comm_bytes;

    /** `evict_and_write_back()` entries (including no-op when num_paths==0). */
    uint64_t stat_evict_invocations;
    /** Evictions that actually ran (num_paths > 0). */
    uint64_t stat_evict_effective;
    /** Wall time spent inside `evict_and_write_back` (seconds). */
    double stat_evict_time_sec;
    /** Wall time spent in effective RingORAM writebacks only (num_paths > 0). */
    double stat_evict_effective_time_sec;
    /** Wall time spent inside `access_without_position_map` (seconds, per call summed). */
    double stat_bmasr_time_sec;
    /** Per-query peak stash block count observed immediately after evict calls. */
    uint64_t stat_max_stash_blocks_after_evict;
    /** Per-query simulated network round trips for RingORAM operations. */
    uint64_t stat_net_round_trips;
    /** Per-query simulated network round trips for RingORAM access operations. */
    uint64_t stat_access_net_round_trips;
    /** Per-query simulated network round trips for RingORAM evict operations. */
    uint64_t stat_evict_net_round_trips;
    /** Per-query RTTs caused by final bucket writes in RingORAM evict operations. */
    uint64_t stat_evict_write_net_round_trips;
    /** Per-query simulated network transfer time from bandwidth, in seconds. */
    double stat_net_transfer_time_sec;
    /** Per-query simulated network latency time from RTT, in seconds. */
    double stat_net_latency_time_sec;
    /** Per-query total simulated network time, in seconds. */
    double stat_net_total_time_sec;

    void reset_query_access_stats();
    /** Configure the optional network simulator used by RingORAM communication accounting. */
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
    // DoubleORAM 专用临时缓存：vector_id -> 最近一次无 position_map 访问后分配的新 leaf。
    std::unordered_map<int, int> double_oram_leaf_cache;

    RingORAM(RemoteRing* storage,
            RandForOramInterface* rand_gen, RingOramConfig config, int num_levels, int cached_levels, bool batch = true, bool lazy = true);

    void evict_and_write_back();

    void init_cache_top();

    // DoubleORAM 专用读取接口：调用方提供每个 vector 的旧 leaf 和新 leaf，不依赖 RingORAM position_map。
    // 如果某些 bucket 的读取预算耗尽，access 会固定读取这些 bucket 的 Z 个槽位并在本次调用末尾 reshuffle。
    std::vector<RingORAMVectorRead> access_without_position_map(
            const std::vector<int>& vector_ids,
            const std::vector<int>& old_leaves,
            const std::vector<int>& new_leaves,
            int pad_l);
    // 将尚未写入服务器的新高精度 block 放入 RingORAM stash，并执行 dummy_access_count 条
    // 与普通查询相同形状的 dummy 路径访问；返回分配给新 block 的 leaf。
    int insert_block_to_stash(
            int vector_id,
            const std::vector<int>& data,
            int dummy_access_count);
    // 读取 DoubleORAM Ring 侧临时 leaf 缓存。
    const std::unordered_map<int, int>& double_oram_cached_leaves() const;
    // 清空 DoubleORAM Ring 侧临时 leaf 缓存。
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
    /** Add one logical RingORAM network transfer to the per-query network stats. */
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

} // double_oram 命名空间


#endif 
