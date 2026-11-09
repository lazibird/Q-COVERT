#pragma once

#include "sonic/util/profiling.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/oram/stash/forestzing/pipeline.hpp"

namespace sn::oram::stash::forestzing {

template <typename Block>
void insert_subtree_subpath_evictslots(
    stash<Block>& st, std::uint32_t subtree_ix, std::uint32_t evict_subpath_ix,
    sn::util::span<const Block> evict_subpath_blocks, std::size_t n_blocks
) {
  sn_prof_zone("forestzing.stash.insert_evictslots");
  // insert evictslots blocks into a subtree stash
  // evictslots serve double duty: first they store existing blocks for all eviction paths, then they receive
  // the rebuilt buckets produced by the eviction pipeline
  // subtree_ix: which subtree these evictslots are for
  // evict_subpath_ix: which evict subpath index within the subtree these evictslots are for
  // evict_subpath_blocks: the blocks to insert
  // n_blocks: the number of blocks to insert

#if defined(ORAM_DEBUG)
  log::ensure(subtree_ix < st.cfg.tree.subtree_count, "forestzing::stash: subtree_ix out of range");
  log::ensure(evict_subpath_ix < st.cfg.limits.evict_batch, "forestzing::stash: evict_subpath_ix out of range");
  st.state.log.dbgf(
      "insert_subtree_subpath_evictslots: subtree_ix=%d evict_subpath_ix=%d n_blocks=%d", subtree_ix, evict_subpath_ix,
      n_blocks
  );
#endif

  const std::size_t n_expected = static_cast<std::size_t>(st.cfg.limits.evictslots_per_path);
  log::ensure(n_blocks == n_expected, "forestzing::stash: invalid evictslots block count");
  log::ensure(evict_subpath_blocks.size() == n_expected, "forestzing::stash: evictslots span size mismatch");

  // get subtree storage
  auto& subtree = st.state.subtrees[subtree_ix];
  // get evictslots section span within subtree storage
  auto section_span = subtree.evictslots_span();
  // get offset of blocks for this subpath
  const std::size_t next_offset = static_cast<std::size_t>(evict_subpath_ix) * n_expected;
  log::ensure(next_offset + n_expected <= section_span.size(), "forestzing::stash: evictslots write overflow");
#if defined(ORAM_DEBUG)
  if (evict_subpath_ix > 0) {
    log::ensure(subtree.evictslots_written == next_offset, "forestzing::stash: evictslots writes must be sequential");
  }
#endif

  // copy inserted blocks into section at offset
  sn::obliv::copy(evict_subpath_blocks.begin(), evict_subpath_blocks.end(), section_span.begin() + next_offset);
  // update written count
  subtree.evictslots_written = static_cast<std::uint64_t>(next_offset + n_expected);
}

template <typename Block, typename Fn>
void consume_subtree_evictslots(stash<Block>& st, std::uint32_t subtree_ix, Fn&& fn) {
  sn_prof_zone("forestzing.stash.consume_evictslots");
  log::ensure(subtree_ix < st.cfg.tree.subtree_count, "forestzing::stash: subtree_ix out of range");

  // get subtree storage
  auto& subtree = st.state.subtrees[subtree_ix];
  // get evictslots section span within subtree storage
  auto section_span = subtree.evictslots_span();

  // get used portion of evictslots
  const std::size_t used = static_cast<std::size_t>(subtree.evictslots_written);
  log::ensure(used <= section_span.size(), "forestzing::stash: used evictslots exceeds section size");

  // get span of used region
  sn::util::span<const Block> used_span(section_span.data(), used);

  // apply function to used region
  fn(used_span);

  // clear evictslots for next use
  subtree.reset_counters();
}

template <typename Block>
void evict_to_paths(stash<Block>& st, pipeline::plan_view plan, sn::threads::thread_team& workers) {
  sn_prof_zone("forestzing.stash.evict_to_paths");
  const std::size_t subtree_count = static_cast<std::size_t>(st.state.geom.subtree_count);

#if defined(ORAM_DEBUG)
  // check plan consistency
  log::ensure(
      plan.subtree_active_subpaths.size() == subtree_count, "forestzing::stash::evict_to_paths: subtree count mismatch"
  );
  log::ensure(
      plan.subtree_leaves.size() == subtree_count * plan.subpaths_per_subtree,
      "forestzing::stash::evict_to_paths: leaf span mismatch"
  );
  log::ensure(
      plan.subpaths_per_subtree == static_cast<std::size_t>(st.state.geom.evict_batch),
      "forestzing::stash::evict_to_paths: plan subpath count mismatch"
  );
  // check evictslots are fully staged
  for (std::uint32_t subtree_ix = 0; subtree_ix < st.state.geom.subtree_count; ++subtree_ix) {
    const auto& subtree = st.state.subtrees[static_cast<std::size_t>(subtree_ix)];
    log::ensure(
        subtree.evictslots_written ==
            static_cast<std::uint64_t>(st.state.geom.evict_batch * st.cfg.limits.evictslots_per_path),
        "forestzing::stash::evict_to_paths: evictslots not fully staged"
    );
  }
#endif

  st.state.runtime.ensure_worker_capacity(workers.logical_threads(), st.state.geom, st.state.uid());

  const auto result = pipeline::run(st.state, st.state.geom, plan, workers);
  (void) result;
}

} // namespace sn::oram::stash::forestzing
