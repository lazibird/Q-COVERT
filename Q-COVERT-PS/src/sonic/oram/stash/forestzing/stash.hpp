#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/stash/forestzing/config.hpp"
#include "sonic/oram/stash/forestzing/layout.hpp"
#include "sonic/oram/stash/forestzing/metrics.hpp"
#include "sonic/oram/stash/forestzing/pipeline.hpp"
#include "sonic/oram/stash/forestzing/state.hpp"
#include "sonic/oram/tree/path_buffer.hpp"
#include "sonic/oram/tree/topology.hpp"
#include "sonic/threads/thread_team.hpp"

namespace sn::oram::stash::forestzing {

namespace log = sn::util::log;

template <typename Block> struct stash {
  using config = forestzing::config;
  using storage_type = sn::oram::stash::core::linear_block_storage<Block>;
  using section = typename storage_type::section;
  using eviction_plan_view = pipeline::plan_view;
  using metrics_snapshot = stash_metrics_snapshot;

  config cfg{};
  const sn::oram::tree::topology* topology = nullptr;
  stash_state<Block> state{};
};

template <typename Block>
stash<Block> make_stash(
    config cfg, const sn::oram::tree::topology& topo, sn::oram::uid_generator& uid_gen, sn::util::log::logger log
);

template <typename Block> void insert_pathread(stash<Block>& st, const Block& block);
template <typename Block> void insert_pathread_batch(stash<Block>& st, sn::util::span<const Block> blocks);
template <typename Block> Block extract(stash<Block>& st, std::int64_t address, std::uint32_t subtree_ix);

template <typename Block>
void insert_subtree_subpath_evictslots(
    stash<Block>& st, std::uint32_t subtree_ix, std::uint32_t evict_subpath_ix,
    sn::util::span<const Block> evict_subpath_blocks, std::size_t n_blocks
);

template <typename Block> stash_metrics_snapshot metrics_snapshot(const stash<Block>& st);
template <typename Block> void reset_metrics(stash<Block>& st);

template <typename Block, typename Fn>
void consume_subtree_evictslots(stash<Block>& st, std::uint32_t subtree_ix, Fn&& fn);

template <typename Block>
void evict_to_paths(stash<Block>& st, pipeline::plan_view plan, sn::threads::thread_team& workers);

} // namespace sn::oram::stash::forestzing

#include "sonic/oram/stash/forestzing/initialize.hpp"
#include "sonic/oram/stash/forestzing/access.hpp"
#include "sonic/oram/stash/forestzing/evict.hpp"
