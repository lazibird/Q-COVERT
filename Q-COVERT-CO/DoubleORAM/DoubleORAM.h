#ifndef DOUBLE_ORAM_DOUBLE_ORAM_H
#define DOUBLE_ORAM_DOUBLE_ORAM_H

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace double_oram {

class Block;
class QuORAM;
class FuORAM;

// Return the number of int slots needed for byte_count bytes, using four bytes per int.
int packed_byte_int_count(int byte_count);
// Pack bytes into int slots in little-endian order.
std::vector<int> pack_bytes_to_ints(const std::vector<uint8_t>& bytes);
// Unpack byte_count bytes from int slots in little-endian order.
std::vector<uint8_t> unpack_bytes_from_ints(const std::vector<int>& words, int word_offset, int byte_count);

// Logical layout of quantized blocks in QuORAM.
enum class DoubleORAMPathBlockLayout {
    // Legacy layout: each vector slot stores vector_id, ring_leaf, and a fixed-length code.
    PerVectorCodes,
    // ANN layout: one block stores metadata for 32 vectors and a full FastScan payload.
    AnnBatch
};

// DoubleORAM protocol parameters for cluster access, quantized layout, and vector dimensions.
struct DoubleORAMConfig {
    // Number of IVF clusters, also the length of the QuORAM cluster counter vector.
    int num_clusters;
    // Maximum vectors per quantized block; 32 in the default configuration.
    int vectors_per_quant_block;
    // Legacy parameter; QuORAM derives paths from actual cluster block counts and offsets.
    int paths_per_cluster;
    // Code bytes per vector in the legacy layout, or full FastScan payload bytes in the ANN layout.
    int quantized_code_bytes;
    // Original vector dimension used to interpret FuORAM full-vector data.
    int vector_dimension;
    // Block::data index containing cluster_id; zero in the default layout.
    int quantized_block_cluster_id_offset;
    // QuORAM quantized block layout.
    DoubleORAMPathBlockLayout path_block_layout;

    // Construct and validate the DoubleORAM protocol parameters.
    DoubleORAMConfig(
            int num_clusters,
            int quantized_code_bytes,
            int vector_dimension,
            int vectors_per_quant_block = 32,
            int paths_per_cluster = 2,
            int quantized_block_cluster_id_offset = 0,
            DoubleORAMPathBlockLayout path_block_layout = DoubleORAMPathBlockLayout::PerVectorCodes);
};

// A quantized vector entry returned by a QuORAM cluster read.
struct QuantizedVectorEntry {
    // Unique vector ID, also the ID of its full-vector block in FuORAM.
    int vector_id;
    // Current FuORAM leaf of this vector's full-vector block.
    int ring_leaf;
    // Fixed-length quantized code of DoubleORAMConfig::quantized_code_bytes bytes.
    std::vector<uint8_t> quantized_code;
};

// Quantized cluster read result; one object per cluster_id.
struct ClusterQuantizedRead {
    // ID of the requested cluster.
    int cluster_id;
    // All quantized vector entries retrieved from the cluster.
    std::vector<QuantizedVectorEntry> vectors;
};

// ANN batch read result for one QuORAM approximate block.
struct AnnApproxBlockRead {
    // IVF cluster ID containing this approximate block.
    int cluster_id;
    // Block ID within the cluster, used to reconstruct the ANN cluster and block access order.
    int block_id;
    // Vector IDs for the block's 32 slots; dummy slots have negative IDs.
    std::vector<int> vector_ids;
    // FuORAM leaves for the block's 32 slots; dummy slots have negative leaves.
    std::vector<int> ring_leaves;
    // Full FastScan payload: 32 Factors followed by 32 packed one-bit codes.
    std::vector<uint8_t> payload;
};

// ANN approximate blocks returned for one cluster_id.
struct AnnClusterApproxRead {
    // ID of the requested cluster.
    int cluster_id;
    // All ANN approximate blocks retrieved from this cluster.
    std::vector<AnnApproxBlockRead> blocks;
};

// Read result for one full vector from FuORAM.
struct RingVectorRead {
    // ID of the requested vector.
    int vector_id;
    // Old FuORAM leaf returned by QuORAM and cached in DoubleORAM before this read.
    int old_leaf;
    // New FuORAM leaf assigned during the read, to be written into the QuORAM stash.
    int new_leaf;
    // Full-vector data, interpreted using FuORAM block_size and the ANN layer.
    std::vector<int> data;
};

// Quantized data for one ApproxOramBlockLayout slot during ANN insertion.
struct AnnQuantizedVectorPayload {
    // Serialized Factor for one vector; four floats in the current IVF-RaBitQ implementation.
    std::vector<uint8_t> factor;
    // Raw one-bit RaBitQ code bytes for one vector, using the same byte order as
    // that vector in IvfRabitQIndex::binary_code. DoubleORAM converts these bytes
    // to the 32-slot transposed FastScan layout in ApproxOramBlockLayout.
    std::vector<uint8_t> binary_code;
};

// Block position and new FuORAM leaf returned after DoubleORAM insertion.
struct DoubleORAMInsertResult {
    // IVF cluster containing the new vector.
    int cluster_id;
    // Offset of the new vector's approximate block within the cluster.
    int cluster_block_id;
    // Slot of the new vector within its approximate block.
    int vector_slot;
    // Initial FuORAM leaf assigned to the new vector.
    int ring_leaf;
};

// Location of a vector within the first-layer QuORAM quantized stash.
struct PathStashVectorLocation {
    // Cluster ID containing the vector.
    int cluster_id;
    // Index of the quantized block within the cluster's stash group.
    int stash_block_index;
    // Slot of the vector within its quantized block.
    int vector_slot;
};

// Coordinator for QuORAM quantized cluster reads and FuORAM full-vector reads.
class DoubleORAM {
public:
    // Bind existing QuORAM and FuORAM instances and store the protocol parameters.
    DoubleORAM(QuORAM& cluster_oram, FuORAM& ring_oram, const DoubleORAMConfig& config);

    // Return the current DoubleORAM parameters.
    const DoubleORAMConfig& config() const;
    // Return the cached vector_id-to-QuORAM-stash locations for testing and debugging.
    const std::unordered_map<int, PathStashVectorLocation>& path_stash_locations() const;
    // Return cached current FuORAM leaves from the latest QuORAM cluster reads.
    const std::unordered_map<int, int>& cached_ring_leaves() const;

    // Read quantized data for one or more clusters and cache their FuORAM leaves.
    std::vector<ClusterQuantizedRead> read_clusters(const std::vector<int>& cluster_ids);
    // Read ANN approximate blocks for one or more clusters and cache their FuORAM leaves.
    std::vector<AnnClusterApproxRead> read_ann_clusters(
            const std::vector<int>& cluster_ids, size_t pad_to_blocks = 0);
    // Read full vectors using cached FuORAM leaves and assign new leaves in DoubleORAM.
    std::vector<RingVectorRead> read_ring_vectors(const std::vector<int>& vector_ids);
    // Update quantized entries in the QuORAM stash with the new FuORAM leaves.
    void update_path_stash_ring_leaves(const std::vector<RingVectorRead>& reads);
    // Insert an ANN vector: first read candidate_cluster_ids using the query access shape,
    // then stage the quantized entry and full-vector block in the two ORAM stashes.
    // candidate_cluster_ids must include target_cluster_id. Later evict_cluster() and
    // evict_ring_and_clear_cache() calls perform writeback for both layers.
    DoubleORAMInsertResult insert_ann_vector(
            int target_cluster_id,
            const std::vector<int>& candidate_cluster_ids,
            int vector_id,
            const AnnQuantizedVectorPayload& quantized,
            const std::vector<uint8_t>& exact_payload,
            int ring_dummy_access_count);
    // Legacy interface; QuORAM does not require writeback after each cluster batch.
    void evict_path();
    // Explicitly trigger query-end QuORAM writeback.
    void evict_cluster();
    // Trigger FuORAM writeback and clear the temporary DoubleORAM full-vector leaf cache.
    void evict_ring_and_clear_cache();
    // Clear the temporary full-vector leaf cache without triggering FuORAM eviction.
    void clear_ring_leaf_cache();

private:
    // QuORAM instance storing quantized data; not owned by DoubleORAM.
    QuORAM* cluster_oram_;
    // FuORAM instance storing full vectors; not owned by DoubleORAM.
    FuORAM* ring_oram_;
    // DoubleORAM protocol parameters.
    DoubleORAMConfig config_;

    // vector_id -> current quantized entry location in the QuORAM stash.
    std::unordered_map<int, PathStashVectorLocation> path_stash_locations_;
    // vector_id -> FuORAM leaf returned by the latest QuORAM read.
    std::unordered_map<int, int> cached_ring_leaves_;
    // Legacy state flag; currently does not prevent consecutive QuORAM cluster accesses.
    bool path_evict_required_;

    // Return int slots per vector entry in the default quantized layout.
    int quantized_entry_ints() const;
    // Return the starting index of a vector slot in the default quantized layout.
    int quantized_entry_base(int slot) const;
    // Return the ring_leaf index for a vector slot in the default layout.
    int quantized_entry_ring_leaf_offset(int slot) const;
    // Check whether a vector slot fits within the quantized block payload.
    bool quantized_entry_fits(const Block& block, int slot) const;
    // Parse a quantized code from a vector slot.
    std::vector<uint8_t> parse_quantized_code(const Block& block, int slot) const;
    // Return the block_id index in the ANN batch layout.
    int ann_block_id_offset() const;
    // Return the vector_id index for a slot in the ANN batch layout.
    int ann_vector_id_offset(int slot) const;
    // Return the ring_leaf index for a slot in the ANN batch layout.
    int ann_ring_leaf_offset(int slot) const;
    // Return the FastScan payload offset in the ANN batch layout.
    int ann_payload_offset() const;
    // Check whether a vector slot fits within the ANN batch block.
    bool ann_slot_fits(const Block& block, int slot) const;
    // Check whether the full ANN batch payload fits within the block.
    bool ann_payload_fits(const Block& block) const;
    // Parse the full FastScan payload from an ANN batch block.
    std::vector<uint8_t> parse_ann_payload(const Block& block) const;
    // Return the ring_leaf index for a slot in the current layout.
    int ring_leaf_offset_for_slot(int slot) const;
    // Locate a vector slot in the QuORAM stash.
    int find_vector_slot_in_stash_block(const Block& block, int vector_id) const;
    // Overwrite the packed payload bytes in Block::data with an ANN approximate payload.
    void write_ann_payload(Block& block, const std::vector<uint8_t>& payload) const;
    // Write a vector's raw binary code into its transposed FastScan slot in the ANN payload.
    void write_ann_binary_code(
            std::vector<uint8_t>& payload,
            int vector_slot,
            int factor_bytes,
            const std::vector<uint8_t>& binary_code) const;
};

} // namespace double_oram

#endif // DOUBLE_ORAM_DOUBLE_ORAM_H
