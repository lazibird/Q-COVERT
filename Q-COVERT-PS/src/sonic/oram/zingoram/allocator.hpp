#pragma once

#include <cstdint>
#include <vector>

#include "sonic/oram/storage/block_heap.hpp"
#include "sonic/oram/tree/topology.hpp"
#include "sonic/util/humanize.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::zingoram {

// Owns the contiguous in-memory block slab used by every RingDORAM bucket.
template <typename Block, typename Store> class allocator {
public:
  using block_t = Block;

  struct plan {
    std::uint32_t slot_count = 0;
    std::size_t n_nodes_inclusive = 0;
    std::uint64_t bucket_bytes = 0;
  };

  allocator(
      const sn::oram::tree::topology&, std::uint64_t node_count, std::uint64_t, std::uint32_t slot_count,
      sn::util::log::logger& log
  ) : logger_(log) {
    plan_.slot_count = slot_count;
    plan_.n_nodes_inclusive = static_cast<std::size_t>(node_count + 1ULL);
    plan_.bucket_bytes = static_cast<std::uint64_t>(slot_count) * sizeof(block_t);

    bucket_ptrs_.assign(plan_.n_nodes_inclusive, nullptr);
    constexpr bool store_owns_storage = [] {
      if constexpr (requires { Store::owns_storage; }) return Store::owns_storage;
      else return false;
    }();
    if constexpr (!store_owns_storage) {
      hot_heap_.configure(plan_.n_nodes_inclusive, static_cast<std::size_t>(slot_count));
      for (std::uint64_t node_id = 1; node_id <= node_count; ++node_id) {
        bucket_ptrs_[node_id] = hot_heap_.base_for(static_cast<std::size_t>(node_id));
      }
    }

    logger_.trcf(
        "zingoram::allocator: mode=%s bucket=%s slots=%u nodes=%llu total=%s",
        store_owns_storage ? "external" : "slab",
        sn::util::humanize::bytes(plan_.bucket_bytes), plan_.slot_count,
        static_cast<unsigned long long>(node_count), sn::util::humanize::bytes(node_count * plan_.bucket_bytes)
    );
  }

  [[nodiscard]] const plan& layout() const noexcept { return plan_; }
  [[nodiscard]] const std::vector<block_t*>& bucket_ptrs() const noexcept { return bucket_ptrs_; }

private:
  plan plan_{};
  sn::oram::zingoram::storage::block_heap<block_t> hot_heap_{};
  std::vector<block_t*> bucket_ptrs_{};
  sn::util::log::logger& logger_;
};

} // namespace sn::oram::zingoram
