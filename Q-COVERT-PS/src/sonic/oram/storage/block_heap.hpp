#pragma once

#include <cstdint>
#include <vector>

#include "sonic/util/span.hpp"

namespace sn::oram::zingoram::storage {

// raii owner for a contiguous slab of blocks covering all buckets
template <typename Block> class block_heap {
public:
  using block_t = Block;

  block_heap() = default;

  void configure(std::size_t node_count_inclusive, std::size_t slots_per_bucket) {
    slots_per_bucket_ = slots_per_bucket;
    const std::size_t total_slots = node_count_inclusive * slots_per_bucket_;
    storage_.assign(total_slots, block_t{});
  }

  [[nodiscard]] block_t* base_for(std::size_t node_id) noexcept {
    const std::size_t offset = node_id * slots_per_bucket_;
    return storage_.data() + offset;
  }

  [[nodiscard]] block_t* base_for_index(std::size_t index) noexcept {
    const std::size_t offset = index * slots_per_bucket_;
    return storage_.data() + offset;
  }

  [[nodiscard]] sn::util::span<block_t> span_for(std::size_t node_id) noexcept {
    return sn::util::span<block_t>(base_for(node_id), slots_per_bucket_);
  }

  [[nodiscard]] std::size_t slots_per_bucket() const noexcept { return slots_per_bucket_; }

private:
  std::vector<block_t> storage_;
  std::size_t slots_per_bucket_ = 0;
};

} // namespace sn::oram::zingoram::storage
