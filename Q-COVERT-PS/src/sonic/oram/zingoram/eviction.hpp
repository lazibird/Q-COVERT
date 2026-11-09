#pragma once

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <vector>

#include "sonic/obliv/ops/platform_ops.hpp"
#include "sonic/util/formatter.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"

#include "sonic/oram/zingoram/schedule.hpp"
#include "sonic/oram/zingoram/shared_top_eviction.hpp"
#include "sonic/oram/zingoram/state.hpp"
#include "sonic/threads/thread_team.hpp"

namespace sn::oram::zingoram {

namespace fz_stash = sn::oram::stash::forestzing;

namespace detail {

// prepare plan for evicting subpaths in each subtree
template <typename Traits>
inline void prepare_eviction_plan(
    const state<Traits>& st, sn::util::span<const std::uint64_t> leaves, std::size_t subpaths_per_subtree,
    typename state<Traits>::eviction_plan_buffers& plan
) {
  sn_prof_zone("zingoram.eviction.prepare_plan");
  const auto& geom = st.shape();
  const std::size_t subtree_count = static_cast<std::size_t>(geom.subtree_count);
  const std::uint64_t subtree_leaf_count = geom.subtree_leaf_count;

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(subtree_leaf_count > 0, "zingoram::evict: subtree_leaf_count must be positive");
  sn::util::log::ensure(
      (subtree_leaf_count & (subtree_leaf_count - 1ULL)) == 0,
      "zingoram::evict: subtree_leaf_count expected power of two"
  );
  sn::util::log::ensure(
      leaves.size() == subtree_count * subpaths_per_subtree, "zingoram::evict: unexpected leaf batch size"
  );
  sn::util::log::ensure(
      plan.subtree_active_subpaths.size() == subtree_count, "zingoram::evict: eviction plan count size mismatch"
  );
  sn::util::log::ensure(
      plan.subtree_leaves.size() == subtree_count * subpaths_per_subtree,
      "zingoram::evict: eviction plan leaf size mismatch"
  );
#endif

  const auto subtree_leaf_shift = static_cast<unsigned int>(geom.subtree_height);
  const std::uint64_t subtree_leaf_mask = subtree_leaf_count - 1ULL;

  sn::obliv::fill(plan.subtree_active_subpaths.begin(), plan.subtree_active_subpaths.end(), 0);

  // gather evict leaves
  for (auto leaf_ix : leaves) {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(leaf_ix < geom.leaf_count, "zingoram::evict: scheduled leaf out of range");
#endif
    // determine which subtree this leaf belongs to
    const std::size_t subtree_ix = static_cast<std::size_t>(leaf_ix >> subtree_leaf_shift);
    // get the local leaf index within the subtree
    const std::uint64_t local_leaf = leaf_ix & subtree_leaf_mask;

#if defined(ORAM_DEBUG)
    sn::util::log::ensure(subtree_ix < subtree_count, "zingoram::evict: subtree_ix out of range");
#endif

    // record the leaf in the eviction plan
    std::uint32_t& count = plan.subtree_active_subpaths[subtree_ix];
    sn::util::log::ensure(count < subpaths_per_subtree, "zingoram::evict: subtree eviction batch overflow");
    plan.subtree_slot(subtree_ix, subpaths_per_subtree)[count] = local_leaf;
    ++count;
  }
}

// populate evictslots with existing blocks for all evict subpaths
template <typename Traits>
inline void stage_subtree_evictslots(
    state<Traits>& st, typename state<Traits>::eviction_plan_buffers& plan, sn::threads::thread_team& workers,
    std::size_t subpaths_per_subtree, std::size_t bucket_real_size, std::size_t non_overlap_height,
    std::size_t per_subpath_real_blocks, std::uint64_t non_overlap_start_level
) {
  sn_prof_zone("zingoram.eviction.stage");
  using block_t = typename Traits::block_t;
  using bucket_t = typename Traits::bucket_t;

  auto& geom = st.shape();
  auto& topo = st.topology();
  auto& storage = st.storage();
  auto& stash = st.stash();

  const std::size_t subtree_count = static_cast<std::size_t>(geom.subtree_count);
  const std::uint64_t subtree_leaf_count = geom.subtree_leaf_count;

  auto read_subtree = [&](std::uint32_t subtree_ix, typename state<Traits>::eviction_scratch& ctx) noexcept {
    sn_prof_zone("zingoram.eviction.stage.subtree");
    // work on all subpaths in this subtree
    const std::uint32_t subpath_count = plan.subtree_active_subpaths[subtree_ix];
    auto subpath_view = ctx.path.view();
    auto leaves_span = plan.subtree_slot(subtree_ix, subpaths_per_subtree);

#if defined(ORAM_DEBUG)
    sn::util::log::ensure(!leaves_span.empty(), "zingoram::evict: empty leaves span");
    sn::util::log::ensure(subpath_count > 0, "zingoram::evict: no subpaths to evict");
    sn::util::log::ensure(subpath_count <= subpaths_per_subtree, "zingoram::evict: subtree subpath overflow");
#endif

    // for each subpath in each subtree
    for (std::uint32_t subpath_ix = 0; subpath_ix < subpath_count; ++subpath_ix) {
      // get leaf index within the subtree
      const std::uint64_t local_leaf = leaves_span[subpath_ix];
      const std::uint64_t global_leaf = local_leaf + static_cast<std::uint64_t>(subtree_ix) * subtree_leaf_count;

      // get path to leaf using global leaf index
      topo.path_to_leaf(global_leaf, subpath_view.node_ids());

      // storage prefetch hint
      for (std::uint64_t level = non_overlap_start_level; level <= geom.height; ++level) {
        const std::uint64_t node_id = subpath_view.node_ids()[static_cast<std::size_t>(level)];
        storage[node_id].prefetch();
      }

      std::size_t blocks_written = 0;
      std::size_t bucket_slot = 0;

      // levels below the non_overlap_start_level are moved to the subtree stashes (treetop + overlap_region).
      // here we only operate on non-overlapping levels of the subpaths
      for (std::uint64_t level = non_overlap_start_level; level <= geom.height; ++level) {
        const std::uint64_t node_id = subpath_view.node_ids()[static_cast<std::size_t>(level)];
        bucket_t& bucket = storage[node_id];

        // read all real slots from the bucket
        {
          sn_prof_zone("zingoram.eviction.stage.bucket_read");
          bucket.read_bucket_max(ctx.bucket_real_buf.data(), ctx.bucket_offset_buf.data());
        }

        const auto src_span = sn::util::span<const block_t>(ctx.bucket_real_buf.data(), bucket_real_size);
        auto dest_span =
            sn::util::span<block_t>(ctx.evict_path_blocks.data() + bucket_slot * bucket_real_size, bucket_real_size);
        {
          sn_prof_zone("zingoram.eviction.stage.copy_reals");
          sn::obliv::copy(src_span.begin(), src_span.end(), dest_span.begin());
        }

        blocks_written += bucket_real_size;
        ++bucket_slot;
      }

#if defined(ORAM_DEBUG)
      sn::util::log::ensure(bucket_slot == non_overlap_height, "zingoram::evict: bucket count mismatch");
      sn::util::log::ensure(blocks_written == per_subpath_real_blocks, "zingoram::evict: real block count mismatch");
#endif

      sn::util::span<const block_t> subpath_span(ctx.evict_path_blocks.data(), blocks_written);
      {
        sn_prof_zone("zingoram.eviction.stage.stash_insert");
        fz_stash::insert_subtree_subpath_evictslots(stash, subtree_ix, subpath_ix, subpath_span, blocks_written);
      }
    }
  };

  const std::size_t logical_workers = workers.logical_threads();
  if (logical_workers <= 1) {
    auto& ctx = st.eviction_scratch_for_worker(0);
    for (std::uint32_t subtree_ix = 0; subtree_ix < static_cast<std::uint32_t>(subtree_count); ++subtree_ix) {
      read_subtree(subtree_ix, ctx);
    }
  } else {
    workers.parallel_work([&, logical_workers](std::size_t logical_ix) noexcept {
      const auto [begin, end] = sn::threads::partition_evenly(logical_ix, subtree_count, logical_workers);
      if (begin == end) {
        return;
      }
      auto& ctx = st.eviction_scratch_for_worker(logical_ix);
      for (std::size_t ix = begin; ix < end; ++ix) {
        read_subtree(static_cast<std::uint32_t>(ix), ctx);
      }
    });
  }

#if defined(ORAM_DEBUG)
  for (std::size_t subtree_ix = 0; subtree_ix < subtree_count; ++subtree_ix) {
    const std::uint32_t count = plan.subtree_active_subpaths[subtree_ix];
    if (count > 0) {
      const auto& leaves_span = plan.subtree_slot(subtree_ix, subpaths_per_subtree);
      std::vector<std::uint64_t> global_leaves(count);
      for (std::uint32_t i = 0; i < count; ++i) {
        global_leaves[i] = leaves_span[i] + static_cast<std::uint64_t>(subtree_ix) * subtree_leaf_count;
      }
      st.log().dbgf(
          "evict: subtree[%d] will evict %d subpaths: %s", subtree_ix, count,
          sn::util::format::format_vec(global_leaves)
      );
    }
  }
  st.log().dbgf("evict: staged evictslots for %d subtrees", subtree_count);
#endif
}

// rebuild buckets along evict subpaths from evictslots
template <typename Traits>
inline void rebuild_subtrees(
    state<Traits>& st, typename state<Traits>::eviction_plan_buffers& plan, sn::threads::thread_team& workers,
    std::size_t subpaths_per_subtree, std::size_t bucket_real_size, std::size_t per_subpath_real_blocks,
    std::uint64_t non_overlap_start_level
) {
  sn_prof_zone("zingoram.eviction.rebuild");
  using block_t = typename Traits::block_t;
  using bucket_t = typename Traits::bucket_t;

  auto& geom = st.shape();
  auto& topo = st.topology();
  auto& storage = st.storage();
  auto& stash = st.stash();
  auto& uid_gen = st.uid_gen();
  auto& prng = st.prng();
  auto& epochs = st.epoch_states();

  const std::size_t subtree_count = static_cast<std::size_t>(geom.subtree_count);
  const std::uint64_t subtree_leaf_count = geom.subtree_leaf_count;
  const std::size_t height = static_cast<std::size_t>(geom.height);

  auto rebuild_subtree = [&](std::uint32_t subtree_ix, typename state<Traits>::eviction_scratch& ctx) noexcept {
    const std::uint32_t subpath_count = plan.subtree_active_subpaths[subtree_ix];
    sn_prof_zone("zingoram.eviction.rebuild.subtree");
    if (subpath_count == 0) {
      return;
    }

    auto path_view = ctx.path.view();

    fz_stash::consume_subtree_evictslots(stash, subtree_ix, [&](sn::util::span<const block_t> evict_span) {
      if (evict_span.empty()) {
        return;
      }

#if defined(ORAM_DEBUG)
      sn::util::log::ensure(
          evict_span.size() == subpath_count * per_subpath_real_blocks, "zingoram::evict: evictslots span size mismatch"
      );
#endif

      auto leaves_span = plan.subtree_slot(subtree_ix, subpaths_per_subtree);
#if defined(ORAM_DEBUG)
      sn::util::log::ensure(subpath_count <= subpaths_per_subtree, "zingoram::evict: subtree subpath overflow");
#endif
      // for each subpath in this subtree
      for (std::uint32_t subpath_ix = 0; subpath_ix < subpath_count; ++subpath_ix) {
        const std::uint64_t local_leaf = leaves_span[subpath_ix];
        const std::uint64_t global_leaf = local_leaf + static_cast<std::uint64_t>(subtree_ix) * subtree_leaf_count;

        topo.path_to_leaf(global_leaf, path_view.node_ids());
#if defined(ORAM_DEBUG)
        sn::util::log::ensure(path_view.node_ids().size() == height + 1, "zingoram::evict: node_ids mismatch");
#endif

        // storage prefetch hint
        for (std::uint64_t level = non_overlap_start_level; level <= geom.height; ++level) {
          const std::uint64_t node_id = path_view.node_ids()[static_cast<std::size_t>(level)];
          storage[node_id].prefetch();
        }

        const auto subpath_span = sn::util::span<const block_t>(
            evict_span.data() + subpath_ix * per_subpath_real_blocks, per_subpath_real_blocks
        );

        // work in the non-overlapping levels of the subpath
        for (std::int64_t level = static_cast<std::int64_t>(geom.height);
             level >= static_cast<std::int64_t>(non_overlap_start_level); --level) {
          sn_prof_zone("zingoram.eviction.rebuild.bucket");
          const std::size_t idx = static_cast<std::size_t>(level);
          const std::uint64_t node_id = path_view.node_ids()[idx];
          bucket_t& bucket = storage[node_id];
          auto& epoch = epochs[node_id];

          const std::size_t bucket_index = static_cast<std::size_t>(level - non_overlap_start_level);
          sn::util::span<const block_t> real_span(
              subpath_span.begin() + bucket_index * bucket_real_size, bucket_real_size
          );
          bucket.rebuild(real_span, uid_gen, prng);
          // publish bucket sync metadata after rebuild
          epoch.publish_after_eviction_rebuild();
        }
      }
    });
    plan.subtree_active_subpaths[subtree_ix] = 0;
  };

  const std::size_t logical_workers = workers.logical_threads();
  if (logical_workers <= 1) {
    auto& ctx = st.eviction_scratch_for_worker(0);
    for (std::uint32_t subtree_ix = 0; subtree_ix < static_cast<std::uint32_t>(subtree_count); ++subtree_ix) {
      rebuild_subtree(static_cast<std::uint32_t>(subtree_ix), ctx);
    }
  } else {
    workers.parallel_work([&, logical_workers](std::size_t logical_ix) noexcept {
      const auto [begin, end] = sn::threads::partition_evenly(logical_ix, subtree_count, logical_workers);
      if (begin == end) {
        return;
      }
      auto& ctx = st.eviction_scratch_for_worker(logical_ix);
      for (std::size_t ix = begin; ix < end; ++ix) {
        rebuild_subtree(static_cast<std::uint32_t>(ix), ctx);
      }
    });
  }

#if defined(ORAM_DEBUG)
  st.log().dbgf("evict: rebuilt buckets for %d subtrees", subtree_count);
#endif
}

} // namespace detail

// perform eviction on scheduled paths
template <typename Traits> void evict(state<Traits>& st, schedule& sched, sn::threads::thread_team& workers) {
  sn_prof_zone("zingoram.eviction");
  using stash_t = typename Traits::stash_t;
  st.ensure_eviction_scratch_capacity(workers.logical_threads());

  // take the batch of evict leaves
  auto leaves = sched.take_evict_leaves();
  st.metrics_ref().record_evict(static_cast<std::uint64_t>(leaves.size()));

#if defined(ORAM_DEBUG)
  // ensure we get the exact right number of evict leaves across the tree
  sn::util::log::ensure(!leaves.empty(), "zingoram::evict: empty eviction leaves");
  sn::util::log::ensure(
      leaves.size() == st.derived().batch_eviction_factor, "zingoram::evict: unexpected eviction leaf count"
  );
#endif

  auto& geom = st.shape();
  auto& opts = st.options();
  auto& stash = st.stash();

  const std::size_t bucket_real_size = static_cast<std::size_t>(opts.bucket_real_size);
  const std::uint64_t start_level = static_cast<std::uint64_t>(geom.routing_depth) + geom.overlap_depth;
  const std::size_t non_overlap_height = static_cast<std::size_t>(geom.non_overlapping_subpath_height);
  const std::size_t per_subpath_real_blocks = non_overlap_height * bucket_real_size;
  const std::uint64_t non_overlap_start_level = start_level;
#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      non_overlap_height == static_cast<std::size_t>(geom.height + 1 - non_overlap_start_level),
      "zingoram::evict: inconsistent non-overlap geometry"
  );
  sn::util::log::ensure(
      per_subpath_real_blocks == non_overlap_height * bucket_real_size, "zingoram::evict: per-subpath mismatch"
  );
#endif
  const std::size_t subpaths_per_subtree = static_cast<std::size_t>(opts.evict_batch);

  // get eviction plan buffers
  auto& plan = st.eviction_plan();

#if defined(ORAM_DEBUG)
  st.log().trcf("evict: leaves=%s", sn::util::format::format_vec(leaves));
#endif

  detail::prepare_eviction_plan<Traits>(
      st, sn::util::span<const std::uint64_t>(leaves.data(), leaves.size()), subpaths_per_subtree, plan
  );

  if (opts.evict_backend == eviction_backend::shared_top && opts.evict_batch > 1) {
    detail::evict_shared_top<Traits>(st, plan, workers);
    return;
  }

  detail::stage_subtree_evictslots<Traits>(
      st, plan, workers, subpaths_per_subtree, bucket_real_size, non_overlap_height, per_subpath_real_blocks,
      non_overlap_start_level
  );

  using stash_plan_view = typename stash_t::eviction_plan_view;
  stash_plan_view plan_view{
      sn::util::span<std::uint32_t>(plan.subtree_active_subpaths.data(), plan.subtree_active_subpaths.size()),
      sn::util::span<std::uint64_t>(plan.subtree_leaves.data(), plan.subtree_leaves.size()), subpaths_per_subtree
  };

  fz_stash::evict_to_paths(stash, plan_view, workers);

  detail::rebuild_subtrees<Traits>(
      st, plan, workers, subpaths_per_subtree, bucket_real_size, per_subpath_real_blocks, non_overlap_start_level
  );
}

} // namespace sn::oram::zingoram
