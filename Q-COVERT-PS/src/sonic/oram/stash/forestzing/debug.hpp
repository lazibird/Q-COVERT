#pragma once

#if defined(ORAM_DEBUG)

#include <cstddef>
#include <cstdint>
#include <limits>

#include "sonic/oram/stash/forestzing/state.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::stash::forestzing::pipeline {
template <typename Block> std::int64_t target_path(const Block& block) noexcept;
template <typename Block> std::int64_t target_depth(const Block& block) noexcept;
struct population;
} // namespace sn::oram::stash::forestzing::pipeline

namespace sn::oram::stash::forestzing::pipeline::debug {

template <typename Block>
inline void log_sorted_storage(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix
) {
  auto span = subtree.storage.span();
  state.log.dbgf(
      "forestzing::pipeline::target_coord_group_sort: subtree=%d sorted storage (count=%d)", subtree_ix,
      static_cast<std::uint64_t>(span.size())
  );

  for (std::size_t ix = 0; ix < span.size(); ++ix) {
    Block& block = span[ix];
    const std::int64_t path = target_path(block);
    const std::int64_t depth = target_depth(block);
    const bool is_real = block.is_real().unwrap();

    if (is_real) {
      state.log.pedf(
          "  storage[%03zu]: real block#%d path=%d depth=%d (address=$%08x leaf_ix=%d)", ix, block.uid, path, depth,
          block.address, block.leaf_ix
      );
    } else {
      state.log.pedf("  storage[%03zu]: dummy block#%d path=%d depth=%d", ix, block.uid, path, depth);
    }

    sn::util::log::ensure(
        state.log, path >= 0 && path < static_cast<std::int64_t>(geom.evict_batch),
        "forestzing::pipeline::target_coord_group_sort: path index out of bounds"
    );
    sn::util::log::ensure(
        state.log, depth >= 0 && depth <= static_cast<std::int64_t>(geom.subtree_height),
        "forestzing::pipeline::target_coord_group_sort: depth out of bounds"
    );
  }
}

template <typename Block>
inline void verify_sorted_storage(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix
) {
  auto span = subtree.storage.span();

  std::int64_t prev_path = std::numeric_limits<std::int64_t>::min();
  std::int64_t prev_depth = std::numeric_limits<std::int64_t>::min();
  bool saw_dummy_at_level = false;
  bool first = true;

  for (std::size_t ix = 0; ix < span.size(); ++ix) {
    Block& block = span[ix];
    const std::int64_t path = target_path(block);
    const std::int64_t depth = target_depth(block);
    const bool is_real = block.is_real().unwrap();

    if (first) {
      prev_path = path;
      prev_depth = depth;
      saw_dummy_at_level = !is_real;
      first = false;
      continue;
    }

    if (path < prev_path) {
      sn::util::log::failf(
          state.log,
          "forestzing::pipeline::target_coord_group_sort: path order violated (prev=%d current=%d) in subtree %d",
          static_cast<int>(prev_path), static_cast<int>(path), subtree_ix
      );
    }

    if (path > prev_path) {
      prev_path = path;
      prev_depth = depth;
      saw_dummy_at_level = !is_real;
      continue;
    }

    if (depth > prev_depth) {
      sn::util::log::failf(
          state.log,
          "forestzing::pipeline::target_coord_group_sort: depth order violated at path %d (prev=%d current=%d)",
          static_cast<int>(path), static_cast<int>(prev_depth), static_cast<int>(depth)
      );
    }

    if (depth < prev_depth) {
      prev_depth = depth;
      saw_dummy_at_level = !is_real;
      continue;
    }

    if (is_real && saw_dummy_at_level) {
      sn::util::log::failf(
          state.log,
          "forestzing::pipeline::target_coord_group_sort: real block appears after dummy at path=%d depth=%d",
          static_cast<int>(path), static_cast<int>(depth)
      );
    }

    if (!is_real) {
      saw_dummy_at_level = true;
    }
  }
}

template <typename Block>
inline void log_postprocess_iteration(
    stash_state<Block>& state, std::uint32_t subtree_ix, std::size_t ix, std::int64_t path, std::int64_t depth,
    const sn::obliv::choice& keep_evicted_real, const sn::obliv::choice& keep_evicted_dummy,
    const sn::obliv::choice& keep_retained_real, const sn::obliv::choice& drop_block,
    const sn::obliv::choice& keep_evicted, std::int64_t waterline_depth, std::uint64_t waterline_fill,
    std::uint64_t path_evicted
) {
  state.log.pedf(
      "  storage[%03zu]: subtree=%d path=%d depth=%d ev_real=%d ev_dummy=%d retain=%d drop=%d wl_depth=%d wl_fill=%d "
      "path_ev=%d",
      ix, subtree_ix, static_cast<int>(path), static_cast<int>(depth), keep_evicted_real.unwrap(),
      keep_evicted_dummy.unwrap(), keep_retained_real.unwrap(), drop_block.unwrap(), static_cast<int>(waterline_depth),
      static_cast<int>(waterline_fill), static_cast<int>(path_evicted)
  );
  sn::util::log::ensure(
      state.log, (keep_evicted.unwrap() + keep_retained_real.unwrap() + drop_block.unwrap()) == 1,
      "forestzing::pipeline::postprocess_multipath: inconsistent keep/drop flags"
  );
  sn::util::log::ensure(
      state.log, !keep_evicted.unwrap() || waterline_depth >= 0,
      "forestzing::pipeline::postprocess_multipath: negative waterline depth for kept block"
  );
}

template <typename Block>
inline void verify_postprocess(
    stash_state<Block>& state, const stash_geometry& geom, std::uint32_t subtree_ix, const population& pop,
    std::int64_t waterline_depth, std::uint64_t waterline_fill, std::uint64_t path_evicted, std::uint64_t total_capacity
) {
  const std::uint64_t path_capacity =
      (static_cast<std::uint64_t>(geom.subtree_height) + 1ULL) * static_cast<std::uint64_t>(geom.bucket_real);
  const std::uint64_t retained_capacity =
      (geom.limits.max_real_blocks > total_capacity) ? (geom.limits.max_real_blocks - total_capacity) : 0ULL;

  sn::util::log::ensure(
      state.log, pop.evicted_total == total_capacity,
      "forestzing::pipeline::postprocess_multipath: evicted block count mismatch"
  );
  sn::util::log::ensure(
      state.log, path_evicted == path_capacity,
      "forestzing::pipeline::postprocess_multipath: final path evicted count mismatch"
  );
  sn::util::log::ensure(
      state.log, waterline_depth == static_cast<std::int64_t>(-1),
      "forestzing::pipeline::postprocess_multipath: final waterline depth invalid"
  );
  sn::util::log::ensure(
      state.log, waterline_fill == 0, "forestzing::pipeline::postprocess_multipath: final waterline fullness invalid"
  );
  sn::util::log::ensure(
      state.log, pop.evicted_real <= pop.evicted_total,
      "forestzing::pipeline::postprocess_multipath: evicted real exceeds total"
  );
  sn::util::log::ensure(
      state.log, pop.retained_real <= retained_capacity,
      "forestzing::pipeline::postprocess_multipath: retained real exceeds capacity"
  );
}

template <typename Block>
inline void validate_compact_layout(
    stash_state<Block>& state, subtree_storage<Block>& subtree, std::uint32_t subtree_ix, std::uint64_t evicted_total,
    std::uint64_t retained_real, std::uint64_t retained_capacity_ub, std::uint64_t kept_count_ub
) {
  auto span = subtree.storage.span();
  std::uint64_t retained_real_count = 0;
  for (std::size_t ix = 0; ix < span.size(); ++ix) {
    const Block& block = span[ix];
    const std::int64_t depth = target_depth(block);
    const bool in_evicted = ix < evicted_total;
    const bool in_retained_region = ix >= evicted_total && ix < evicted_total + retained_capacity_ub;
    const bool in_prefix = ix < kept_count_ub;

    if (in_evicted) {
      sn::util::log::ensure(
          state.log, depth >= 0,
          "forestzing::pipeline::compact_kept_blocks: evicted section contains sentinel depth block"
      );
    } else if (in_retained_region) {
      sn::util::log::ensure(
          state.log, depth == -1,
          "forestzing::pipeline::compact_kept_blocks: retained section contains non-sentinel depth block"
      );
      retained_real_count += static_cast<std::uint64_t>(block.is_real().unwrap());
    } else if (in_prefix) {
      sn::util::log::ensure(
          state.log, depth <= -1,
          "forestzing::pipeline::compact_kept_blocks: garbage prefix contains non-sentinel depth block"
      );
    }
  }
  sn::util::log::ensure(
      state.log, retained_real_count == retained_real,
      "forestzing::pipeline::compact_kept_blocks: retained real count mismatch"
  );
  state.log.dbgf(
      "forestzing::pipeline::compact_kept_blocks: layout validated for subtree=%d (evicted=%d retained=%d)", subtree_ix,
      evicted_total, retained_real
  );
}

template <typename Block>
inline void log_compact_layout(
    stash_state<Block>& state, subtree_storage<Block>& subtree, std::uint32_t subtree_ix, std::uint64_t evicted_total,
    std::uint64_t retained_real, std::uint64_t retained_capacity_ub, std::uint64_t kept_count_ub
) {
  auto span = subtree.storage.span();
  state.log.dbgf(
      "forestzing::pipeline::compact_kept_blocks: compacted layout subtree=%d evicted=%d retained=%d capacity=%d "
      "kept_ub=%d",
      subtree_ix, evicted_total, retained_real, retained_capacity_ub, kept_count_ub
  );

  const std::uint64_t retained_end = evicted_total + retained_capacity_ub;
  for (std::size_t ix = 0; ix < span.size() && ix < kept_count_ub; ++ix) {
    const Block& block = span[ix];
    const std::int64_t path = target_path(block);
    const std::int64_t depth = target_depth(block);
    const bool is_real = block.is_real().unwrap();
    const char* region = (ix < evicted_total) ? "evict" : (ix < retained_end ? "retain" : "garbage");
    const char* type = is_real ? "real" : "dummy";

    if (is_real) {
      state.log.pedf(
          "  compact[%03zu]: %s %s block#%d path=%d depth=%d (address=$%08x leaf_ix=%d)", ix, region, type, block.uid,
          static_cast<int>(path), static_cast<int>(depth), block.address, block.leaf_ix
      );
    } else {
      state.log.pedf(
          "  compact[%03zu]: %s %s block#%d path=%d depth=%d", ix, region, type, block.uid, static_cast<int>(path),
          static_cast<int>(depth)
      );
    }
  }
}

template <typename Block>
inline void log_final_subtree_layout(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix,
    const population& pop
) {
  const auto treetop_span = subtree.treetop.span(subtree.storage);
  const auto overlap_span = subtree.overlap_region.span(subtree.storage);
  const auto local_deferred_span = subtree.local_deferred.span(subtree.storage);
  const auto routed_span = subtree.routed_pathreads.span(subtree.storage);
  const auto evictslots_span = subtree.evictslots.span(subtree.storage);
  const auto fillers_span = subtree.fillers.span(subtree.storage);
  (void) geom;

  state.log.dbgf(
      "forestzing::pipeline::evict_subtree: final stash layout subtree=%d (evicted_total=%d evicted_real=%d "
      "retained_real=%d)",
      subtree_ix, pop.evicted_total, pop.evicted_real, pop.retained_real
  );

  auto log_section = [&](const char* label, const auto& span, std::size_t offset, std::size_t used) {
    std::size_t real_count = 0;
    for (const auto& block : span) {
      real_count += static_cast<std::size_t>(block.is_real().unwrap());
    }
    const std::size_t dummy_count = span.size() - real_count;
    state.log.dbgf(
        "  %s: offset=%zu size=%zu used=%zu real=%zu dummy=%zu", label, offset, span.size(), used, real_count,
        dummy_count
    );
    for (std::size_t ix = 0; ix < span.size(); ++ix) {
      const Block& block = span[ix];
      const bool is_real = block.is_real().unwrap();
      const char* type = is_real ? "real" : "dummy";
      const std::int64_t path = target_path(block);
      const std::int64_t depth = target_depth(block);
      const std::size_t storage_ix = offset + ix;
      if (is_real) {
        state.log.pedf(
            "    %s[%03zu] storage[%05zu]: %s block#%d path=%d depth=%d (address=$%08x leaf_ix=%d)", label, ix,
            storage_ix, type, block.uid, static_cast<int>(path), static_cast<int>(depth), block.address, block.leaf_ix
        );
      } else {
        state.log.pedf(
            "    %s[%03zu] storage[%05zu]: %s block#%d path=%d depth=%d", label, ix, storage_ix, type, block.uid,
            static_cast<int>(path), static_cast<int>(depth)
        );
      }
    }
  };

  log_section("treetop", treetop_span, subtree.treetop.offset, treetop_span.size());
  log_section("overlap", overlap_span, subtree.overlap_region.offset, overlap_span.size());
  log_section("local_deferred", local_deferred_span, subtree.local_deferred.offset, local_deferred_span.size());
  log_section(
      "evictslots", evictslots_span, subtree.evictslots.offset, static_cast<std::size_t>(subtree.evictslots_written)
  );
  log_section(
      "routed_pathreads", routed_span, subtree.routed_pathreads.offset,
      static_cast<std::size_t>(subtree.routed_real_count)
  );
  log_section("fillers", fillers_span, subtree.fillers.offset, 0);
}

template <typename Block>
inline void validate_final_subtree_layout(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix,
    const population& pop, std::uint64_t expected_evicted, std::uint64_t retained_capacity_ub
) {
  const auto treetop_span = subtree.treetop.span(subtree.storage);
  const auto overlap_span = subtree.overlap_region.span(subtree.storage);
  const auto local_deferred_span = subtree.local_deferred.span(subtree.storage);
  const auto routed_span = subtree.routed_pathreads.span(subtree.storage);
  const auto evictslots_span = subtree.evictslots.span(subtree.storage);
  const auto fillers_span = subtree.fillers.span(subtree.storage);

  sn::util::log::ensure(
      state.log, treetop_span.size() == geom.section_sizes.treetop,
      "forestzing::pipeline::evict_subtree: treetop size mismatch"
  );
  sn::util::log::ensure(
      state.log, local_deferred_span.size() == geom.section_sizes.local_deferred,
      "forestzing::pipeline::evict_subtree: local deferred size mismatch"
  );
  sn::util::log::ensure(
      state.log, overlap_span.size() == geom.section_sizes.overlap_region,
      "forestzing::pipeline::evict_subtree: overlap section size mismatch"
  );
  sn::util::log::ensure(
      state.log, evictslots_span.size() == geom.section_sizes.evictslots,
      "forestzing::pipeline::evict_subtree: evictslots size mismatch"
  );
  sn::util::log::ensure(
      state.log, treetop_span.size() + local_deferred_span.size() == retained_capacity_ub,
      "forestzing::pipeline::evict_subtree: retained capacity mismatch"
  );
  sn::util::log::ensure(
      state.log, overlap_span.size() + evictslots_span.size() == expected_evicted,
      "forestzing::pipeline::evict_subtree: evicted capacity mismatch"
  );

  const std::int64_t sentinel_depth = -1;
  std::uint64_t retained_real_count = 0;
  auto check_retained = [&](const auto& span, const char* label) {
    (void) label;
    for (const auto& block : span) {
      sn::util::log::ensuref(
          state.log, target_depth(block) == sentinel_depth,
          "forestzing::pipeline::evict_subtree: retained block depth mismatch: block#%d in %s has depth %d, expected "
          "%d",
          block.uid, label, static_cast<int>(target_depth(block)), static_cast<int>(sentinel_depth)
      );
      retained_real_count += static_cast<std::uint64_t>(block.is_real().unwrap());
    }
  };
  check_retained(treetop_span, "treetop");
  check_retained(local_deferred_span, "local_deferred");
  sn::util::log::ensure(
      state.log, retained_real_count == pop.retained_real,
      "forestzing::pipeline::evict_subtree: retained real count mismatch"
  );

  std::uint64_t evicted_real_count = 0;
  auto check_evicted = [&](const auto& span, const char* label) {
    (void) label;
    for (const auto& block : span) {
      const std::int64_t depth = target_depth(block);
      sn::util::log::ensure(
          state.log, depth >= 0 && depth <= static_cast<std::int64_t>(geom.subtree_height),
          "forestzing::pipeline::evict_subtree: evicted block depth out of range"
      );
      evicted_real_count += static_cast<std::uint64_t>(block.is_real().unwrap());
    }
  };
  check_evicted(overlap_span, "overlap");
  check_evicted(evictslots_span, "evictslots");
  sn::util::log::ensure(
      state.log, evicted_real_count == pop.evicted_real,
      "forestzing::pipeline::evict_subtree: evicted real count mismatch"
  );
  sn::util::log::ensure(
      state.log, pop.evicted_total == expected_evicted, "forestzing::pipeline::evict_subtree: evicted total mismatch"
  );
  sn::util::log::ensure(
      state.log, subtree.evictslots_written == evictslots_span.size(),
      "forestzing::pipeline::evict_subtree: evictslots written count mismatch"
  );

  for (const Block& block : routed_span) {
    sn::util::log::ensure(
        state.log, !block.is_real().unwrap(), "forestzing::pipeline::evict_subtree: routed pathreads not cleared"
    );
  }
  sn::util::log::ensure(
      state.log, subtree.routed_real_count == 0, "forestzing::pipeline::evict_subtree: routed real count not reset"
  );
  for (const Block& block : fillers_span) {
    sn::util::log::ensure(
        state.log, !block.is_real().unwrap(), "forestzing::pipeline::evict_subtree: fillers region not cleared"
    );
  }

  state.log.dbgf("forestzing::pipeline::evict_subtree: final layout validated for subtree=%d", subtree_ix);
}

} // namespace sn::oram::stash::forestzing::pipeline::debug

#endif
