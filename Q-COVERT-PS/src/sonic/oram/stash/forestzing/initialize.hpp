#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "sonic/util/picoformat.hpp"
#include "sonic/oram/zingoram/analysis.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::oram::stash::forestzing {

namespace detail {

template <typename Block> subtree_section_sizes compute_subtree_section_sizes(const stash<Block>& st) {
  sn_prof_zone("forestzing.stash.compute_section_sizes");
  subtree_section_sizes sizes{};

  sizes.treetop = st.cfg.tree.subpath_nonexistent_real_block_count;
  sizes.overlap_region = st.state.geom.evict_batch * st.state.geom.overlap_depth * st.state.geom.bucket_real;

  sizes.local_deferred = st.cfg.limits.stash_bound;

  if (!st.cfg.shared_top_backend || st.state.geom.routing_depth > 0) {
    sizes.routed_pathreads = sn::oram::zingoram::analysis::balls_and_bins_integer(
        st.cfg.limits.num_pathreads, st.state.geom.subtree_count, static_cast<double>(st.cfg.balls_and_bins_security)
    );
  }
  if (!st.cfg.shared_top_backend) {
    sizes.evictslots = st.state.geom.non_overlap_height * st.state.geom.evict_batch * st.state.geom.bucket_real;
    sizes.fillers = st.cfg.tree.subpath_real_block_count * st.state.geom.evict_batch;
  }

  return sizes;
}

template <typename Block> void initialize_subtree_limits(stash<Block>& st) {
  auto& limits = st.state.geom.limits;
  const auto& sections = st.state.geom.section_sizes;

  limits.routing_input_blocks = st.cfg.limits.num_pathreads;
  limits.routing_output_blocks = sections.routed_pathreads;
  limits.sort_size = sections.total();
  limits.max_real_blocks = sections.total() - sections.fillers;
  limits.max_treetop_blocks = sections.treetop;
  limits.overlap_region_blocks = sections.overlap_region;
  limits.max_unevicted_deferred = sections.local_deferred;
  limits.dummy_slots = sections.fillers;
}

template <typename Block> void initialize_geometry(stash<Block>& st) {
  log::ensure(st.cfg.tree.subtree_count > 0, "forestzing::stash: subtree_count must be positive");
  log::ensure(st.cfg.limits.num_pathreads > 0, "forestzing::stash: num_pathreads must be positive");

  auto& geom = st.state.geom;
  geom.subtree_count = st.cfg.tree.subtree_count;
  geom.routing_depth = st.cfg.tree.routing_depth;
  geom.overlap_depth = st.cfg.tree.overlap_depth;
  geom.non_overlap_height = st.cfg.tree.non_overlapping_subpath_height;
  geom.bucket_real = st.cfg.bucket.real;
  geom.evict_batch = st.cfg.limits.evict_batch;
  geom.subtree_height = st.cfg.tree.subtree_height;
  geom.tree_height = st.cfg.tree.height;
  geom.leaf_count = static_cast<std::uint64_t>(st.topology->leaf_count());
  log::ensure(geom.leaf_count > 0, "forestzing::stash: topology must expose positive leaf count");
  log::ensure(
      geom.leaf_count % geom.subtree_count == 0, "forestzing::stash: leaf count must be divisible by subtree count"
  );
  geom.subtree_leaf_count = geom.leaf_count / geom.subtree_count;

  log::ensure(
      geom.non_overlap_height + geom.routing_depth + geom.overlap_depth == st.cfg.tree.height + 1,
      "forestzing::stash: inconsistent non-overlapping subtree height"
  );

  geom.section_sizes = compute_subtree_section_sizes(st);
  initialize_subtree_limits(st);
}

template <typename Block> void initialize_global_stash(stash<Block>& st) {
  const std::uint64_t gstash_pathreads = st.cfg.limits.num_pathreads;
  const std::uint64_t gstash_mod_capacity = 2 * gstash_pathreads;
  const std::size_t global_mods_size = static_cast<std::size_t>(gstash_mod_capacity);

  st.state.log.trcf(
      "initialize: global stash layout: pathreads=%d mods=%d", static_cast<std::uint64_t>(gstash_pathreads),
      static_cast<std::uint64_t>(gstash_mod_capacity)
  );

  st.state.global_stash = std::make_unique<sn::oram::stash::core::lock_free_block_storage<Block>>(
      0, global_mods_size, st.state.log.child("global_stash")
  );
}

template <typename Block> void initialize_subtree_stashes(stash<Block>& st) {
  const std::uint64_t subtree_total = st.state.geom.section_sizes.total();
  log::ensure(subtree_total > 0, "forestzing::stash: subtree stash must have positive capacity");

  st.state.log.trcf(
      "initialize: subtree stash layout: total=%d (treetop=%d, local_deferred=%d, overlap_region=%d, "
      "sub_pathreads=%d, evictslots=%d, fillers=%d)",
      static_cast<std::uint64_t>(subtree_total), static_cast<std::uint64_t>(st.state.geom.section_sizes.treetop),
      static_cast<std::uint64_t>(st.state.geom.section_sizes.local_deferred),
      static_cast<std::uint64_t>(st.state.geom.section_sizes.overlap_region),
      static_cast<std::uint64_t>(st.state.geom.section_sizes.routed_pathreads),
      static_cast<std::uint64_t>(st.state.geom.section_sizes.evictslots),
      static_cast<std::uint64_t>(st.state.geom.section_sizes.fillers)
  );

  st.state.subtrees.clear();
  st.state.subtrees.reserve(st.state.geom.subtree_count);
  for (std::uint32_t subtree_ix = 0; subtree_ix < st.state.geom.subtree_count; ++subtree_ix) {
    auto subtree_logger = st.state.log.child(pfm::format("subtree[%d]", subtree_ix));
    st.state.subtrees.emplace_back(st.state.geom.section_sizes, st.state.uid(), std::move(subtree_logger));
  }

  const std::size_t subtree_count = static_cast<std::size_t>(st.state.geom.subtree_count);
  st.state.subtree_extract_locks =
      std::make_unique<typename stash_state<Block>::subtree_extract_lock[]>(subtree_count);
  st.state.subtree_extract_lock_count = subtree_count;
}

template <typename Block> void initialize_runtime_state(stash<Block>& st) {
  auto& runtime = st.state.runtime;
  auto& uid = st.state.uid();

  const std::size_t snapshot_capacity = static_cast<std::size_t>(st.cfg.limits.num_pathreads);
  runtime.initialize(st.state.geom, snapshot_capacity, uid, 1);
}

} // namespace detail

template <typename Block>
stash<Block> make_stash(
    config cfg, const sn::oram::tree::topology& topo, sn::oram::uid_generator& uid_gen, sn::util::log::logger log
) {
  stash<Block> st{};
  st.cfg = std::move(cfg);
  st.topology = &topo;
  st.state.log = std::move(log).child("forestzing");
  st.state.uid_generator = std::ref(uid_gen);
  st.state.disjoint_epoch_mode = st.cfg.disjoint_epoch_mode;

  detail::initialize_geometry(st);
  if (!st.cfg.disjoint_epoch_mode) {
    detail::initialize_global_stash(st);
  }
  detail::initialize_subtree_stashes(st);
  detail::initialize_runtime_state(st);

  return st;
}

template <typename Block> stash_metrics_snapshot metrics_snapshot(const stash<Block>& st) {
  stash_metrics_snapshot snapshot{};
  snapshot.global_snapshot_capacity = static_cast<std::uint64_t>(st.state.runtime.snapshot_span().size());
  snapshot.subtree_stash_total_capacity = st.state.geom.section_sizes.total();
  snapshot.routed_pathreads_capacity = st.state.geom.section_sizes.routed_pathreads;
  snapshot.subtree_stash_treetop_capacity = st.state.geom.section_sizes.treetop;
  snapshot.subtree_stash_overlap_capacity = st.state.geom.section_sizes.overlap_region;
  snapshot.subtree_stash_deferred_capacity = st.state.geom.section_sizes.local_deferred;
  snapshot.subtree_stash_relocated_capacity =
      snapshot.subtree_stash_treetop_capacity + snapshot.subtree_stash_overlap_capacity;
  snapshot.subtree_stash_overflow_capacity = snapshot.subtree_stash_deferred_capacity;
#if defined(SONIC_ORAM_METRICS)
  snapshot.maxima = st.state.metrics.maxima();
#endif
  return snapshot;
}

template <typename Block> void reset_metrics(stash<Block>& st) {
#if defined(SONIC_ORAM_METRICS)
  st.state.metrics.reset();
#else
  static_cast<void>(st);
#endif
}

} // namespace sn::oram::stash::forestzing
