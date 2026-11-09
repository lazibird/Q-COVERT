#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/threads/thread_team.hpp"
#include "sonic/util/formatter.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"
#include "sonic/util/profiling.hpp"

#include "sonic/oram/stash/forestzing/state.hpp"

namespace sn::oram::stash::forestzing::pipeline {

// plan describing the per-subtree eviction workload
struct plan_view {
  sn::util::span<std::uint32_t> subtree_active_subpaths;
  sn::util::span<std::uint64_t> subtree_leaves;
  std::size_t subpaths_per_subtree = 0;

  [[nodiscard]] sn::util::span<const std::uint64_t> subpath_leaves_for(std::uint32_t subtree_ix) const noexcept {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        subtree_ix < subtree_active_subpaths.size(), "forestzing::pipeline: leaves_for subtree index out of range"
    );
#endif
    const std::size_t offset = static_cast<std::size_t>(subtree_ix) * subpaths_per_subtree;
    sn::util::log::ensure(
        offset + subpaths_per_subtree <= subtree_leaves.size(), "forestzing::pipeline: leaves_for out of bounds"
    );
    return sn::util::span<const std::uint64_t>(subtree_leaves.data() + offset, subpaths_per_subtree);
  }
};

struct population {
  std::uint64_t evicted_total = 0;
  std::uint64_t evicted_real = 0;
  std::uint64_t retained_real = 0;
};

template <typename Block> struct result {};

struct leaf_range {
  std::uint64_t begin = 0;
  std::uint64_t end = 0;

  [[nodiscard]] std::uint64_t size() const noexcept { return end - begin; }
};

[[nodiscard]] inline std::uint64_t encode_target_coord(std::int64_t path, std::int64_t depth) noexcept {
  const std::uint64_t path_bits = static_cast<std::uint64_t>(static_cast<std::int32_t>(path));
  const std::uint64_t depth_bits = static_cast<std::uint64_t>(static_cast<std::int32_t>(depth));
  return (path_bits << 32U) | depth_bits;
}

[[nodiscard]] inline std::int64_t decode_target_path(std::uint64_t encoded) noexcept {
  return static_cast<std::int32_t>(encoded >> 32U);
}

[[nodiscard]] inline std::int64_t decode_target_depth(std::uint64_t encoded) noexcept {
  return static_cast<std::int32_t>(encoded & 0xffffffffU);
}

template <typename Block> inline void set_target_coord(Block& block, std::int64_t path, std::int64_t depth) noexcept {
  block.extra = encode_target_coord(path, depth);
}

template <typename Block> inline std::int64_t target_path(const Block& block) noexcept {
  return decode_target_path(block.extra);
}

template <typename Block> inline std::int64_t target_depth(const Block& block) noexcept {
  return decode_target_depth(block.extra);
}

} // namespace sn::oram::stash::forestzing::pipeline

#if defined(ORAM_DEBUG)
#include "sonic/oram/stash/forestzing/debug.hpp"
#endif

namespace sn::oram::stash::forestzing::pipeline {

// sorting key for grouping blocks by target coordinate
struct target_coord_key {
  std::uint64_t packed = 0;
};

// extract key for target coordinate grouping sort
template <typename Block> struct target_coord_key_extractor {
  explicit target_coord_key_extractor(std::uint64_t max_depth) noexcept : max_depth_(max_depth) {
    std::uint64_t tmp = max_depth_;
    while (tmp > 0) {
      ++depth_bits_;
      tmp >>= 1U;
    }
    if (depth_bits_ == 0) {
      depth_bits_ = 1;
    }
    path_shift_ = depth_bits_ + 1U; // reserve bits for inverted depth and dummy flag
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        path_shift_ < 64U, "forestzing::pipeline::target_coord_group_sort: shift exceeds 64-bit packing"
    );
#endif
  }

  target_coord_key operator()(const Block& block) const noexcept {
    target_coord_key key{};
    const std::uint64_t path = static_cast<std::uint64_t>(target_path(block));
    const std::uint64_t depth = static_cast<std::uint64_t>(target_depth(block));
    const std::uint64_t inv_depth = max_depth_ - depth; // depth descending -> ascending
    const std::uint64_t dummy_bit = static_cast<std::uint64_t>(block.is_dummy().unwrap());

    // pack as [path | inv_depth | dummy]
    key.packed = (path << path_shift_) | (inv_depth << 1U) | dummy_bit;
    return key;
  }

private:
  std::uint64_t max_depth_ = 0;
  std::uint64_t depth_bits_ = 0;
  std::uint64_t path_shift_ = 0;
};

// constant-time comparator for target coordinate ordering
struct target_coord_key_compare {
  // order: path asc, depth desc, real before dummy (encoded in packed key)
  bool operator()(const target_coord_key& lhs, const target_coord_key& rhs) const noexcept {
    return sn::obliv::ct_lt<std::uint64_t>(lhs.packed, rhs.packed);
  }
};

#if defined(SONIC_ORAM_METRICS)
template <typename Block>
[[nodiscard]] inline std::uint64_t count_real_blocks(sn::util::span<const Block> blocks) noexcept {
  std::uint64_t count = 0;
  for (const auto& block : blocks) {
    count += static_cast<std::uint64_t>(block.is_real().unwrap());
  }
  return count;
}

template <typename Block>
inline void observe_subtree_stash_sections(stash_state<Block>& state, const subtree_storage<Block>& subtree) {
  const auto treetop_span = subtree.treetop.span(subtree.storage);
  const auto overlap_span = subtree.overlap_region.span(subtree.storage);
  const auto local_deferred_span = subtree.local_deferred.span(subtree.storage);
  const std::uint64_t treetop_real = count_real_blocks(treetop_span);
  const std::uint64_t overlap_real = count_real_blocks(overlap_span);
  const std::uint64_t local_deferred_real = count_real_blocks(local_deferred_span);
  const std::uint64_t subtree_stash_real = treetop_real + overlap_real + local_deferred_real;
  state.metrics.observe_subtree_stash_treetop_real(treetop_real);
  state.metrics.observe_subtree_stash_overlap_real(overlap_real);
  state.metrics.observe_subtree_stash_deferred_real(local_deferred_real);
  state.metrics.observe_subtree_stash_real(subtree_stash_real);
}
#endif

template <typename Block>
inline void observe_forest_stash_sections(stash_state<Block>& state) {
#if defined(SONIC_ORAM_METRICS)
  std::uint64_t treetop = 0, overlap = 0, deferred = 0;
  for (const auto& subtree : state.subtrees) {
    treetop += count_real_blocks(subtree.treetop.span(subtree.storage));
    overlap += count_real_blocks(subtree.overlap_region.span(subtree.storage));
    deferred += count_real_blocks(subtree.local_deferred.span(subtree.storage));
  }
  state.metrics.observe_subtree_stash_treetop_real(treetop);
  state.metrics.observe_subtree_stash_overlap_real(overlap);
  state.metrics.observe_subtree_stash_deferred_real(deferred);
  state.metrics.observe_subtree_stash_real(treetop + overlap + deferred);
#endif
}

template <typename Block> inline void snapshot_global_stash(stash_state<Block>& state) {
  sn_prof_zone("forestzing.pipeline.snapshot_global");
  auto& runtime = state.runtime;

  // clear previous snapshot
  auto& uid = state.uid();
  for (std::size_t i = 0; i < runtime.snapshot_buffer.size(); ++i) {
    runtime.snapshot_buffer[i].set_dummy(uid);
  }

  // snapshot the global stash to buffer
  auto snapshot_span = sn::util::span<Block>(runtime.snapshot_buffer.data(), runtime.snapshot_buffer.size());
  runtime.snapshot_count = state.global_stash->condense_to(snapshot_span);
#if defined(SONIC_ORAM_METRICS)
  state.metrics.observe_global_snapshot_real(static_cast<std::uint64_t>(runtime.snapshot_count));
#endif
}

// route global stash snapshot to subtree
template <typename Block>
inline void route_gstash_to_subtree(
    stash_state<Block>& state, subtree_storage<Block>& subtree, std::uint32_t subtree_ix, leaf_range subtree_leaf_range,
    typename stash_runtime<Block>::worker_scratch& scratch
) {
  sn_prof_zone("forestzing.pipeline.route_gstash");
  const auto snapshot_span = std::as_const(state.runtime).snapshot_span();
  auto routing_span = scratch.routing_span();
  auto marks_span = scratch.marks_span();
  auto prefix_span = scratch.prefix_span();

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      snapshot_span.size() == routing_span.size(),
      "forestzing::pipeline::route_gstash_to_subtree: snapshot/routing size mismatch"
  );
  sn::util::log::ensure(
      routing_span.size() <= marks_span.size(), "forestzing::pipeline::route_gstash_to_subtree: marks span too small"
  );
  sn::util::log::ensure(
      routing_span.size() <= prefix_span.size(), "forestzing::pipeline::route_gstash_to_subtree: prefix span too small"
  );
#endif

  // copy snapshot to routing buffer
  sn::obliv::copy(snapshot_span.begin(), snapshot_span.end(), routing_span.begin());
  // clear marks
  sn::obliv::fill(marks_span.begin(), marks_span.end(), std::uint8_t{0});

  std::size_t n_marked = 0;
  for (std::size_t ix = 0; ix < routing_span.size(); ++ix) {
    Block& block = routing_span[ix];
    const sn::obliv::choice is_real = block.is_real();

    // check if block's assigned leaf is in the subtree's leaf range
    const sn::obliv::choice ge_min(sn::obliv::ct_ge<std::int64_t>(block.leaf_ix, subtree_leaf_range.begin));
    const sn::obliv::choice lt_max(sn::obliv::ct_lt<std::int64_t>(block.leaf_ix, subtree_leaf_range.end));
    const sn::obliv::choice in_subtree = is_real && ge_min && lt_max;

    // mark if in subtree
    const std::uint8_t keep = static_cast<std::uint8_t>(in_subtree.unwrap());
    marks_span[ix] = keep;
    n_marked += keep;
  }

  // compact marked blocks to front of routing buffer
  {
    sn_prof_zone("forestzing.pipeline.route_gstash.compact");
    sn::sortshuffle::ser::orshuffle::orcompact(
        routing_span.data(), routing_span.size(), marks_span.data(), prefix_span.data()
    );
  }

  // ensure we didn't exceed the bound of routed pathreads
  auto routed_out_span = subtree.routed_pathreads.span(subtree.storage);
  sn::util::log::ensure(
      routing_span.size() >= routed_out_span.size(),
      "forestzing::pipeline::route_gstash_to_subtree: routing span smaller than output span"
  );
  sn::util::log::ensuref(
      state.log, n_marked <= routed_out_span.size(),
      "forestzing::pipeline::route_gstash_to_subtree: too many blocks routed to subtree (%d > %d)", n_marked,
      routed_out_span.size()
  );

  // copy the bounded compacted prefix into the routed section
  sn::obliv::copy(routing_span.begin(), routing_span.begin() + routed_out_span.size(), routed_out_span.begin());

  // obliviously clear slots beyond the kept count
  auto& uid = state.uid();
  for (std::size_t ix = 0; ix < routed_out_span.size(); ++ix) {
    const sn::obliv::choice drop(
        sn::obliv::ct_ge<std::uint64_t>(static_cast<std::uint64_t>(ix), static_cast<std::uint64_t>(n_marked))
    );
    routed_out_span[ix].set_dummy_cond(drop, uid);
  }

#if defined(ORAM_DEBUG)
  state.log.dbgf(
      "forestzing::pipeline::route_gstash_to_subtree: subtree=%d routed_real=%d capacity=%d", subtree_ix, n_marked,
      routed_out_span.size()
  );
#endif

  subtree.routed_real_count = static_cast<std::uint32_t>(n_marked);
#if defined(SONIC_ORAM_METRICS)
  state.metrics.observe_subtree_routed_real(static_cast<std::uint64_t>(n_marked));
#endif
}

// iterate over all worker (non-filler) blocks and invoke fn(block, i)
template <typename Block, typename Fn> inline void for_each_working_block(subtree_storage<Block>& subtree, Fn&& fn) {
  const auto sections = std::array<const typename subtree_storage<Block>::section*, 5>{
      &subtree.treetop, &subtree.overlap_region, &subtree.local_deferred, &subtree.routed_pathreads, &subtree.evictslots
  };

  std::size_t i = 0;
  for (const auto* section : sections) {
    auto span = section->span(subtree.storage);
    for (auto& block : span) {
      fn(block, i);
      ++i;
    }
  }
}

// assign target coordinates to all working blocks in subtree stash
template <typename Block>
inline void assign_target_coords(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix,
    sn::util::span<const std::uint64_t> leaves_span, std::uint32_t active_subpaths, leaf_range subtree_leaf_range,
    typename stash_runtime<Block>::worker_scratch& scratch
) {
  sn_prof_zone("forestzing.pipeline.assign_coords");
  const std::int64_t max_depth = static_cast<std::int64_t>(geom.subtree_height);
  auto& prng = *scratch.prng;

  // the stride between each evict subpath
  const std::uint64_t evict_subpath_stride = geom.subtree_leaf_count / active_subpaths;
  // the offset (from the first leaf) of the first evict subpath
  const std::uint64_t evict_first_subpath_offset = leaves_span[0];
  // the subtree-local node id of the first leaf
  const std::uint64_t subtree_leaf_level_first_node_id = geom.subtree_leaf_count;

  // assign coordinate to each working block
  for_each_working_block(subtree, [&](Block& block, std::size_t iter_i) {
    const sn::obliv::choice block_is_real = block.is_real();
    const sn::obliv::choice block_is_dummy = !block_is_real;

#if defined(ORAM_DEBUG)
    if (block_is_real.unwrap()) {
      // any real block's mapped leaf must be within the subtree's leaf range
      sn::util::log::ensuref(
          block.leaf_ix >= static_cast<std::int64_t>(subtree_leaf_range.begin) &&
              block.leaf_ix < static_cast<std::int64_t>(subtree_leaf_range.end),
          "forestzing::pipeline::assign_target_coords: real block outside subtree leaf range: block#%d leaf_ix=%d not "
          "in [%d, %d)",
          block.uid, block.leaf_ix, subtree_leaf_range.begin, subtree_leaf_range.end
      );
    }
#endif

    // get mapped leaf (only valid if real)
    std::int64_t block_leaf = block.leaf_ix;

    // select a random leaf for dummy blocks
    const std::int64_t dummy_leaf = prng.random_u64(subtree_leaf_range.begin, subtree_leaf_range.end);

    // if the block is dummy, assign to random leaf
    sn::obliv::ct_set<std::int64_t>(&block_leaf, dummy_leaf, block_is_dummy.unwrap());

    // block's local leaf index within subtree
    const std::int64_t block_leaf_local = block_leaf - static_cast<std::int64_t>(subtree_leaf_range.begin);

#if defined(ORAM_DEBUG)
    // ensure we now have a valid block leaf
    sn::util::log::ensuref(
        block_leaf >= static_cast<std::int64_t>(subtree_leaf_range.begin) &&
            block_leaf < static_cast<std::int64_t>(subtree_leaf_range.end),
        "forestzing::pipeline::assign_target_coords: block leaf out of bounds: %d not in [%d, %d)", block_leaf,
        subtree_leaf_range.begin, subtree_leaf_range.end
    );
#endif

    // determine coordinate of block's preferred evict node
    // a 2d coordinate in the form (path_ix, depth)

    // get global leaf node id
    std::uint64_t mapped_leaf_node_id =
        scratch.vtree_topology.leaf_index_to_node_id(static_cast<std::uint64_t>(block_leaf));
    // get subtree-local leaf node id
    std::uint64_t mapped_local_leaf_node_id =
        scratch.forest_topology.node_id_to_subtree_node_id(mapped_leaf_node_id, subtree_ix);

    // - obliviously determine the target path
    // determine rank in the subtree leaf level
    std::uint64_t subtree_leaf_rank = mapped_local_leaf_node_id - subtree_leaf_level_first_node_id;
    // determine which active evict path (local leaf ix)
    std::uint64_t evict_path_region = subtree_leaf_rank / evict_subpath_stride;
    // determine the index in revlex order of the evict path
    std::uint64_t evict_path_revlex_ix = state.runtime.region_to_revlex_path_ix(evict_path_region);
    // determine subtree-local leaf ix of the evict path
    std::uint64_t evict_local_leaf_ix =
        sn::obliv::ct_madd(evict_path_region, evict_subpath_stride, evict_first_subpath_offset);
    std::uint64_t evict_local_leaf_node_id = scratch.subtree_topology.leaf_index_to_node_id(evict_local_leaf_ix);

    // - obliviously determine the target depth
    // determine the deepest depth where this block can reside
    const std::uint64_t evict_local_depth =
        scratch.subtree_topology.ct_evictable_depth(mapped_local_leaf_node_id, evict_local_leaf_node_id);

    // store coordinate
    std::int64_t coord_path_ix = evict_path_revlex_ix;
    std::int64_t coord_depth = evict_local_depth;

#if defined(ORAM_DEBUG)
    // state.log.pedf(
    //     "    subtree=%d it=%d leaf=%d region=%d path=%d depth=%d", subtree_ix, iter_i, block_leaf, evict_path_region,
    //     coord_path_ix, coord_depth
    // );

    if (block_is_real.unwrap()) {
      state.log.pedf(
          "  working[%03d]: real block#%d (address=$%08x, leaf_ix=%d) assigned to coord (p=%d,d=%d)", iter_i, block.uid,
          block.address, block.leaf_ix, coord_path_ix, coord_depth
      );
    } else {
      state.log.pedf(
          "  working[%03d]: dummy block#%d (rnd_leaf_ix=%d) assigned to coord (p=%d,d=%d)", iter_i, block.uid,
          block_leaf, coord_path_ix, coord_depth
      );
    }
    // ensure coordinate is valid
    sn::util::log::ensure(
        state.log, coord_path_ix >= 0 && coord_path_ix < static_cast<std::int64_t>(active_subpaths),
        "forestzing::pipeline::assign_target_coords: path index out of bounds"
    );
    sn::util::log::ensure(
        state.log, coord_depth >= 0 && coord_depth <= max_depth,
        "forestzing::pipeline::assign_target_coords: depth out of bounds"
    );
#endif

    set_target_coord(block, coord_path_ix, coord_depth);
  });
}

template <typename Block>
inline void populate_fillers(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix
) {
  sn_prof_zone("forestzing.pipeline.populate_fillers");
  auto filler_span = subtree.fillers.span(subtree.storage);
  const std::size_t path_count = static_cast<std::size_t>(geom.evict_batch);
  const std::size_t depth_count = static_cast<std::size_t>(geom.subtree_height + 1ULL);

  // total number of filler blocks needed
  [[maybe_unused]] const std::size_t n_filler = path_count * depth_count * static_cast<std::size_t>(geom.bucket_real);

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      filler_span.size() == n_filler, "forestzing::pipeline::populate_fillers: filler span size mismatch"
  );
#endif

  auto& uid = state.uid();
  std::size_t filler_ix = 0;
  for (std::size_t path_ix = 0; path_ix < path_count; ++path_ix) {
    for (std::size_t depth = 0; depth < depth_count; ++depth) {
      for (std::size_t slot = 0; slot < geom.bucket_real; ++slot) {
        Block& filler = filler_span[filler_ix++];
        // create dummy block with current path/depth
        filler.set_dummy(uid);
        set_target_coord(filler, static_cast<std::int64_t>(path_ix), static_cast<std::int64_t>(depth));
      }
    }
  }
#if defined(ORAM_DEBUG)
  state.log.dbgf("forestzing::pipeline::populate_fillers: subtree=%d filler_blocks=%d", subtree_ix, filler_span.size());
#endif
}

// sort subtree storage by target coordinate (path asc, depth desc, real-first)
template <typename Block>
inline void target_coord_group_sort(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix
) {
  sn_prof_zone("forestzing.pipeline.group_sort");
  auto storage_span = subtree.storage.span();

#if defined(ORAM_DEBUG)
  state.log.dbgf(
      "forestzing::pipeline::target_coord_group_sort: subtree=%d slots=%d", subtree_ix,
      static_cast<std::uint64_t>(storage_span.size())
  );
  sn::util::log::ensure(
      storage_span.size() == static_cast<std::size_t>(geom.section_sizes.total()),
      "forestzing::pipeline::target_coord_group_sort: storage size mismatch"
  );
#endif

  target_coord_key_extractor<Block> extractor{geom.subtree_height};
  target_coord_key_compare compare{};
  {
    sn_prof_zone("forestzing.pipeline.group_sort.sort");
#if defined(RINGDORAM_COUNT_SORT_COMPARATORS)
    sn::sortshuffle::ser::bitonic::counting::category_scope comparator_scope(
        sn::sortshuffle::ser::bitonic::counting::category::eviction_main
    );
#endif
    sn::sortshuffle::ser::bitonic::bitonic_sort(storage_span.data(), storage_span.size(), extractor, compare);
  }

#if defined(ORAM_DEBUG)
  debug::log_sorted_storage(state, geom, subtree, subtree_ix);
  debug::verify_sorted_storage(state, geom, subtree, subtree_ix);
#endif
}

// postprocess to push up blocks from overfull buckets, multi-path layout
template <typename Block>
inline population postprocess_multipath(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix,
    typename stash_runtime<Block>::worker_scratch& scratch
) {
  sn_prof_zone("forestzing.pipeline.postprocess");
  using std::int64_t;
  using std::uint64_t;
  using namespace sn::obliv;

  auto storage_span = subtree.storage.span();
  auto marks_span = scratch.marks_span();
  auto prefix_span = scratch.prefix_span();

#if defined(ORAM_DEBUG)
  state.log.dbgf(
      "forestzing::pipeline::postprocess_multipath: subtree=%d slots=%d", subtree_ix,
      static_cast<std::uint64_t>(storage_span.size())
  );
  sn::util::log::ensure(
      storage_span.size() <= marks_span.size(), "forestzing::pipeline::postprocess_multipath: marks span too small"
  );
  sn::util::log::ensure(
      storage_span.size() + 1 <= prefix_span.size(),
      "forestzing::pipeline::postprocess_multipath: prefix span too small"
  );
#endif

  // reset scratch buffers
  const std::size_t mark_reset = std::min(marks_span.size(), storage_span.size());
  sn::obliv::fill(marks_span.begin(), marks_span.begin() + static_cast<std::size_t>(mark_reset), std::uint8_t{0});
  const std::size_t prefix_reset = std::min(prefix_span.size(), storage_span.size() + 1);
  sn::obliv::fill(prefix_span.begin(), prefix_span.begin() + static_cast<std::size_t>(prefix_reset), std::size_t{0});

  population pop{};

  if (storage_span.empty()) {
#if defined(ORAM_DEBUG)
    state.log.dbgf("forestzing::pipeline::postprocess_multipath: subtree=%d storage empty", subtree_ix);
#endif
    return pop;
  }

  // each path has (H+1) buckets of size Z
  const uint64_t bucket_size = geom.bucket_real;
  const int64_t max_depth = static_cast<int64_t>(geom.subtree_height);
  // dropped blocks are sent to sentinel/unassigned depth -1
  const int64_t sentinel_depth = -1;
  // per-path capacity and total capacity
  [[maybe_unused]] const uint64_t path_capacity =
      (static_cast<uint64_t>(geom.subtree_height) + 1ULL) * static_cast<uint64_t>(geom.bucket_real);
  [[maybe_unused]] const uint64_t total_capacity = path_capacity * static_cast<uint64_t>(geom.evict_batch);

  std::int64_t current_path = target_path(storage_span[0]);
  int64_t waterline_depth = max_depth;
  uint64_t waterline_fill = 0;
  uint64_t path_evicted = 0;

  // scan the entire storage, previously grouped by target coordinate
  // we scan each path from leaf level towards the root
  // for each block, decide to evict, retain, or drop

  for (std::size_t ix = 0; ix < storage_span.size(); ++ix) {
    Block& block = storage_span[ix];
    // metadata: target path/depth, is_real
    const int64_t block_path = target_path(block);
    const int64_t block_depth = target_depth(block);
    const choice is_real = block.is_real();
    const choice is_dummy = !is_real;

    // first block initializes the waterline state
    const choice first_slot = choice(ct_eq<std::size_t>(ix, 0));
    const bool same_path = ct_eq<int64_t>(block_path, current_path);
    const choice is_new_path = choice(!same_path);

    // detect path transitions (first slot counts as new path)
    const choice path_changed = first_slot || is_new_path;
    const bool path_changed_bit = path_changed.unwrap();

#if defined(ORAM_DEBUG)
    const int64_t prev_waterline_depth = waterline_depth;
    const uint64_t prev_waterline_fill = waterline_fill;
    const uint64_t prev_path_evicted = path_evicted;

    sn::util::log::ensure(
        state.log, block_path >= 0 && block_path < static_cast<int64_t>(geom.evict_batch),
        "forestzing::pipeline::postprocess_multipath: path index out of bounds"
    );
    sn::util::log::ensure(
        state.log, block_depth >= -1 && block_depth <= max_depth,
        "forestzing::pipeline::postprocess_multipath: depth out of bounds"
    );
    if ((!first_slot && path_changed).unwrap()) {
      sn::util::log::ensure(
          state.log, prev_waterline_depth == sentinel_depth && prev_waterline_fill == 0,
          "forestzing::pipeline::postprocess_multipath: path transition with partially filled level"
      );
      sn::util::log::ensure(
          state.log, prev_path_evicted == path_capacity,
          "forestzing::pipeline::postprocess_multipath: path transition without full bucket population"
      );
    }
#endif

    current_path = ct_select<int64_t>(block_path, current_path, path_changed_bit);
    waterline_depth = ct_select<int64_t>(max_depth, waterline_depth, path_changed_bit);
    waterline_fill = ct_select<uint64_t>(0ULL, waterline_fill, path_changed_bit);
    path_evicted = ct_select<uint64_t>(0ULL, path_evicted, path_changed_bit);

    // waterline state (per-path)
    const choice path_full = choice(ct_lt<int64_t>(waterline_depth, static_cast<int64_t>(0)));
    // level has space if waterline_fill < bucket_size
    const choice level_has_space = !path_full && choice(ct_lt<uint64_t>(waterline_fill, bucket_size));
    // block assigned below waterline if current depth > waterline_depth
    const choice assigned_below_waterline = choice(ct_gt<int64_t>(block_depth, waterline_depth));

    // real blocks take priority; dummy/filler is only kept when the bucket has remaining slots
    const choice keep_evicted_real = is_real && !path_full;
    const choice keep_evicted_dummy = is_dummy && level_has_space && !assigned_below_waterline;
    const choice keep_evicted = keep_evicted_real || keep_evicted_dummy;
    // if real and not evicted, then retained
    const choice keep_retained_real = is_real && !keep_evicted;
    const choice keep_block = keep_evicted || keep_retained_real;
    // if dummy and not evicted, drop (garbage)
    [[maybe_unused]] const choice drop_block = !is_real && !keep_evicted;

#if defined(ORAM_DEBUG)
    debug::log_postprocess_iteration(
        state, subtree_ix, ix, current_path, block_depth, keep_evicted_real, keep_evicted_dummy, keep_retained_real,
        drop_block, keep_evicted, waterline_depth, waterline_fill, path_evicted
    );
#endif

    // if we keep the block in any form, mark for compaction
    marks_span[ix] = static_cast<std::uint8_t>(keep_block.unwrap());

    // if evicted, assign to waterline depth, otherwise sentinel (-1) for dropped blocks
    const int64_t assigned_depth = ct_select<int64_t>(waterline_depth, sentinel_depth, keep_evicted.unwrap());
    set_target_coord(block, current_path, assigned_depth);

    // if we evicted the block, increment waterline fullness
    const uint64_t kept_fill = ct_select<uint64_t>(waterline_fill + 1ULL, waterline_fill, keep_evicted.unwrap());
    // if the bucket is now full, raise waterline and reset fullness
    const bool bucket_full_now = keep_evicted.unwrap() && ct_eq<uint64_t>(kept_fill, bucket_size);
    waterline_fill = ct_select<uint64_t>(0ULL, kept_fill, bucket_full_now);
    waterline_depth = ct_select<int64_t>(waterline_depth - 1, waterline_depth, bucket_full_now);

    // update per-path and global populations
    path_evicted += static_cast<uint64_t>(keep_evicted.unwrap());
    pop.evicted_total += static_cast<uint64_t>(keep_evicted.unwrap());
    pop.evicted_real += static_cast<uint64_t>(keep_evicted_real.unwrap());
    pop.retained_real += static_cast<uint64_t>(keep_retained_real.unwrap());
  }

#if defined(ORAM_DEBUG)
  // verify postprocess invariants
  debug::verify_postprocess(
      state, geom, subtree_ix, pop, waterline_depth, waterline_fill, path_evicted, total_capacity
  );
  state.log.dbgf(
      "forestzing::pipeline::postprocess_multipath: subtree=%d evicted_total=%d evicted_real=%d retained_real=%d",
      subtree_ix, pop.evicted_total, pop.evicted_real, pop.retained_real
  );
#endif

  return pop;
}

// compact kept blocks to the front, evicted blocks first then retained blocks
template <typename Block>
inline void compact_kept_blocks(
    stash_state<Block>& state, subtree_storage<Block>& subtree, std::uint32_t subtree_ix,
    typename stash_runtime<Block>::worker_scratch& scratch, std::uint64_t kept_count_ub, std::uint64_t evicted_total,
    std::uint64_t retained_real, std::uint64_t retained_capacity_ub
) {
  sn_prof_zone("forestzing.pipeline.compact_kept");
  auto storage_span = subtree.storage.span();
  auto marks_span = scratch.marks_span();
  auto prefix_span = scratch.prefix_span();

  // total size of the block buffer
  const std::size_t storage_size = storage_span.size();

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      storage_size >= kept_count_ub, "forestzing::pipeline::compact_kept_blocks: kept upper bound exceeds storage size"
  );
  sn::util::log::ensure(
      kept_count_ub <= marks_span.size(),
      "forestzing::pipeline::compact_kept_blocks: marks span smaller than kept upper bound"
  );
  sn::util::log::ensure(
      kept_count_ub + 1 <= prefix_span.size(),
      "forestzing::pipeline::compact_kept_blocks: prefix span smaller than kept upper bound"
  );
  sn::util::log::ensure(
      retained_real <= retained_capacity_ub,
      "forestzing::pipeline::compact_kept_blocks: retained real exceeds retained capacity bound"
  );
#endif

  // step 1: compact all kept blocks to the front (with marks from postprocess)
  // we know that all kept blocks will be within the kept_count_ub prefix
  {
    sn_prof_zone("forestzing.pipeline.compact_kept.pass0");
    sn::sortshuffle::ser::orshuffle::orcompact(
        storage_span.data(), storage_size, marks_span.data(), prefix_span.data()
    );
  }

  // prepare marks for the second compaction
  sn::obliv::fill(marks_span.begin(), marks_span.begin() + static_cast<std::size_t>(kept_count_ub), std::uint8_t{0});

  // step 2: compact so evicted blocks precede retained blocks
  for (std::size_t ix = 0; ix < static_cast<std::size_t>(kept_count_ub); ++ix) {
    const Block& block = storage_span[ix];
    const bool is_retained = sn::obliv::ct_eq<std::int64_t>(target_depth(block), static_cast<std::int64_t>(-1));
    marks_span[ix] = static_cast<std::uint8_t>(!is_retained);
  }
  {
    sn_prof_zone("forestzing.pipeline.compact_kept.pass1");
    sn::sortshuffle::ser::orshuffle::orcompact(
        storage_span.data(), static_cast<std::size_t>(kept_count_ub), marks_span.data(), prefix_span.data()
    );
  }

#if defined(ORAM_DEBUG)
  state.log.dbgf(
      "forestzing::pipeline::compact_kept_blocks: subtree=%d kept_count_ub=%d evicted_total=%d retained_real=%d",
      subtree_ix, kept_count_ub, evicted_total, retained_real
  );
  sn::util::log::ensure(
      kept_count_ub >= evicted_total + retained_real,
      "forestzing::pipeline::compact_kept_blocks: kept upper bound violated"
  );
  debug::log_compact_layout(
      state, subtree, subtree_ix, evicted_total, retained_real, retained_capacity_ub, kept_count_ub
  );
  debug::validate_compact_layout(
      state, subtree, subtree_ix, evicted_total, retained_real, retained_capacity_ub, kept_count_ub
  );
#endif
}

template <typename Block>
inline sn::util::span<Block> copy_kept_blocks_to_aside(
    stash_state<Block>& state, subtree_storage<Block>& subtree, typename stash_runtime<Block>::worker_scratch& scratch,
    std::uint64_t kept_count_ub
) {
  sn_prof_zone("forestzing.pipeline.copy_kept");
  auto storage_span = subtree.storage.span();
  auto aside_span = scratch.kept_aside_span();
  auto& uid = state.uid();

  const std::size_t kept_count = static_cast<std::size_t>(kept_count_ub);

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      kept_count == aside_span.size(), "forestzing::pipeline::copy_kept_blocks_to_aside: aside span size mismatch"
  );
  sn::util::log::ensure(
      kept_count <= storage_span.size(),
      "forestzing::pipeline::copy_kept_blocks_to_aside: kept upper bound exceeds storage size"
  );
#endif

  // move kept blocks to aside buffer
  sn::obliv::copy(storage_span.begin(), storage_span.begin() + kept_count, aside_span.begin());
  // dummy fill the slots we moved out
  for (std::size_t ix = 0; ix < kept_count; ++ix) {
    storage_span[ix].set_dummy(uid);
  }

  return sn::util::span<Block>(aside_span.data(), kept_count);
}

// move evicted blocks into evictslots and overlap region
template <typename Block>
inline void distribute_evicted_paths(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix,
    const population& pop, sn::util::span<Block> evicted_aside, std::uint64_t expected_evicted
) {
  sn_prof_zone("forestzing.pipeline.distribute_evicted");
  auto evictslots_span = subtree.evictslots.span(subtree.storage);
  auto overlap_span = subtree.overlap_region.span(subtree.storage);
  auto& uid = state.uid();

  const std::uint64_t path_count = geom.evict_batch;
  const std::uint64_t levels_per_path = geom.subtree_height + 1ULL;
  const std::uint64_t path_stride = levels_per_path * geom.bucket_real;
  const std::uint64_t per_path_non_overlap = geom.non_overlap_height * geom.bucket_real;
  const std::uint64_t per_path_overlap = geom.overlap_depth * geom.bucket_real;

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      pop.evicted_total == expected_evicted, "forestzing::pipeline::distribute_evicted_paths: evicted total mismatch"
  );
  sn::util::log::ensure(
      evicted_aside.size() == expected_evicted,
      "forestzing::pipeline::distribute_evicted_paths: source span size mismatch"
  );
  sn::util::log::ensure(
      evictslots_span.size() == path_count * per_path_non_overlap,
      "forestzing::pipeline::distribute_evicted_paths: evictslots size mismatch"
  );
  sn::util::log::ensure(
      overlap_span.size() == path_count * per_path_overlap,
      "forestzing::pipeline::distribute_evicted_paths: overlap section size mismatch"
  );
#endif

  const std::uint64_t bucket_size = geom.bucket_real;
  const std::uint64_t non_overlap_buckets = geom.non_overlap_height;
  const std::uint64_t overlap_buckets = geom.overlap_depth;

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      non_overlap_buckets + overlap_buckets == levels_per_path,
      "forestzing::pipeline::distribute_evicted_paths: bucket partition mismatch"
  );
#endif

  // move a bucket from source to dest, marking source as dummy
  auto move_bucket = [&](sn::util::span<Block> src_bucket, sn::util::span<Block> dst_bucket) {
    sn::obliv::copy(src_bucket.begin(), src_bucket.end(), dst_bucket.begin());
    for (Block& block : src_bucket) {
      block.set_dummy(uid);
    }
  };

  // for each path in the multipath evict buffer
  for (std::uint64_t path_ix = 0; path_ix < path_count; ++path_ix) {
    // span of all blocks in this path
    sn::util::span<Block> path_span(
        evicted_aside.data() + static_cast<std::size_t>(path_ix * path_stride), static_cast<std::size_t>(path_stride)
    );

    // span of destination evictslots for non-overlap region
    sn::util::span<Block> evict_dst_span(
        evictslots_span.data() + static_cast<std::size_t>(path_ix * per_path_non_overlap),
        static_cast<std::size_t>(per_path_non_overlap)
    );
    // for each bucket in the non-overlap region (from leaf to root)
    for (std::uint64_t bucket_ix = 0; bucket_ix < non_overlap_buckets; ++bucket_ix) {
      // move bucket from multipath to evictslots
      const std::size_t src_bucket_ix = static_cast<std::size_t>(non_overlap_buckets - 1ULL - bucket_ix);
      auto src_bucket = path_span.subspan(
          src_bucket_ix * static_cast<std::size_t>(bucket_size), static_cast<std::size_t>(bucket_size)
      );
      auto dst_bucket = evict_dst_span.subspan(
          bucket_ix * static_cast<std::size_t>(bucket_size), static_cast<std::size_t>(bucket_size)
      );
      move_bucket(src_bucket, dst_bucket);
    }

    // span of destination overlap region for overlap region
    sn::util::span<Block> overlap_dst_span(
        overlap_span.data() + static_cast<std::size_t>(path_ix * per_path_overlap),
        static_cast<std::size_t>(per_path_overlap)
    );
    // for each bucket in the overlap region (from overlap start to root)
    for (std::uint64_t bucket_ix = 0; bucket_ix < overlap_buckets; ++bucket_ix) {
      // move bucket from multipath to overlap region
      const std::size_t src_bucket_ix =
          static_cast<std::size_t>(non_overlap_buckets + overlap_buckets - 1ULL - bucket_ix);
      auto src_bucket = path_span.subspan(
          src_bucket_ix * static_cast<std::size_t>(bucket_size), static_cast<std::size_t>(bucket_size)
      );
      auto dst_bucket = overlap_dst_span.subspan(
          bucket_ix * static_cast<std::size_t>(bucket_size), static_cast<std::size_t>(bucket_size)
      );
      move_bucket(src_bucket, dst_bucket);
    }
  }

  subtree.evictslots_written = static_cast<std::uint64_t>(path_count * per_path_non_overlap);
}

template <typename Block>
inline void distribute_retained_blocks(
    stash_state<Block>& state, const stash_geometry& geom, subtree_storage<Block>& subtree, std::uint32_t subtree_ix,
    const population& pop, std::uint64_t retained_capacity_ub, sn::util::span<Block> retained_aside
) {
  sn_prof_zone("forestzing.pipeline.distribute_retained");
  auto treetop_span = subtree.treetop.span(subtree.storage);
  auto local_deferred_span = subtree.local_deferred.span(subtree.storage);
  auto& uid = state.uid();

  const std::size_t retained_capacity = treetop_span.size() + local_deferred_span.size();

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      retained_capacity == retained_capacity_ub,
      "forestzing::pipeline::distribute_retained_blocks: retained capacity mismatch"
  );
  sn::util::log::ensure(
      retained_aside.size() == retained_capacity_ub,
      "forestzing::pipeline::distribute_retained_blocks: source span size mismatch"
  );
  sn::util::log::ensure(
      pop.retained_real <= retained_capacity_ub,
      "forestzing::pipeline::distribute_retained_blocks: retained exceeds capacity"
  );
#endif

  // move retained blocks, marking source as dummy
  auto move_retained_to_section = [&](sn::util::span<Block> dst_span, std::size_t src_offset) {
    for (std::size_t ix = 0; ix < dst_span.size(); ++ix) {
      Block& src = retained_aside[src_offset + ix];
      Block& dst = dst_span[ix];
      dst = src;
      src.set_dummy(uid);
    }
  };

  if (treetop_span.size() > 0) {
    move_retained_to_section(treetop_span, 0);
  }
  if (local_deferred_span.size() > 0) {
    move_retained_to_section(local_deferred_span, treetop_span.size());
  }
}

// dummify all blocks in the working/temporary subtree sections
template <typename Block>
inline void reset_working_sections(stash_state<Block>& state, subtree_storage<Block>& subtree) {
  sn_prof_zone("forestzing.pipeline.reset_work");
  auto& uid = state.uid();
  auto routed_span = subtree.routed_pathreads.span(subtree.storage);
  for (auto& block : routed_span) {
    block.set_dummy(uid);
  }
  auto filler_span = subtree.fillers.span(subtree.storage);
  for (auto& block : filler_span) {
    block.set_dummy(uid);
  }
  subtree.routed_real_count = 0;
}

template <typename Block>
inline void evict_subtree(
    stash_state<Block>& state, const stash_geometry& geom, const plan_view& plan, std::uint32_t subtree_ix,
    typename stash_runtime<Block>::worker_scratch& scratch
) {
  sn_prof_zone("forestzing.pipeline.evict_subtree");
  auto& subtree = state.subtrees[static_cast<std::size_t>(subtree_ix)];
  auto leaves_span = plan.subpath_leaves_for(subtree_ix);

  // store virtual (full tree) leaf indices for the subtree's leaf span
  const std::uint64_t subtree_begin = subtree_ix * geom.subtree_leaf_count;
  const leaf_range subtree_leaf_range{subtree_begin, subtree_begin + geom.subtree_leaf_count};
  const std::uint32_t n_evict_subpaths = plan.subtree_active_subpaths[static_cast<std::size_t>(subtree_ix)];

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      std::as_const(state.runtime).snapshot_span().size() == scratch.routing_span().size(),
      "forestzing::pipeline::evict_subtree: snapshot buffer size mismatch"
  );
  sn::util::log::ensure(
      n_evict_subpaths == geom.evict_batch,
      "forestzing::pipeline::evict_subtree: active subpath count must match evict batch"
  );
  sn::util::log::ensure(
      leaves_span.size() == static_cast<std::size_t>(geom.evict_batch),
      "forestzing::pipeline::evict_subtree: leaf span size mismatch"
  );
  sn::util::log::ensure(
      geom.subtree_leaf_count % geom.evict_batch == 0,
      "forestzing::pipeline::evict_subtree: subtree leaves must be divisible by evict batch"
  );
  state.log.trcf(
      "forestzing::pipeline::evict_subtree: subtree=%d (%d..%d), evict_leaves=%s", subtree_ix, subtree_leaf_range.begin,
      subtree_leaf_range.end, sn::util::format::format_vec(leaves_span)
  );
  // Reverse-lexicographic order is a permutation of equally spaced regions,
  // not an ascending walk (for E=4 it visits regions 0, 2, 1, 3).
  const std::uint64_t evict_leaf_ix_stride = geom.subtree_leaf_count / static_cast<std::uint64_t>(geom.evict_batch);
  const std::uint64_t first_leaf = leaves_span[0];
  for (std::size_t i = 0; i < leaves_span.size(); ++i) {
    const std::uint64_t relative =
        (leaves_span[i] + geom.subtree_leaf_count - first_leaf) % geom.subtree_leaf_count;
    sn::util::log::ensuref(
        relative % evict_leaf_ix_stride == 0,
        "forestzing::pipeline::evict_subtree: evict leaf not on region stride: %d", leaves_span[i]
    );
    const std::uint64_t region = relative / evict_leaf_ix_stride;
    sn::util::log::ensuref(
        state.runtime.region_to_revlex_path_ix(region) == i,
        "forestzing::pipeline::evict_subtree: invalid reverse-lex region order at path %d", i
    );
  }
#endif

  // route global stash snapshot to subtree
  route_gstash_to_subtree(state, subtree, subtree_ix, subtree_leaf_range, scratch);

  // assign 2d target coordinates
  assign_target_coords(state, geom, subtree, subtree_ix, leaves_span, n_evict_subpaths, subtree_leaf_range, scratch);

  // populate filler blocks
  populate_fillers(state, geom, subtree, subtree_ix);

  // sort blocks by target coordinate to realize infinity-oram grouping
  target_coord_group_sort(state, geom, subtree, subtree_ix);

  // postprocess grouped buckets across all evict paths
  const population pop = postprocess_multipath(state, geom, subtree, subtree_ix, scratch);

  const std::uint64_t levels_per_path = geom.subtree_height + 1ULL;
  const std::uint64_t expected_evicted = geom.evict_batch * levels_per_path * geom.bucket_real;
  const std::uint64_t retained_capacity_ub = geom.section_sizes.treetop + geom.section_sizes.local_deferred;
  const std::uint64_t kept_count_ub = expected_evicted + retained_capacity_ub;

  // check populations of evicted/retained blocks
  sn::util::log::ensure(
      pop.evicted_total == expected_evicted, "forestzing::pipeline::evict_subtree: evicted total mismatch (postprocess)"
  );
  sn::util::log::ensure(
      pop.retained_real <= retained_capacity_ub,
      "forestzing::pipeline::evict_subtree: retained exceeds treetop+deferred capacity"
  );
  sn::util::log::ensure(
      pop.evicted_total + pop.retained_real <= kept_count_ub,
      "forestzing::pipeline::evict_subtree: kept total exceeds upper bound"
  );

  // compact blocks into organized layout
  // [ evicted (multipath) | retained (treetop, deferred) | ...garbage... ]
  compact_kept_blocks(
      state, subtree, subtree_ix, scratch, kept_count_ub, pop.evicted_total, pop.retained_real, retained_capacity_ub
  );

  // copy kept blocks into a scratch "aside" buffer to allow redistribution without aliasing
  sn::util::span<Block> aside_span = copy_kept_blocks_to_aside(state, subtree, scratch, kept_count_ub);
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      static_cast<std::size_t>(expected_evicted + retained_capacity_ub) == aside_span.size(),
      "forestzing::pipeline::evict_subtree: aside kept span size mismatch"
  );
#endif
  // span for evicted blocks stored aside
  sn::util::span<Block> evicted_aside(aside_span.data(), static_cast<std::size_t>(expected_evicted));
  // span for retained blocks stored aside
  sn::util::span<Block> retained_aside(
      aside_span.data() + static_cast<std::size_t>(expected_evicted), static_cast<std::size_t>(retained_capacity_ub)
  );

  // move evicted path blocks into their final sections
  distribute_evicted_paths(state, geom, subtree, subtree_ix, pop, evicted_aside, expected_evicted);

  // place retained blocks back into subtree stash
  distribute_retained_blocks(state, geom, subtree, subtree_ix, pop, retained_capacity_ub, retained_aside);

  // reset temporary working regions
  reset_working_sections(state, subtree);

#if defined(SONIC_ORAM_METRICS)
  observe_subtree_stash_sections(state, subtree);
#endif

#if defined(ORAM_DEBUG)
  debug::log_final_subtree_layout(state, geom, subtree, subtree_ix, pop);
  debug::validate_final_subtree_layout(state, geom, subtree, subtree_ix, pop, expected_evicted, retained_capacity_ub);
#endif
}

template <typename Block>
inline void evict_subtrees(
    stash_state<Block>& state, const stash_geometry& geom, const plan_view& plan, sn::threads::thread_team& workers
) {
  sn_prof_zone("forestzing.pipeline.evict_subtrees");
  const std::size_t subtree_count = static_cast<std::size_t>(geom.subtree_count);
  const std::size_t logical_workers = workers.logical_threads();

  auto dispatch = [&](std::size_t worker_ix, std::size_t begin, std::size_t end) {
    auto& scratch = state.runtime.worker_buffers[worker_ix];
    for (std::size_t ix = begin; ix < end; ++ix) {
      evict_subtree(state, geom, plan, static_cast<std::uint32_t>(ix), scratch);
    }
  };

  if (logical_workers <= 1) {
    dispatch(0, 0, subtree_count);
  } else {
    workers.parallel_work([&, logical_workers](std::size_t logical_ix) noexcept {
      const auto [begin, end] = sn::threads::partition_evenly(logical_ix, subtree_count, logical_workers);
      if (begin == end) {
        return;
      }
      dispatch(logical_ix, begin, end);
    });
  }
}

template <typename Block>
inline result<Block> run(
    stash_state<Block>& state, const stash_geometry& geom, const plan_view& plan, sn::threads::thread_team& workers
) {
  sn_prof_zone("forestzing.pipeline.run");
#if defined(ORAM_DEBUG)
  state.log.trcf("forestzing::pipeline::run: subtrees=%d", geom.subtree_count);
  for (std::size_t ix = 0; ix < plan.subtree_active_subpaths.size(); ++ix) {
    state.log.dbgf("  subtree[%d]: subpaths=%s", ix, sn::util::format::format_vec(plan.subpath_leaves_for(ix)));
  }
  sn::util::log::ensure(
      state.runtime.worker_buffers.size() >= workers.logical_threads(),
      "forestzing::pipeline: insufficient worker scratch capacity"
  );
#endif

  // prepare global stash for routing
  auto& runtime = state.runtime;
  if (!state.disjoint_epoch_mode) {
    // normal mode: snapshot the lock-free global stash
    snapshot_global_stash(state);
  } else {
    // a global stash snapshot should already be prefilled
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        runtime.snapshot_prefilled, "forestzing::pipeline::run: expected prefilled snapshot in disjoint epoch mode"
    );
    state.log.dbgf(
        "forestzing::pipeline::run: using prefilled snapshot (count=%d)",
        static_cast<std::uint64_t>(runtime.snapshot_count)
    );
#endif
    runtime.snapshot_prefilled = false;
  }

  result<Block> res{};

  // evict subtrees in parallel
  observe_forest_stash_sections(state);
  evict_subtrees(state, geom, plan, workers);
  observe_forest_stash_sections(state);

  return res;
}

} // namespace sn::oram::stash::forestzing::pipeline
