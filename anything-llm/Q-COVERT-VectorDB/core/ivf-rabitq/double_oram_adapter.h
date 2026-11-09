#pragma once

#include <cstdint>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <vector>

#include "DoubleORAM.h"
#include "oram_interface.h"

class DoubleOramAnnReader : public AnnDoubleOramReader {
public:
    DoubleOramAnnReader(
            double_oram::DoubleORAM& double_oram,
            int exact_payload_bytes,
            const std::unordered_set<uint32_t>* live_vector_ids = nullptr)
        : double_oram_(&double_oram),
          exact_payload_bytes_(exact_payload_bytes),
          live_vector_ids_(live_vector_ids),
          cluster_open_(false) {
        if (exact_payload_bytes_ <= 0) {
            throw std::invalid_argument("exact_payload_bytes must be positive");
        }
    }

    std::vector<AnnApproxBlock> read_clusters(const std::vector<uint32_t>& cluster_ids) override {
        if (cluster_ids.empty()) {
            return {};
        }

        std::vector<int> ids;
        ids.reserve(cluster_ids.size());
        for (uint32_t cluster_id : cluster_ids) {
            ids.push_back(static_cast<int>(cluster_id));
        }
        std::vector<double_oram::AnnClusterApproxRead> cluster_reads =
            double_oram_->read_ann_clusters(ids);
        if (cluster_reads.size() != cluster_ids.size()) {
            throw std::runtime_error("DoubleORAM returned an unexpected ANN cluster batch size");
        }

        cluster_open_ = true;

        std::vector<AnnApproxBlock> out;
        for (size_t i = 0; i < cluster_reads.size(); ++i) {
            if (cluster_reads[i].cluster_id != static_cast<int>(cluster_ids[i])) {
                throw std::runtime_error("DoubleORAM returned ANN clusters out of request order");
            }
            out.reserve(out.size() + cluster_reads[i].blocks.size());
            for (const double_oram::AnnApproxBlockRead& block_read : cluster_reads[i].blocks) {
                AnnApproxBlock block;
                block.cluster_id = static_cast<uint32_t>(block_read.cluster_id);
                block.block_id = static_cast<uint32_t>(block_read.block_id);
                block.payload = block_read.payload;
                block.vector_ids.reserve(block_read.vector_ids.size());
                for (int vector_id : block_read.vector_ids) {
                    if (vector_id < 0 ||
                        (live_vector_ids_ != nullptr &&
                         live_vector_ids_->find(static_cast<uint32_t>(vector_id)) ==
                             live_vector_ids_->end())) {
                        block.vector_ids.push_back(kDummyVectorId);
                    } else {
                        block.vector_ids.push_back(static_cast<uint32_t>(vector_id));
                    }
                }
                out.push_back(std::move(block));
            }
        }
        return out;
    }

    std::vector<AnnExactVector> read_exact_vectors(const std::vector<uint32_t>& vector_ids) override {
        if (!cluster_open_) {
            throw std::logic_error("read_clusters must be called before reading ANN exact vectors");
        }
        if (vector_ids.empty()) {
            return {};
        }

        std::vector<int> ids;
        ids.reserve(vector_ids.size());
        for (uint32_t vector_id : vector_ids) {
            if (vector_id == kDummyVectorId) {
                throw std::invalid_argument("dummy vector id cannot be read from RingORAM");
            }
            ids.push_back(static_cast<int>(vector_id));
        }

        std::vector<double_oram::RingVectorRead> ring_reads = double_oram_->read_ring_vectors(ids);
        std::vector<AnnExactVector> out;
        out.reserve(ring_reads.size());
        for (const double_oram::RingVectorRead& ring_read : ring_reads) {
            pending_ring_updates_.push_back(ring_read);

            AnnExactVector exact;
            exact.vector_id = static_cast<uint32_t>(ring_read.vector_id);
            exact.payload = double_oram::unpack_bytes_from_ints(ring_read.data, 0, exact_payload_bytes_);
            out.push_back(std::move(exact));
        }
        return out;
    }

    void finish_clusters() override {
        if (!cluster_open_) {
            return;
        }

        if (!pending_ring_updates_.empty()) {
            double_oram_->update_path_stash_ring_leaves(pending_ring_updates_);
            pending_ring_updates_.clear();
        }
        cluster_open_ = false;
    }

    void end_query() override {
        if (cluster_open_) {
            finish_clusters();
        }
        double_oram_->evict_cluster();
        double_oram_->evict_ring_and_clear_cache();
    }

private:
    static constexpr uint32_t kDummyVectorId = static_cast<uint32_t>(-1);

    double_oram::DoubleORAM* double_oram_;
    int exact_payload_bytes_;
    const std::unordered_set<uint32_t>* live_vector_ids_;
    bool cluster_open_;
    std::vector<double_oram::RingVectorRead> pending_ring_updates_;
};
