#pragma once

#include <cstddef>
#include <utility>

#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/stash/core/linear_block_storage.hpp"
#include "sonic/oram/stash/core/section_cursor.hpp"
#include "sonic/util/log.hpp"
#include "sonic/oram/stash/forestzing/config.hpp"

namespace sn::oram::stash::forestzing {

struct subtree_section_sizes {
  std::uint64_t treetop = 0;
  std::uint64_t overlap_region = 0;
  std::uint64_t local_deferred = 0;
  std::uint64_t routed_pathreads = 0;
  std::uint64_t evictslots = 0;
  std::uint64_t fillers = 0;

  [[nodiscard]] std::uint64_t total() const noexcept {
    return treetop + overlap_region + local_deferred + routed_pathreads + evictslots + fillers;
  }
};

namespace log = sn::util::log;

// storage for a subtree's stash
template <typename Block> struct subtree_storage {
  using storage_type = sn::oram::stash::core::linear_block_storage<Block>;
  using section = typename storage_type::section;

  storage_type storage{};
  section treetop{};
  section overlap_region{};
  section local_deferred{};
  section routed_pathreads{};
  section evictslots{};
  section fillers{};
  std::uint64_t evictslots_written = 0;
  std::uint32_t routed_real_count = 0;

  subtree_storage(const subtree_section_sizes& sizes, sn::oram::uid_generator& uid_gen, sn::util::log::logger logger) :
      storage(static_cast<std::size_t>(sizes.total()), uid_gen, std::move(logger)) {
    sn::oram::stash::core::section_layout<Block> layout(storage);
    treetop = layout.take_section(sizes.treetop);
    overlap_region = layout.take_section(sizes.overlap_region);
    local_deferred = layout.take_section(sizes.local_deferred);
    routed_pathreads = layout.take_section(sizes.routed_pathreads);
    evictslots = layout.take_section(sizes.evictslots);
    fillers = layout.take_section(sizes.fillers);
    layout.finish();
    reset_counters();
  }

  void reset_counters() {
    evictslots_written = 0;
    routed_real_count = 0;
  }

  [[nodiscard]] sn::util::span<Block> evictslots_span() { return evictslots.span(storage); }

  [[nodiscard]] sn::util::span<const Block> evictslots_span() const { return evictslots.span(storage); }

  [[nodiscard]] sn::util::span<const Block> used_evictslots_span() const {
    auto full = evictslots.span(storage);
    const std::size_t used = static_cast<std::size_t>(evictslots_written);
    return sn::util::span<const Block>(full.data(), used);
  }
};

} // namespace sn::oram::stash::forestzing
