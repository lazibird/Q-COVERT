#ifndef DOUBLE_ORAM_CLUSTER_ORAM_H
#define DOUBLE_ORAM_CLUSTER_ORAM_H

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

// ClusterORAM 读取某个簇后返回的 stash 块位置。
struct ClusterORAMClusterRead {
    // 被读取的簇 id。
    int cluster_id;
    // 该簇当前读出的每个块在 ClusterORAM stash 分组中的下标。
    std::vector<int> stash_block_indices;
};

// ClusterORAM stash 中的一个真实块，附带簇 id 和簇内 block 编号。
struct ClusterORAMStashBlock {
    // 块所属簇 id。
    int cluster_id;
    // 块在簇内的编号，也是 PRF 的 cluster_offset 参数。
    int cluster_offset;
    // 块下一次写回应使用的 leaf；读簇时用 counter+1 预先计算。
    int leaf;
    // 实际 ORAM block 数据。
    Block block;
};

// ClusterORAM bucket 槽位 metadata；客户端明文保存。
struct ClusterORAMSlotMeta {
    // 槽位中真实块所属簇 id；dummy 为 -1。
    int cluster_id = -1;
    // 槽位中真实块在簇内编号；dummy 为 -1。
    int cluster_offset = -1;
    // 槽位中真实块当前所在路径 leaf；dummy 为 -1。
    int leaf = -1;
    // 槽位自上次 reshuffle/evict 后是否仍可读取。
    bool valid = true;
};

// 使用 RingORAM 风格路径读写按簇组织的低精度量化块。
class ClusterORAM : public OramInterface {
public:
    // 底层远程存储，复用 RemoteRing 的加密 block 存储接口。
    RemoteRing* storage;
    // ORAM 随机数接口，当前用于接口兼容和可能的 shuffle。
    RandForOramInterface* rand_gen;

    // 单个 block 的 int 槽位数量。
    int block_size;
    // 每个 bucket 可容纳的真实块数量。
    int real_bucket_size;
    // 每个 bucket 的 dummy 槽位数量。
    int dummy_size;
    // 每个 bucket 的总槽位数，等于 real_bucket_size + dummy_size。
    int bucket_size;
    // 每次 evict 的路径数量。
    int evict_rate;

    // ORAM 树层数。
    int num_levels;
    // ORAM 叶子数量。
    int num_leaves;
    // 真实低精度块总数。
    int num_blocks;
    // ORAM bucket 数量。
    int num_buckets;
    // 是否启用树顶缓存；启用后服务端路径读写跳过顶部 num_cached_levels 层。
    bool cache_top;
    // 缓存在客户端的顶部层数，0 表示不缓存。
    int num_cached_levels;
    // 顶部缓存覆盖的 bucket 数量。
    int num_cached_bucket;

    // 当前 reverse lexicographic eviction 计数器。
    int G;
    // 自上次 evict 以来累计读取的真实路径数量。
    int cnt_delayed_leaves;

    // 每个簇的块数量 p_c。
    std::vector<int> cluster_block_counts;
    // 每个簇的访问计数器；读取后递增，用于下一次路径计算和 evict。
    std::vector<uint64_t> cluster_counters;
    // 客户端明文 bucket metadata。
    std::vector<std::vector<ClusterORAMSlotMeta>> metadata;
    // 每个 bucket 自上次 reshuffle/evict 后已经消耗的读取次数。
    std::vector<int> bucket_read_counts;
    // ClusterORAM stash，按 cluster_id 分组。
    std::map<int, std::vector<ClusterORAMStashBlock>> cluster_stash;
    // 每个簇内 cluster_offset 到 stash 下标的索引；不存在时为 -1。
    std::vector<std::vector<int>> cluster_stash_indices;

    // 每次 query 统计：簇访问次数。
    uint64_t stat_access_invocations;
    // 每次 query 统计：读取的真实簇块数量。
    uint64_t stat_real_block_indices;
    // 每次 query 统计：server ciphertext block 读取数量。
    uint64_t stat_server_cipher_block_reads;
    // 每次 query 统计：估算通信量。
    uint64_t stat_comm_bytes;
    // 每次 query 统计：evict 调用次数。
    uint64_t stat_evict_invocations;
    // 每次 query 统计：实际执行的 evict 次数。
    uint64_t stat_evict_effective;
    // 每次 query 统计：access 累计耗时，单位秒。
    double stat_access_time_sec;
    // 每次 query 统计：evict 调用累计耗时，单位秒。
    double stat_evict_time_sec;
    // 每次 query 统计：实际 evict 累计耗时，单位秒。
    double stat_evict_effective_time_sec;
    // 每次 query 统计：evict 调用结束后 ClusterORAM stash 块数峰值。
    uint64_t stat_max_stash_blocks_after_evict;
    // 每次 query 统计：ClusterORAM 模拟网络往返次数。
    uint64_t stat_net_round_trips;
    // 每次 query 统计：ClusterORAM access 阶段模拟网络往返次数。
    uint64_t stat_access_net_round_trips;
    // 每次 query 统计：ClusterORAM evict 阶段模拟网络往返次数。
    uint64_t stat_evict_net_round_trips;
    // 每次 query 统计：ClusterORAM evict 最终 bucket 写回产生的 RTT 次数。
    uint64_t stat_evict_write_net_round_trips;
    // 每次 query 统计：由带宽产生的模拟传输时间，单位秒。
    double stat_net_transfer_time_sec;
    // 每次 query 统计：由 RTT 产生的模拟等待时间，单位秒。
    double stat_net_latency_time_sec;
    // 每次 query 统计：模拟网络总时间，单位秒。
    double stat_net_total_time_sec;

    ClusterORAM(
            RemoteRing* storage,
            RandForOramInterface* rand_gen,
            const RingOramConfig& config,
            int num_levels,
            const std::vector<int>& cluster_block_counts);

    // 清空当前 query 的访问、写回、耗时和通信统计。
    void reset_query_access_stats();
    // 配置 ClusterORAM 通信统计使用的网络模拟参数。
    void set_network_profile(const NetworkProfile& profile);
    // 设置 metadata 是否按服务器端存储来模拟通信；false 表示客户端本地保存。
    void set_metadata_remote(bool remote);
    // 返回当前 metadata 归属是否为服务器端。
    bool metadata_stored_on_server() const;

    // 计算指定簇块在指定访问计数下对应的 leaf。
    int cluster_leaf(int cluster_id, uint64_t counter, int cluster_offset) const;
    // 一次性计算指定簇全部块在指定访问计数下对应的 leaf。
    std::vector<int> cluster_leaves(int cluster_id, uint64_t counter) const;
    // 读取一个或多个簇的全部低精度块。
    std::vector<ClusterORAMClusterRead> read_clusters_for_double_oram(const std::vector<int>& cluster_ids);
    // 返回指定簇当前 stash 副本。
    std::vector<ClusterORAMStashBlock> get_cluster_stash(int cluster_id) const;
    // 返回指定簇内 block 在 stash 分组中的下标；该 block 不在 stash 时返回 -1。
    int cluster_stash_index(int cluster_id, int cluster_offset) const;
    // 返回指定簇内 block 的 stash 副本；该 block 必须已经由读取操作放入 stash。
    ClusterORAMStashBlock get_cluster_stash_block(int cluster_id, int cluster_offset) const;
    // 更新指定簇 stash 中某个块的 Block::data 字段。
    void update_cluster_stash_value(int cluster_id, int stash_block_index, int data_offset, int value);
    // 整体替换指定 stash block 的 payload；用于原子更新 ANN 批量块中的多个字段。
    void replace_cluster_stash_block_data(
            int cluster_id,
            int cluster_offset,
            const std::vector<int>& data);
    // 返回当前物理树是否还能容纳一个新的 ClusterORAM 真实 block。
    bool can_append_cluster_block() const;
    // 为指定簇追加一个新的量化 block 到 stash，并返回其簇内 block 编号。
    // 调用方必须已访问该簇，且 data 的长度必须等于 ClusterORAM block_size。
    int append_cluster_block_to_stash(int cluster_id, const std::vector<int>& data);
    // 按 RingORAM reverse lexicographic 规则执行 query-end evict。
    void evict_and_write_back();
    // 返回指定簇当前访问计数。
    uint64_t cluster_counter(int cluster_id) const;

    int ReverseBits(int G, int bits_length);
    std::pair<int, int> Q(int leaf, int level);
    int P(int leaf, int level) override;
    int getNumLeaves() override;
    int getNumLevels() override;
    int getNumBlocks() override;
    int getNumBuckets() override;

private:
    // 当前 ClusterORAM 使用的网络模拟参数。
    NetworkProfile network_profile_;
    // 是否把 bucket metadata 当作服务器端状态来统计额外访问通信。
    bool metadata_remote_;
    // 将一次逻辑网络传输计入当前 query 的模拟网络统计。
    void account_network_transfer(
        uint64_t bytes,
        uint64_t round_trips = 1,
        bool evict_phase = false,
        bool evict_write = false);
    // 单个 bucket 的 ClusterORAM metadata 持久化字节数。
    uint64_t metadata_bucket_bytes() const;
    // metadata 在服务器端时，计入一次读取若干 bucket metadata 的逻辑通信。
    void account_remote_metadata_transfer(size_t bucket_count, bool evict_phase = false);
    // 对读取次数耗尽的 bucket 执行 RingORAM 风格 early reshuffle，返回 reshuffle 读阶段字节数。
    uint64_t early_reshuffle(const std::vector<int>& buckets, bool account_read = true);
    // 统计当前 ClusterORAM stash 中真实块数量。
    uint64_t current_stash_block_count() const;
    // 在 evict 调用结束点更新 stash 峰值统计。
    void update_max_stash_blocks_after_evict();
    // 检查簇 id 和 offset 是否在配置范围内。
    void validate_cluster_block(int cluster_id, int cluster_offset) const;
    // 从 bucket metadata 中查找目标块槽位；未找到返回 -1。
    int find_slot(int bucket_pos, int cluster_id, int cluster_offset) const;
    // 从 bucket metadata 中查找仍可读取的 dummy 槽位；未找到返回 -1。
    int find_dummy_slot(int bucket_pos) const;
    // 从簇内索引中查找 stash 下标；未找到返回 -1。
    int find_stash_index(int cluster_id, int cluster_offset) const;
    // 将 block 及其下一次写回 leaf 放入 stash。
    void add_to_stash(int cluster_id, int cluster_offset, int leaf, Block* block);
};

} // namespace double_oram

#endif
