#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "sonic/oram/tree/topology.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::zingoram {

class schedule {
public:
  schedule(const sn::oram::tree::topology& topo, std::uint32_t eviction_count) :
      topo_(topo),
      leaf_count_(topo_.leaf_count()),
      eviction_count_(std::max<std::uint32_t>(1, eviction_count)),
      batch_buffer_(eviction_count_, 0) {}

  sn::util::span<const std::uint64_t> take_evict_leaves() {
    fill_batch(batch_counter_);
    ++batch_counter_;
    return sn::util::span<const std::uint64_t>(batch_buffer_.data(), batch_buffer_.size());
  }

private:
  void fill_batch(std::uint64_t batch) {
    if (batch_buffer_.empty()) {
      return;
    }

    std::uint64_t index = (batch * static_cast<std::uint64_t>(batch_buffer_.size())) % leaf_count_;
    for (auto& leaf : batch_buffer_) {
      leaf = topo_.reverse_lex_leaf(index);
      ++index;
      if (index == leaf_count_) {
        index = 0;
      }
    }
  }

  const sn::oram::tree::topology& topo_;
  std::uint64_t leaf_count_;
  std::uint32_t eviction_count_;
  std::uint64_t batch_counter_{0};
  std::vector<std::uint64_t> batch_buffer_;
};

} // namespace sn::oram::zingoram
