#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/crypto/prng.hpp"
#include "sonic/crypto/random.hpp"
#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/stash/core/lock_free_block_storage.hpp"
#include "sonic/oram/stash/forestzing/layout.hpp"
#include "sonic/oram/stash/forestzing/metrics.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/oram/tree/topology.hpp"
#include "sonic/oram/tree/forest_topology.hpp"
#include "sonic/threads/sync.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

namespace sn::oram::stash::forestzing {

struct stash_subtree_limits {
  std::uint64_t routing_input_blocks = 0;
  std::uint64_t routing_output_blocks = 0;
  std::uint64_t sort_size = 0;
  std::uint64_t max_real_blocks = 0;
  std::uint64_t max_treetop_blocks = 0;
  std::uint64_t overlap_region_blocks = 0;
  std::uint64_t max_unevicted_deferred = 0;
  std::uint64_t dummy_slots = 0;
};

struct stash_geometry {
  std::uint64_t leaf_count = 0;
  std::uint64_t subtree_leaf_count = 0;
  std::uint64_t subtree_height = 0;
  std::uint64_t tree_height = 0;
  std::uint32_t subtree_count = 0;
  std::uint32_t routing_depth = 0;
  std::uint64_t overlap_depth = 0;
  std::uint64_t non_overlap_height = 0;
  std::uint64_t bucket_real = 0;
  std::uint64_t evict_batch = 0;
  subtree_section_sizes section_sizes{};
  stash_subtree_limits limits{};
};

template <typename Block> struct stash_runtime {
  struct worker_scratch {
    std::vector<Block> routing_buffer;
    std::vector<Block> kept_aside_buffer;
    std::vector<std::uint8_t> marks;
    std::vector<std::size_t> prefix;
    std::vector<std::uint64_t> path_leaf_nodes;
    std::vector<std::uint64_t> path_leaf_indices;
    std::unique_ptr<sn::crypto::buffered_prng<>> prng{};
    sn::oram::tree::topology vtree_topology{};
    sn::oram::tree::topology subtree_topology{};
    sn::oram::tree::forest_topology forest_topology{};

    [[nodiscard]] sn::util::span<Block> routing_span() noexcept {
      return sn::util::span<Block>(routing_buffer.data(), routing_buffer.size());
    }

    [[nodiscard]] sn::util::span<Block> kept_aside_span() noexcept {
      return sn::util::span<Block>(kept_aside_buffer.data(), kept_aside_buffer.size());
    }

    [[nodiscard]] sn::util::span<std::uint8_t> marks_span() noexcept {
      return sn::util::span<std::uint8_t>(marks.data(), marks.size());
    }

    [[nodiscard]] sn::util::span<std::size_t> prefix_span() noexcept {
      return sn::util::span<std::size_t>(prefix.data(), prefix.size());
    }

    [[nodiscard]] sn::util::span<std::uint64_t> path_leaf_nodes_span() noexcept {
      return sn::util::span<std::uint64_t>(path_leaf_nodes.data(), path_leaf_nodes.size());
    }

    [[nodiscard]] sn::util::span<std::uint64_t> path_leaf_indices_span() noexcept {
      return sn::util::span<std::uint64_t>(path_leaf_indices.data(), path_leaf_indices.size());
    }
  };

  std::vector<Block> snapshot_buffer;
  std::size_t snapshot_count = 0;
  bool snapshot_prefilled = false;
  std::vector<worker_scratch> worker_buffers;
  std::vector<std::uint32_t> leaf_region_to_revlex_path_ix;

  [[nodiscard]] sn::util::span<Block> snapshot_span() noexcept {
    return sn::util::span<Block>(snapshot_buffer.data(), snapshot_buffer.size());
  }

  [[nodiscard]] sn::util::span<const Block> snapshot_span() const noexcept {
    return sn::util::span<const Block>(snapshot_buffer.data(), snapshot_buffer.size());
  }

  void initialize(
      const stash_geometry& geom, std::size_t snapshot_capacity, sn::oram::uid_generator& uid,
      std::size_t initial_worker_capacity
  ) {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        initial_worker_capacity > 0, "forestzing::stash_runtime: initial worker capacity must be > 0"
    );
#endif
    reset_snapshot_buffer(snapshot_capacity, uid);
    worker_buffers.clear();
    ensure_worker_capacity(initial_worker_capacity, geom, uid);
  }

  void ensure_worker_capacity(std::size_t required, const stash_geometry& geom, sn::oram::uid_generator& uid) {
    prepare_region_mapping(geom);
    if (worker_buffers.size() >= required) {
      return;
    }
    const std::size_t sort_capacity = static_cast<std::size_t>(geom.limits.sort_size);
    const std::size_t routing_capacity = static_cast<std::size_t>(geom.limits.routing_input_blocks);
    const std::size_t compact_capacity = std::max(sort_capacity, routing_capacity);
    const std::size_t leaves_capacity = static_cast<std::size_t>(geom.evict_batch);
    const std::size_t previous = worker_buffers.size();

    worker_buffers.resize(required);
    sn::crypto::random_device rd;

    for (std::size_t ix = previous; ix < required; ++ix) {
      auto& scratch = worker_buffers[ix];
      scratch.routing_buffer.resize(routing_capacity);
      const std::size_t levels_per_path = static_cast<std::size_t>(geom.subtree_height + 1ULL);
      const std::size_t evicted_capacity =
          static_cast<std::size_t>(geom.evict_batch) * levels_per_path * static_cast<std::size_t>(geom.bucket_real);
      const std::size_t retained_capacity =
          static_cast<std::size_t>(geom.section_sizes.treetop + geom.section_sizes.local_deferred);
      const std::size_t kept_capacity = evicted_capacity + retained_capacity;
      scratch.kept_aside_buffer.resize(kept_capacity);
      for (auto& blk : scratch.routing_buffer) {
        blk.set_dummy(uid);
      }
      for (auto& blk : scratch.kept_aside_buffer) {
        blk.set_dummy(uid);
      }
      scratch.marks.assign(compact_capacity, std::uint8_t{0});
      scratch.prefix.assign(compact_capacity + 1, std::size_t{0});
      scratch.path_leaf_nodes.assign(leaves_capacity, 0);
      scratch.path_leaf_indices.assign(leaves_capacity, 0);
      scratch.prng = std::make_unique<sn::crypto::buffered_prng<>>();
      scratch.prng->reseed(sn::crypto::prng::make_seed(rd));
      scratch.vtree_topology.reset(geom.tree_height);
      scratch.subtree_topology.reset(geom.subtree_height);
      scratch.forest_topology.reset(geom.tree_height, static_cast<std::uint32_t>(geom.routing_depth));
    }
  }

  [[nodiscard]] std::uint64_t region_to_revlex_path_ix(std::uint64_t region_ix) const {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(
        region_ix < leaf_region_to_revlex_path_ix.size(),
        "forestzing::stash_runtime: region_to_revlex_path_ix out of range"
    );
#endif
    return leaf_region_to_revlex_path_ix[static_cast<std::size_t>(region_ix)];
  }

private:
  void reset_snapshot_buffer(std::size_t capacity, sn::oram::uid_generator& uid) {
    snapshot_buffer.resize(capacity);
    snapshot_count = 0;
    snapshot_prefilled = false;
    for (auto& block : snapshot_buffer) {
      block.set_dummy(uid);
    }
  }

  void prepare_region_mapping(const stash_geometry& geom) {
    if (!leaf_region_to_revlex_path_ix.empty()) {
      return;
    }
    const std::uint64_t region_count = static_cast<std::uint64_t>(geom.evict_batch);
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(region_count > 0, "forestzing::stash_runtime: evict batch must be positive");
    sn::util::log::ensure(
        (region_count & (region_count - 1ULL)) == 0,
        "forestzing::stash_runtime: evict batch expected to be power of two"
    );
#endif
    if (region_count == 0) {
      return;
    }
    leaf_region_to_revlex_path_ix.resize(static_cast<std::size_t>(region_count));
    // region index tells us which sub-span of the subtree leaves a leaf falls under
    // so we must convert the region index to the reverse lexicographic path index
    // the number of regions is equal to the evict batch size
    // we just have to apply the order permutation to the region index
    const std::uint64_t revlex_height = static_cast<std::uint64_t>(sn::obliv::ct_log2(region_count));
    sn::oram::tree::topology revlex_helper(revlex_height);
    // simulate a tree with region_ix leaves and use our revlex path utility
    for (std::uint64_t revlex_counter = 0; revlex_counter < region_count; ++revlex_counter) {
      const std::uint64_t revlex_region_ix = revlex_helper.reverse_lex_leaf(revlex_counter);
      leaf_region_to_revlex_path_ix[static_cast<std::size_t>(revlex_region_ix)] =
          static_cast<std::uint32_t>(revlex_counter);
    }
  }
};

template <typename Block> struct stash_state {
  struct alignas(64) subtree_extract_lock {
    sn::threads::mutex mutex{};
  };

  sn::util::log::logger log{};
  std::optional<std::reference_wrapper<sn::oram::uid_generator>> uid_generator{};
  std::unique_ptr<sn::oram::stash::core::lock_free_block_storage<Block>> global_stash;
  std::vector<subtree_storage<Block>> subtrees{};
  std::unique_ptr<subtree_extract_lock[]> subtree_extract_locks{};
  std::size_t subtree_extract_lock_count = 0;
  stash_geometry geom{};
  stash_runtime<Block> runtime{};
#if defined(SONIC_ORAM_METRICS)
  stash_metrics metrics{};
#endif
  bool disjoint_epoch_mode = false;

  [[nodiscard]] sn::oram::uid_generator& uid() {
    sn::util::log::ensure(uid_generator.has_value(), "forestzing::stash_state: uid generator uninitialized");
    return uid_generator->get();
  }

  [[nodiscard]] const sn::oram::uid_generator& uid() const {
    sn::util::log::ensure(uid_generator.has_value(), "forestzing::stash_state: uid generator uninitialized");
    return uid_generator->get();
  }

  [[nodiscard]] sn::threads::mutex& subtree_extract_mutex(std::uint32_t subtree_ix) {
    sn::util::log::ensure(
        subtree_extract_locks != nullptr, "forestzing::stash_state: subtree extract locks uninitialized"
    );
    sn::util::log::ensure(
        static_cast<std::size_t>(subtree_ix) < subtree_extract_lock_count,
        "forestzing::stash_state: subtree extract lock index out of range"
    );
    return subtree_extract_locks[static_cast<std::size_t>(subtree_ix)].mutex;
  }
};

} // namespace sn::oram::stash::forestzing
