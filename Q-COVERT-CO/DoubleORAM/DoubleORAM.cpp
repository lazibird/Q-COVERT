#include "DoubleORAM.h"
#include "Block.h"
#include "QuORAM.h"
#include "FuORAM.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace double_oram {

int packed_byte_int_count(int byte_count) {
    if (byte_count < 0) {
        throw std::invalid_argument("byte_count must be non-negative");
    }
    return (byte_count + static_cast<int>(sizeof(int)) - 1) / static_cast<int>(sizeof(int));
}

std::vector<int> pack_bytes_to_ints(const std::vector<uint8_t>& bytes) {
    std::vector<int> words(packed_byte_int_count(static_cast<int>(bytes.size())), 0);
    for (size_t i = 0; i < bytes.size(); i++) {
        size_t word_index = i / sizeof(int);
        size_t byte_index = i % sizeof(int);
        words[word_index] |= static_cast<int>(bytes[i]) << (8 * byte_index);
    }
    return words;
}

std::vector<uint8_t> unpack_bytes_from_ints(const std::vector<int>& words, int word_offset, int byte_count) {
    if (word_offset < 0 || byte_count < 0) {
        throw std::invalid_argument("word_offset and byte_count must be non-negative");
    }
    int word_count = packed_byte_int_count(byte_count);
    if (word_offset > static_cast<int>(words.size()) ||
            word_count > static_cast<int>(words.size()) - word_offset) {
        throw std::out_of_range("packed byte range is outside the int vector");
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(byte_count);
    for (int i = 0; i < byte_count; i++) {
        int word = words[word_offset + i / static_cast<int>(sizeof(int))];
        int shift = 8 * (i % static_cast<int>(sizeof(int)));
        bytes.push_back(static_cast<uint8_t>((word >> shift) & 0xff));
    }
    return bytes;
}

DoubleORAMConfig::DoubleORAMConfig(
        int num_clusters,
        int quantized_code_bytes,
        int vector_dimension,
        int vectors_per_quant_block,
        int paths_per_cluster,
        int quantized_block_cluster_id_offset,
        DoubleORAMPathBlockLayout path_block_layout)
    : num_clusters(num_clusters),
      vectors_per_quant_block(vectors_per_quant_block),
      paths_per_cluster(paths_per_cluster),
      quantized_code_bytes(quantized_code_bytes),
      vector_dimension(vector_dimension),
      quantized_block_cluster_id_offset(quantized_block_cluster_id_offset),
      path_block_layout(path_block_layout) {
    if (num_clusters <= 0) {
        throw std::invalid_argument("num_clusters must be positive");
    }
    if (quantized_code_bytes <= 0) {
        throw std::invalid_argument("quantized_code_bytes must be positive");
    }
    if (vector_dimension <= 0) {
        throw std::invalid_argument("vector_dimension must be positive");
    }
    if (vectors_per_quant_block <= 0) {
        throw std::invalid_argument("vectors_per_quant_block must be positive");
    }
    if (paths_per_cluster <= 0) {
        throw std::invalid_argument("paths_per_cluster must be positive");
    }
    if (quantized_block_cluster_id_offset < 0) {
        throw std::invalid_argument("quantized_block_cluster_id_offset must be non-negative");
    }
}

DoubleORAM::DoubleORAM(QuORAM& cluster_oram, FuORAM& ring_oram, const DoubleORAMConfig& config)
    : cluster_oram_(&cluster_oram),
      ring_oram_(&ring_oram),
      config_(config),
      path_evict_required_(false) {
}

const DoubleORAMConfig& DoubleORAM::config() const {
    return config_;
}

const std::unordered_map<int, PathStashVectorLocation>& DoubleORAM::path_stash_locations() const {
    return path_stash_locations_;
}

const std::unordered_map<int, int>& DoubleORAM::cached_ring_leaves() const {
    return cached_ring_leaves_;
}

std::vector<ClusterQuantizedRead> DoubleORAM::read_clusters(const std::vector<int>& cluster_ids) {
    if (config_.path_block_layout != DoubleORAMPathBlockLayout::PerVectorCodes) {
        throw std::logic_error("read_clusters requires the per-vector QuORAM block layout");
    }
    if (cluster_ids.empty()) {
        return {};
    }

    std::vector<QuORAMClusterRead> path_reads = cluster_oram_->read_clusters_for_double_oram(cluster_ids);
    std::vector<ClusterQuantizedRead> reads;
    reads.reserve(path_reads.size());

    for (const QuORAMClusterRead& path_read : path_reads) {
        std::vector<QuORAMStashBlock> stash_blocks = cluster_oram_->get_cluster_stash(path_read.cluster_id);
        ClusterQuantizedRead cluster_read;
        cluster_read.cluster_id = path_read.cluster_id;

        for (int stash_block_index : path_read.stash_block_indices) {
            if (stash_block_index < 0 || stash_block_index >= static_cast<int>(stash_blocks.size())) {
                throw std::runtime_error("QuORAM returned an invalid DoubleORAM stash block index");
            }

            const Block& block = stash_blocks[stash_block_index].block;
            for (int slot = 0; slot < config_.vectors_per_quant_block; slot++) {
                if (!quantized_entry_fits(block, slot)) {
                    break;
                }

                int base = quantized_entry_base(slot);
                int vector_id = block.data[base];
                if (vector_id < 0) {
                    continue;
                }

                int ring_leaf = block.data[quantized_entry_ring_leaf_offset(slot)];
                if (ring_leaf < 0 || ring_leaf >= ring_oram_->getNumLeaves()) {
                    throw std::runtime_error("DoubleORAM quantized block contains an invalid FuORAM leaf");
                }

                QuantizedVectorEntry entry;
                entry.vector_id = vector_id;
                entry.ring_leaf = ring_leaf;
                entry.quantized_code = parse_quantized_code(block, slot);
                cluster_read.vectors.push_back(entry);

                cached_ring_leaves_[vector_id] = ring_leaf;
                PathStashVectorLocation location;
                location.cluster_id = path_read.cluster_id;
                location.stash_block_index = stash_block_index;
                location.vector_slot = slot;
                path_stash_locations_[vector_id] = location;
            }
        }

        reads.push_back(cluster_read);
    }

    return reads;
}

std::vector<AnnClusterApproxRead> DoubleORAM::read_ann_clusters(
        const std::vector<int>& cluster_ids, size_t pad_to_blocks) {
    if (config_.path_block_layout != DoubleORAMPathBlockLayout::AnnBatch) {
        throw std::logic_error("read_ann_clusters requires the ANN batch QuORAM block layout");
    }
    if (cluster_ids.empty()) {
        return {};
    }

    std::vector<QuORAMClusterRead> path_reads =
        cluster_oram_->read_clusters_for_double_oram(cluster_ids, pad_to_blocks);
    std::vector<AnnClusterApproxRead> reads;
    reads.reserve(path_reads.size());

    for (const QuORAMClusterRead& path_read : path_reads) {
        std::vector<QuORAMStashBlock> stash_blocks = cluster_oram_->get_cluster_stash(path_read.cluster_id);
        AnnClusterApproxRead cluster_read;
        cluster_read.cluster_id = path_read.cluster_id;

        for (int stash_block_index : path_read.stash_block_indices) {
            if (stash_block_index < 0 || stash_block_index >= static_cast<int>(stash_blocks.size())) {
                throw std::runtime_error("QuORAM returned an invalid DoubleORAM stash block index");
            }

            const Block& block = stash_blocks[stash_block_index].block;
            if (!ann_payload_fits(block)) {
                throw std::runtime_error("ANN approx block payload does not fit in the QuORAM block");
            }

            AnnApproxBlockRead block_read;
            block_read.cluster_id = path_read.cluster_id;
            block_read.block_id = block.data[ann_block_id_offset()];
            block_read.vector_ids.reserve(config_.vectors_per_quant_block);
            block_read.ring_leaves.reserve(config_.vectors_per_quant_block);

            for (int slot = 0; slot < config_.vectors_per_quant_block; slot++) {
                if (!ann_slot_fits(block, slot)) {
                    throw std::runtime_error("ANN approx block vector metadata does not fit in the QuORAM block");
                }

                int vector_id = block.data[ann_vector_id_offset(slot)];
                int ring_leaf = block.data[ann_ring_leaf_offset(slot)];
                block_read.vector_ids.push_back(vector_id);
                block_read.ring_leaves.push_back(ring_leaf);

                if (vector_id < 0) {
                    continue;
                }
                if (ring_leaf < 0 || ring_leaf >= ring_oram_->getNumLeaves()) {
                    throw std::runtime_error("ANN approx block contains an invalid FuORAM leaf");
                }

                cached_ring_leaves_[vector_id] = ring_leaf;
                PathStashVectorLocation location;
                location.cluster_id = path_read.cluster_id;
                location.stash_block_index = stash_block_index;
                location.vector_slot = slot;
                path_stash_locations_[vector_id] = location;
            }

            block_read.payload = parse_ann_payload(block);
            cluster_read.blocks.push_back(std::move(block_read));
        }

        std::sort(
            cluster_read.blocks.begin(),
            cluster_read.blocks.end(),
            [](const AnnApproxBlockRead& lhs, const AnnApproxBlockRead& rhs) {
                return lhs.block_id < rhs.block_id;
            });
        reads.push_back(std::move(cluster_read));
    }

    return reads;
}

std::vector<RingVectorRead> DoubleORAM::read_ring_vectors(const std::vector<int>& vector_ids) {
    if (vector_ids.empty()) {
        return {};
    }

    std::vector<int> old_leaves;
    std::vector<int> new_leaves;
    old_leaves.reserve(vector_ids.size());
    new_leaves.reserve(vector_ids.size());
    for (int vector_id : vector_ids) {
        auto leaf_it = cached_ring_leaves_.find(vector_id);
        if (leaf_it == cached_ring_leaves_.end()) {
            throw std::out_of_range("vector_id does not have a cached FuORAM leaf; read its cluster first");
        }
        old_leaves.push_back(leaf_it->second);
        new_leaves.push_back(ring_oram_->rand_gen->getRandomLeafWithBound(ring_oram_->num_leaves));
    }

    std::vector<FuORAMVectorRead> ring_reads =
        ring_oram_->access_without_position_map(
            vector_ids,
            old_leaves,
            new_leaves,
            static_cast<int>(vector_ids.size()));

    std::vector<RingVectorRead> reads;
    reads.reserve(ring_reads.size());
    for (const FuORAMVectorRead& ring_read : ring_reads) {
        RingVectorRead read;
        read.vector_id = ring_read.vector_id;
        read.old_leaf = ring_read.old_leaf;
        read.new_leaf = ring_read.new_leaf;
        read.data = ring_read.data;
        reads.push_back(read);
        cached_ring_leaves_[read.vector_id] = read.new_leaf;
    }

    return reads;
}

void DoubleORAM::update_path_stash_ring_leaves(const std::vector<RingVectorRead>& reads) {
    for (const RingVectorRead& read : reads) {
        auto location_it = path_stash_locations_.find(read.vector_id);
        if (location_it == path_stash_locations_.end()) {
            throw std::out_of_range("vector_id does not have a cached QuORAM stash location");
        }

        const PathStashVectorLocation& location = location_it->second;
        int ring_leaf_offset = ring_leaf_offset_for_slot(location.vector_slot);
        cluster_oram_->update_cluster_stash_value(
            location.cluster_id,
            location.stash_block_index,
            ring_leaf_offset,
            read.new_leaf);
        cached_ring_leaves_[read.vector_id] = read.new_leaf;
    }
}

DoubleORAMInsertResult DoubleORAM::insert_ann_vector(
        int target_cluster_id,
        const std::vector<int>& candidate_cluster_ids,
        int vector_id,
        const AnnQuantizedVectorPayload& quantized,
        const std::vector<uint8_t>& exact_payload,
        int ring_dummy_access_count) {
    if (config_.path_block_layout != DoubleORAMPathBlockLayout::AnnBatch) {
        throw std::logic_error("insert_ann_vector requires the ANN batch QuORAM block layout");
    }
    if (target_cluster_id < 0 || target_cluster_id >= config_.num_clusters) {
        throw std::out_of_range("target_cluster_id is outside DoubleORAM range");
    }
    if (vector_id < 0 || vector_id >= ring_oram_->num_blocks) {
        throw std::out_of_range("new vector_id is outside the reserved FuORAM block-id capacity");
    }
    if (ring_dummy_access_count < 0) {
        throw std::invalid_argument("ring_dummy_access_count must be non-negative");
    }
    if (candidate_cluster_ids.empty() ||
            std::find(candidate_cluster_ids.begin(), candidate_cluster_ids.end(), target_cluster_id) ==
                candidate_cluster_ids.end()) {
        throw std::invalid_argument("candidate_cluster_ids must include target_cluster_id");
    }
    if (ring_oram_->contains_block(vector_id)) {
        throw std::runtime_error("new vector_id already exists in FuORAM");
    }

    const int vectors_per_block = config_.vectors_per_quant_block;
    const size_t per_vector_payload = quantized.factor.size() + quantized.binary_code.size();
    if (per_vector_payload == 0 ||
            per_vector_payload * static_cast<size_t>(vectors_per_block) !=
                static_cast<size_t>(config_.quantized_code_bytes)) {
        throw std::invalid_argument(
            "ANN factor/binary-code bytes do not match the configured approximate block layout");
    }
    if (quantized.binary_code.empty() || quantized.binary_code.size() % sizeof(uint64_t) != 0) {
        throw std::invalid_argument("ANN binary code must contain a whole number of uint64 words");
    }

    // This performs the same QuORAM path accesses as one query batch and
    // leaves every accessed approximate block in the cluster-organized stash.
    read_ann_clusters(candidate_cluster_ids);

    int cluster_block_id = -1;
    int vector_slot = -1;
    const int existing_block_count = cluster_oram_->cluster_block_counts[target_cluster_id];
    if (existing_block_count > 0) {
        cluster_block_id = existing_block_count - 1;
        const QuORAMStashBlock last_block =
            cluster_oram_->get_cluster_stash_block(target_cluster_id, cluster_block_id);
        if (!ann_payload_fits(last_block.block)) {
            throw std::runtime_error("last QuORAM ANN block has an invalid payload layout");
        }
        for (int slot = 0; slot < vectors_per_block; ++slot) {
            if (last_block.block.data[ann_vector_id_offset(slot)] < 0) {
                vector_slot = slot;
                break;
            }
        }
    }

    if (vector_slot < 0 && !cluster_oram_->can_append_cluster_block()) {
        throw std::runtime_error("QuORAM capacity is exhausted; rebuild the generation before inserting");
    }

    // Reserve the FuORAM block before mutating the approximate block. All
    // following cluster mutations are range-checked and cannot fail due to
    // capacity after the check above, which avoids an observable partial insert.
    std::vector<int> ring_data(ring_oram_->block_size, 0);
    const std::vector<int> packed_exact = pack_bytes_to_ints(exact_payload);
    if (static_cast<int>(packed_exact.size()) > ring_oram_->block_size) {
        throw std::invalid_argument("exact payload exceeds FuORAM block_size");
    }
    std::copy(packed_exact.begin(), packed_exact.end(), ring_data.begin());
    const int ring_leaf = ring_oram_->insert_block_to_stash(
        vector_id, ring_data, ring_dummy_access_count);

    if (vector_slot < 0) {
        std::vector<int> new_block_data(cluster_oram_->block_size, 0);
        new_block_data[config_.quantized_block_cluster_id_offset] = target_cluster_id;
        new_block_data[ann_block_id_offset()] = existing_block_count;
        for (int slot = 0; slot < vectors_per_block; ++slot) {
            new_block_data[ann_vector_id_offset(slot)] = -1;
            new_block_data[ann_ring_leaf_offset(slot)] = -1;
        }
        cluster_block_id = cluster_oram_->append_cluster_block_to_stash(
            target_cluster_id, new_block_data);
        vector_slot = 0;
    }

    QuORAMStashBlock target_block =
        cluster_oram_->get_cluster_stash_block(target_cluster_id, cluster_block_id);
    if (!ann_payload_fits(target_block.block) ||
            target_block.block.data[ann_vector_id_offset(vector_slot)] >= 0) {
        throw std::runtime_error("target QuORAM ANN insertion slot is not available");
    }
    std::vector<uint8_t> approx_payload = parse_ann_payload(target_block.block);
    const size_t factor_bytes = quantized.factor.size();
    const size_t code_bytes = quantized.binary_code.size();
    const size_t factor_offset = static_cast<size_t>(vector_slot) * factor_bytes;
    std::copy(quantized.factor.begin(), quantized.factor.end(), approx_payload.begin() + factor_offset);
    write_ann_binary_code(
        approx_payload,
        vector_slot,
        static_cast<int>(factor_bytes),
        quantized.binary_code);
    target_block.block.data[ann_vector_id_offset(vector_slot)] = vector_id;
    target_block.block.data[ann_ring_leaf_offset(vector_slot)] = ring_leaf;
    write_ann_payload(target_block.block, approx_payload);
    cluster_oram_->replace_cluster_stash_block_data(
        target_cluster_id, cluster_block_id, target_block.block.data);

    const int stash_block_index =
        cluster_oram_->cluster_stash_index(target_cluster_id, cluster_block_id);
    if (stash_block_index < 0) {
        throw std::runtime_error("new ANN vector block unexpectedly left QuORAM stash");
    }
    cached_ring_leaves_[vector_id] = ring_leaf;
    path_stash_locations_[vector_id] = {
        target_cluster_id,
        stash_block_index,
        vector_slot,
    };
    return {target_cluster_id, cluster_block_id, vector_slot, ring_leaf};
}

void DoubleORAM::evict_path() {
    path_stash_locations_.clear();
    path_evict_required_ = false;
}

void DoubleORAM::evict_cluster() {
    cluster_oram_->evict_and_write_back();
    path_stash_locations_.clear();
    path_evict_required_ = false;
}

void DoubleORAM::evict_ring_and_clear_cache() {
    ring_oram_->evict_and_write_back();
    clear_ring_leaf_cache();
}

void DoubleORAM::clear_ring_leaf_cache() {
    cached_ring_leaves_.clear();
    ring_oram_->clear_double_oram_leaf_cache();
}

int DoubleORAM::quantized_entry_ints() const {
    if (config_.quantized_code_bytes > std::numeric_limits<int>::max() - 2) {
        throw std::overflow_error("quantized_code_bytes is too large");
    }
    return 2 + config_.quantized_code_bytes;
}

int DoubleORAM::quantized_entry_base(int slot) const {
    if (slot < 0 || slot >= config_.vectors_per_quant_block) {
        throw std::out_of_range("quantized vector slot is outside the configured range");
    }

    int entry_ints = quantized_entry_ints();
    if (slot > (std::numeric_limits<int>::max() - config_.quantized_block_cluster_id_offset - 1) / entry_ints) {
        throw std::overflow_error("quantized vector slot offset is too large");
    }
    return config_.quantized_block_cluster_id_offset + 1 + slot * entry_ints;
}

int DoubleORAM::quantized_entry_ring_leaf_offset(int slot) const {
    int base = quantized_entry_base(slot);
    if (base == std::numeric_limits<int>::max()) {
        throw std::overflow_error("ring leaf offset is too large");
    }
    return base + 1;
}

bool DoubleORAM::quantized_entry_fits(const Block& block, int slot) const {
    int base = quantized_entry_base(slot);
    int end = base + quantized_entry_ints();
    return end <= block.block_size && end <= static_cast<int>(block.data.size());
}

std::vector<uint8_t> DoubleORAM::parse_quantized_code(const Block& block, int slot) const {
    if (!quantized_entry_fits(block, slot)) {
        throw std::out_of_range("quantized vector slot is outside the block payload");
    }

    std::vector<uint8_t> code;
    code.reserve(config_.quantized_code_bytes);
    int base = quantized_entry_base(slot);
    for (int i = 0; i < config_.quantized_code_bytes; i++) {
        int value = block.data[base + 2 + i];
        if (value < 0 || value > 255) {
            throw std::runtime_error("quantized code byte is outside [0, 255]");
        }
        code.push_back(static_cast<uint8_t>(value));
    }
    return code;
}

int DoubleORAM::ann_block_id_offset() const {
    if (config_.quantized_block_cluster_id_offset == std::numeric_limits<int>::max()) {
        throw std::overflow_error("ANN block_id offset is too large");
    }
    return config_.quantized_block_cluster_id_offset + 1;
}

int DoubleORAM::ann_vector_id_offset(int slot) const {
    if (slot < 0 || slot >= config_.vectors_per_quant_block) {
        throw std::out_of_range("ANN vector slot is outside the configured range");
    }
    int base = ann_block_id_offset() + 1;
    if (slot > std::numeric_limits<int>::max() - base) {
        throw std::overflow_error("ANN vector_id offset is too large");
    }
    return base + slot;
}

int DoubleORAM::ann_ring_leaf_offset(int slot) const {
    if (slot < 0 || slot >= config_.vectors_per_quant_block) {
        throw std::out_of_range("ANN vector slot is outside the configured range");
    }
    int base = ann_block_id_offset() + 1 + config_.vectors_per_quant_block;
    if (slot > std::numeric_limits<int>::max() - base) {
        throw std::overflow_error("ANN ring_leaf offset is too large");
    }
    return base + slot;
}

int DoubleORAM::ann_payload_offset() const {
    int block_id_offset = ann_block_id_offset();
    if (config_.vectors_per_quant_block >
            (std::numeric_limits<int>::max() - block_id_offset - 1) / 2) {
        throw std::overflow_error("ANN payload offset is too large");
    }
    return block_id_offset + 1 + 2 * config_.vectors_per_quant_block;
}

bool DoubleORAM::ann_slot_fits(const Block& block, int slot) const {
    int vector_id_offset = ann_vector_id_offset(slot);
    int ring_leaf_offset = ann_ring_leaf_offset(slot);
    return vector_id_offset < block.block_size &&
           ring_leaf_offset < block.block_size &&
           vector_id_offset < static_cast<int>(block.data.size()) &&
           ring_leaf_offset < static_cast<int>(block.data.size());
}

bool DoubleORAM::ann_payload_fits(const Block& block) const {
    int offset = ann_payload_offset();
    int payload_words = packed_byte_int_count(config_.quantized_code_bytes);
    if (payload_words > std::numeric_limits<int>::max() - offset) {
        throw std::overflow_error("ANN payload end offset is too large");
    }
    int end = offset + payload_words;
    return ann_block_id_offset() < block.block_size &&
           ann_block_id_offset() < static_cast<int>(block.data.size()) &&
           end <= block.block_size &&
           end <= static_cast<int>(block.data.size());
}

std::vector<uint8_t> DoubleORAM::parse_ann_payload(const Block& block) const {
    if (!ann_payload_fits(block)) {
        throw std::out_of_range("ANN approx payload is outside the block payload");
    }

    return unpack_bytes_from_ints(block.data, ann_payload_offset(), config_.quantized_code_bytes);
}

void DoubleORAM::write_ann_payload(Block& block, const std::vector<uint8_t>& payload) const {
    if (!ann_payload_fits(block)) {
        throw std::out_of_range("ANN approx payload is outside the block payload");
    }
    if (payload.size() != static_cast<size_t>(config_.quantized_code_bytes)) {
        throw std::invalid_argument("ANN approx payload has the wrong byte length");
    }
    const std::vector<int> packed = pack_bytes_to_ints(payload);
    const int offset = ann_payload_offset();
    std::copy(packed.begin(), packed.end(), block.data.begin() + offset);
}

void DoubleORAM::write_ann_binary_code(
        std::vector<uint8_t>& payload,
        int vector_slot,
        int factor_bytes,
        const std::vector<uint8_t>& binary_code) const {
    if (vector_slot < 0 || vector_slot >= config_.vectors_per_quant_block ||
            config_.vectors_per_quant_block != 32) {
        throw std::invalid_argument("ANN FastScan code insertion requires a 32-vector block");
    }
    if (factor_bytes < 0 || binary_code.empty() || binary_code.size() % sizeof(uint64_t) != 0) {
        throw std::invalid_argument("ANN binary code layout is invalid");
    }
    const size_t expected_size =
        static_cast<size_t>(config_.vectors_per_quant_block) *
        (static_cast<size_t>(factor_bytes) + binary_code.size());
    if (payload.size() != expected_size) {
        throw std::invalid_argument("ANN payload does not match its factor/binary-code layout");
    }

    // This is the inverse addressing of pack_codes_binary()/pack_codes_bytes().
    // It updates only the target vector's nibbles, preserving codes already
    // present in the other 31 slots of the FastScan block.
    constexpr int kPerm0[16] = {
        0, 8, 1, 9, 2, 10, 3, 11,
        4, 12, 5, 13, 6, 14, 7, 15,
    };
    const int lane = vector_slot % 16;
    int packed_index = -1;
    for (int i = 0; i < 16; ++i) {
        if (kPerm0[i] == lane) {
            packed_index = i;
            break;
        }
    }
    if (packed_index < 0) {
        throw std::logic_error("ANN FastScan permutation does not contain the vector slot");
    }

    std::vector<uint8_t> transformed = binary_code;
    for (size_t word = 0; word < transformed.size(); word += sizeof(uint64_t)) {
        for (size_t byte = 0; byte < sizeof(uint64_t) / 2; ++byte) {
            std::swap(transformed[word + byte], transformed[word + sizeof(uint64_t) - 1 - byte]);
        }
    }
    for (uint8_t& value : transformed) {
        value = static_cast<uint8_t>((value << 4) | (value >> 4));
    }

    const size_t packed_base =
        static_cast<size_t>(config_.vectors_per_quant_block) * static_cast<size_t>(factor_bytes);
    for (size_t byte = 0; byte < transformed.size(); ++byte) {
        const uint8_t low_nibble = static_cast<uint8_t>(transformed[byte] & 0x0f);
        const uint8_t high_nibble = static_cast<uint8_t>(transformed[byte] >> 4);
        const size_t column_base = packed_base + byte * 32;
        uint8_t& low_destination = payload[column_base + static_cast<size_t>(packed_index)];
        uint8_t& high_destination = payload[column_base + 16 + static_cast<size_t>(packed_index)];
        if (vector_slot < 16) {
            low_destination = static_cast<uint8_t>((low_destination & 0xf0) | low_nibble);
            high_destination = static_cast<uint8_t>((high_destination & 0xf0) | high_nibble);
        } else {
            low_destination = static_cast<uint8_t>((low_destination & 0x0f) | (low_nibble << 4));
            high_destination = static_cast<uint8_t>((high_destination & 0x0f) | (high_nibble << 4));
        }
    }
}

int DoubleORAM::ring_leaf_offset_for_slot(int slot) const {
    if (config_.path_block_layout == DoubleORAMPathBlockLayout::AnnBatch) {
        return ann_ring_leaf_offset(slot);
    }
    return quantized_entry_ring_leaf_offset(slot);
}

int DoubleORAM::find_vector_slot_in_stash_block(const Block& block, int vector_id) const {
    for (int slot = 0; slot < config_.vectors_per_quant_block; slot++) {
        if (config_.path_block_layout == DoubleORAMPathBlockLayout::AnnBatch) {
            if (!ann_slot_fits(block, slot)) {
                break;
            }
            if (block.data[ann_vector_id_offset(slot)] == vector_id) {
                return slot;
            }
            continue;
        }

        if (!quantized_entry_fits(block, slot)) {
            break;
        }
        if (block.data[quantized_entry_base(slot)] == vector_id) {
            return slot;
        }
    }
    return -1;
}

} // namespace double_oram
