#ifndef DOUBLE_ORAM_DOUBLE_ORAM_H
#define DOUBLE_ORAM_DOUBLE_ORAM_H

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace double_oram {

class Block;
class ClusterORAM;
class RingORAM;

// 返回存储 byte_count 个字节所需的 int 槽位数，当前按 4 bytes/int 打包。
int packed_byte_int_count(int byte_count);
// 将 bytes 按小端顺序打包到 int 槽位。
std::vector<int> pack_bytes_to_ints(const std::vector<uint8_t>& bytes);
// 从 int 槽位按小端顺序解包出 byte_count 个字节。
std::vector<uint8_t> unpack_bytes_from_ints(const std::vector<int>& words, int word_offset, int byte_count);

// ClusterORAM 中低精度块的逻辑布局。
enum class DoubleORAMPathBlockLayout {
    // 旧布局：每个向量槽位保存 vector_id、ring_leaf 和定长量化 code。
    PerVectorCodes,
    // ANN 布局：一个块保存 32 个向量的元数据，以及整块 FastScan payload。
    AnnBatch
};

// DoubleORAM 顶层协议参数，描述簇访问、量化块布局和向量数据维度。
struct DoubleORAMConfig {
    // IVF 分簇数量，也是 ClusterORAM 簇计数器 vector 的长度。
    int num_clusters;
    // 每个量化块最多容纳的向量数量，默认按说明设置为 32。
    int vectors_per_quant_block;
    // 保留旧参数；ClusterORAM 使用每簇实际 block 数和 block offset 计算路径。
    int paths_per_cluster;
    // 旧布局下表示单个向量的低精度量化二进制长度；ANN 布局下表示整块 FastScan payload 字节数。
    int quantized_code_bytes;
    // 原始向量维度，供上层解释 RingORAM 高精度数据。
    int vector_dimension;
    // 量化块中 cluster_id 所在的 Block::data 下标，默认布局为 0。
    int quantized_block_cluster_id_offset;
    // ClusterORAM 低精度块布局类型。
    DoubleORAMPathBlockLayout path_block_layout;

    // 构造并校验 DoubleORAM 顶层参数。
    DoubleORAMConfig(
            int num_clusters,
            int quantized_code_bytes,
            int vector_dimension,
            int vectors_per_quant_block = 32,
            int paths_per_cluster = 2,
            int quantized_block_cluster_id_offset = 0,
            DoubleORAMPathBlockLayout path_block_layout = DoubleORAMPathBlockLayout::PerVectorCodes);
};

// ClusterORAM 读取簇后返回的单个向量低精度量化条目。
struct QuantizedVectorEntry {
    // 向量唯一 id，同时也是 RingORAM 中高精度块的 block id。
    int vector_id;
    // 该向量高精度块当前所在的 RingORAM leaf。
    int ring_leaf;
    // 固定长度低精度量化二进制，长度应等于 DoubleORAMConfig::quantized_code_bytes。
    std::vector<uint8_t> quantized_code;
};

// ClusterORAM 按簇读取后的返回结果，一个对象对应一个 cluster_id。
struct ClusterQuantizedRead {
    // 本次读取的簇 id。
    int cluster_id;
    // 该簇内所有被取回的向量低精度量化条目。
    std::vector<QuantizedVectorEntry> vectors;
};

// ANN 批量近似块读取结果，一个对象对应一个 ClusterORAM approx block。
struct AnnApproxBlockRead {
    // 该近似块所属 IVF 簇 id。
    int cluster_id;
    // 该近似块在簇内的 block id，用于还原 ANN 的 cluster/block 访问顺序。
    int block_id;
    // 该近似块 32 个槽位的向量 id；dummy 槽位为负数。
    std::vector<int> vector_ids;
    // 该近似块 32 个槽位对应的 RingORAM leaf；dummy 槽位为负数。
    std::vector<int> ring_leaves;
    // ANN FastScan 所需整块 payload，布局为 32 个 Factor 加 32 个 packed 1bit code。
    std::vector<uint8_t> payload;
};

// ANN 按簇读取后的近似块集合，一个对象对应一个 cluster_id。
struct AnnClusterApproxRead {
    // 本次读取的簇 id。
    int cluster_id;
    // 该簇当前读出的所有 ANN 近似块。
    std::vector<AnnApproxBlockRead> blocks;
};

// RingORAM 读取单个高精度向量后的返回结果。
struct RingVectorRead {
    // 被读取的向量 id。
    int vector_id;
    // 读取前由 ClusterORAM 返回并由 DoubleORAM 缓存的旧 RingORAM leaf。
    int old_leaf;
    // RingORAM 读取后重新分配的新 leaf，需要回写到 ClusterORAM stash。
    int new_leaf;
    // 高精度向量数据；长度和解释方式由 RingORAM block_size 与上层 ANN 决定。
    std::vector<int> data;
};

// 插入一个 ANN 向量时写入 ApproxOramBlockLayout 单个槽位的低精度数据。
struct AnnQuantizedVectorPayload {
    // 一个向量对应的 Factor 序列化字节；当前 IVF-RaBitQ 实现为四个 float。
    std::vector<uint8_t> factor;
    // 一个向量对应的原始 1-bit RaBitQ code 字节，字节序必须与
    // IvfRabitQIndex::binary_code 中同一向量的字节序一致。DoubleORAM 会将其
    // 转换到 ApproxOramBlockLayout 的 32 槽 FastScan 转置布局。
    std::vector<uint8_t> binary_code;
};

// DoubleORAM 插入完成后返回的位置与新 RingORAM leaf。
struct DoubleORAMInsertResult {
    // 新向量所属 IVF 簇。
    int cluster_id;
    // 新向量所在近似块的簇内编号。
    int cluster_block_id;
    // 新向量在近似块内的槽位编号。
    int vector_slot;
    // 新向量在 RingORAM 中分配的初始 leaf。
    int ring_leaf;
};

// 记录一个向量当前位于第一层 ClusterORAM stash 中哪个量化块。
struct PathStashVectorLocation {
    // 向量所属簇 id。
    int cluster_id;
    // 该簇 stash 分组里的量化块下标。
    int stash_block_index;
    // 向量在量化块中的槽位下标。
    int vector_slot;
};

// DoubleORAM 顶层协调器，负责串联 ClusterORAM 低精度簇读和 RingORAM 高精度向量读。
class DoubleORAM {
public:
    // 绑定已有 ClusterORAM 和 RingORAM 实例，并保存 DoubleORAM 协议参数。
    DoubleORAM(ClusterORAM& cluster_oram, RingORAM& ring_oram, const DoubleORAMConfig& config);

    // 返回当前 DoubleORAM 参数。
    const DoubleORAMConfig& config() const;
    // 返回 vector_id 到 ClusterORAM stash 位置的缓存，主要用于测试和调试。
    const std::unordered_map<int, PathStashVectorLocation>& path_stash_locations() const;
    // 返回 vector_id 到当前 RingORAM leaf 的缓存，来自最近一次 ClusterORAM 簇读取。
    const std::unordered_map<int, int>& cached_ring_leaves() const;

    // 读取一个或多个簇的量化数据，并缓存这些向量对应的 RingORAM leaf。
    std::vector<ClusterQuantizedRead> read_clusters(const std::vector<int>& cluster_ids);
    // 读取一个或多个簇的 ANN 批量近似块，并缓存这些向量对应的 RingORAM leaf。
    std::vector<AnnClusterApproxRead> read_ann_clusters(const std::vector<int>& cluster_ids);
    // 使用已缓存的 RingORAM leaf 读取指定向量的高精度数据，并在 DoubleORAM 中生成新 leaf。
    std::vector<RingVectorRead> read_ring_vectors(const std::vector<int>& vector_ids);
    // 将 RingORAM 读取得到的新 leaf 更新到 ClusterORAM stash 中的量化块。
    void update_path_stash_ring_leaves(const std::vector<RingVectorRead>& reads);
    // 插入一个 ANN 向量：先读取 candidate_cluster_ids 形成与查询一致的 ClusterORAM
    // 访问，再把低精度槽位与高精度 RingORAM block 留在两层 stash 中。
    // candidate_cluster_ids 必须包含 target_cluster_id；写回由后续 evict_cluster()
    // 与 evict_ring_and_clear_cache() 统一完成。
    DoubleORAMInsertResult insert_ann_vector(
            int target_cluster_id,
            const std::vector<int>& candidate_cluster_ids,
            int vector_id,
            const AnnQuantizedVectorPayload& quantized,
            const std::vector<uint8_t>& exact_payload,
            int ring_dummy_access_count);
    // 兼容旧接口；ClusterORAM 不要求每个簇批次后写回。
    void evict_path();
    // 显式触发 ClusterORAM query-end 写回。
    void evict_cluster();
    // 显式触发 RingORAM 写回，并清空 DoubleORAM 维护的 Ring leaf 临时缓存。
    void evict_ring_and_clear_cache();
    // 只清空 Ring leaf 临时缓存，不触发 RingORAM eviction。
    void clear_ring_leaf_cache();

private:
    // 低精度量化数据所在的 ClusterORAM 实例，不由 DoubleORAM 拥有。
    ClusterORAM* cluster_oram_;
    // 高精度向量数据所在的 RingORAM 实例，不由 DoubleORAM 拥有。
    RingORAM* ring_oram_;
    // DoubleORAM 顶层协议参数。
    DoubleORAMConfig config_;

    // vector_id -> 该向量当前所在的 ClusterORAM stash 量化块位置。
    std::unordered_map<int, PathStashVectorLocation> path_stash_locations_;
    // vector_id -> 最近一次 ClusterORAM 读取返回的 RingORAM leaf。
    std::unordered_map<int, int> cached_ring_leaves_;
    // 保留旧状态位，当前不阻塞连续 ClusterORAM 簇访问。
    bool path_evict_required_;

    // 返回默认量化块布局中单个向量槽位占用的 int 数量。
    int quantized_entry_ints() const;
    // 返回默认量化块布局中指定槽位的起始下标。
    int quantized_entry_base(int slot) const;
    // 返回默认量化块布局中指定槽位 ring_leaf 字段的下标。
    int quantized_entry_ring_leaf_offset(int slot) const;
    // 检查指定槽位是否在量化块 payload 范围内。
    bool quantized_entry_fits(const Block& block, int slot) const;
    // 从量化块槽位中解析低精度量化二进制。
    std::vector<uint8_t> parse_quantized_code(const Block& block, int slot) const;
    // 返回 ANN 批量布局中 block_id 字段所在的下标。
    int ann_block_id_offset() const;
    // 返回 ANN 批量布局中指定槽位 vector_id 字段所在的下标。
    int ann_vector_id_offset(int slot) const;
    // 返回 ANN 批量布局中指定槽位 ring_leaf 字段所在的下标。
    int ann_ring_leaf_offset(int slot) const;
    // 返回 ANN 批量布局中 FastScan payload 的起始下标。
    int ann_payload_offset() const;
    // 检查 ANN 批量布局中指定槽位是否在 block 范围内。
    bool ann_slot_fits(const Block& block, int slot) const;
    // 检查 ANN 批量布局的 payload 是否完整存在于 block 中。
    bool ann_payload_fits(const Block& block) const;
    // 从 ANN 批量布局中解析整块 FastScan payload。
    std::vector<uint8_t> parse_ann_payload(const Block& block) const;
    // 根据当前布局返回指定槽位 ring_leaf 字段所在的下标。
    int ring_leaf_offset_for_slot(int slot) const;
    // 在 ClusterORAM stash 中定位向量所在槽位。
    int find_vector_slot_in_stash_block(const Block& block, int vector_id) const;
    // 将 ANN 批量近似 payload 覆盖写回 Block::data 的 packed 字节区。
    void write_ann_payload(Block& block, const std::vector<uint8_t>& payload) const;
    // 将单向量原始 binary code 写入 ANN payload 中 FastScan 转置后的指定槽位。
    void write_ann_binary_code(
            std::vector<uint8_t>& payload,
            int vector_slot,
            int factor_bytes,
            const std::vector<uint8_t>& binary_code) const;
};

} // namespace double_oram

#endif // DOUBLE_ORAM_DOUBLE_ORAM_H
