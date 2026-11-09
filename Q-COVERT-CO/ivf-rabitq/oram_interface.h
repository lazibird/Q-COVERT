#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

class OramReader {
public:
    virtual ~OramReader() = default;

    virtual std::vector<uint8_t> read(size_t logical_address) = 0;

    virtual std::vector<std::vector<uint8_t>> read_many(const std::vector<size_t>& logical_addresses) {
        std::vector<std::vector<uint8_t>> out;
        out.reserve(logical_addresses.size());
        for (size_t addr : logical_addresses) {
            out.push_back(read(addr));
        }
        return out;
    }

    virtual void end_query() {}
};

class OramWriter {
public:
    virtual ~OramWriter() = default;

    virtual void initialize_block(size_t logical_address, const std::vector<uint8_t>& plain_block) = 0;
    virtual void initialize_path_block(size_t logical_address,
                                       uint32_t cluster_id,
                                       uint32_t block_id,
                                       const std::vector<uint32_t>& vector_ids,
                                       const std::vector<uint32_t>& ring_leaves,
                                       const std::vector<uint8_t>& plain_block) {
        (void)cluster_id;
        (void)block_id;
        (void)vector_ids;
        (void)ring_leaves;
        initialize_block(logical_address, plain_block);
    }
    virtual void initialize_ring_block(uint32_t vector_id, const std::vector<uint8_t>& plain_block) {
        initialize_block(vector_id, plain_block);
    }
    virtual bool ring_leaf_for_vector(uint32_t vector_id, uint32_t& leaf) const {
        (void)vector_id;
        (void)leaf;
        return false;
    }
    virtual void seal() {}
};

struct TwoOramReaders {
    OramReader* approx = nullptr;
    OramReader* exact = nullptr;
};

struct TwoOramWriters {
    OramWriter* approx = nullptr;
    OramWriter* exact = nullptr;
};

struct AnnApproxBlock {
    uint32_t cluster_id = 0;
    uint32_t block_id = 0;
    std::vector<uint32_t> vector_ids;
    std::vector<uint8_t> payload;
};

struct AnnExactVector {
    uint32_t vector_id = 0;
    std::vector<uint8_t> payload;
};

class AnnDoubleOramReader {
public:
    virtual ~AnnDoubleOramReader() = default;

    virtual std::vector<AnnApproxBlock> read_clusters(const std::vector<uint32_t>& cluster_ids) = 0;

    virtual std::vector<AnnApproxBlock> read_cluster(uint32_t cluster_id) {
        return read_clusters({cluster_id});
    }

    virtual std::vector<AnnExactVector> read_exact_vectors(const std::vector<uint32_t>& vector_ids) = 0;

    virtual void finish_clusters() = 0;

    virtual void finish_cluster() {
        finish_clusters();
    }

    virtual void end_query() = 0;
};
