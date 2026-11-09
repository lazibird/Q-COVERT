#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <memory>
#include <vector>
#include <type_traits>

#include "sonic/util/log.hpp"
#include "sonic/util/humanize.hpp"
#include "sonic/util/span.hpp"
#include "sonic/obliv/ops/platform_ops.hpp"

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/oram/stash/forestzing/stash.hpp"
#include "sonic/oram/tree/overlap.hpp"
#include "sonic/oram/tree/bucket_heap.hpp"
#include "sonic/oram/tree/path_buffer.hpp"
#include "sonic/oram/tree/topology.hpp"
#include "sonic/oram/zingoram/metrics.hpp"
#include "sonic/oram/zingoram/analysis.hpp"
#include "sonic/oram/zingoram/options.hpp"
#include "sonic/oram/zingoram/detail/epoch_state.hpp"
#include "sonic/oram/zingoram/detail/bucket_metadata_arena.hpp"
#include "sonic/oram/zingoram/allocator.hpp"
#if defined(ORAM_DEBUG)
#include "sonic/oram/tree/assigned_block_map.hpp"
#endif
#include <optional>

namespace sn::oram::zingoram {

namespace fz_stash = sn::oram::stash::forestzing;

template <typename Traits> class state {
public:
  using options_t = typename Traits::options_t;
  using block_t = typename Traits::block_t;
  using bucket_t = typename Traits::bucket_t;
  using stash_t = typename Traits::stash_t;
  using stash_config = typename stash_t::config;
  static constexpr bool disjoint_epoch_mode = Traits::disjoint_epoch_mode;

  struct geometry {
    std::uint64_t height = 0;
    std::uint64_t leaf_count = 0;
    std::uint64_t node_count = 0;
    std::uint64_t bucket_total_size = 0;
    std::uint64_t path_block_count = 0;
    std::uint64_t path_real_block_count = 0;
    std::uint32_t routing_depth = 0;
    std::uint32_t subtree_count = 1;
    std::uint64_t subtree_height = 0;
    std::uint64_t subtree_leaf_count = 0;
    std::uint64_t subpath_real_block_count = 0;
    std::uint64_t subpath_nonexistent_real_block_count = 0;
    std::uint64_t overlap_depth = 0;
    std::uint64_t non_overlapping_subpath_height = 0;
  };

  struct derived_parameters {
    // stash bound for the selected eviction schedule
    std::uint64_t stash_bound = 0;
    // number of pathreads (accesses between evictions)
    std::uint64_t num_pathreads = 0;
    // base eviction rate before batch scaling
    std::uint64_t base_eviction_rate = 0;
    // number of paths to evict per round (forest batch eviction + multi-path batch eviction)
    std::uint64_t batch_eviction_factor = 0;
    // eviction rate adjusted for batch eviction (A')
    std::uint64_t adjusted_eviction_rate = 0;
    // whether access concurrency is single-threaded
    bool single_thread_access = false;
  };

  struct access_scratch {
    void configure(const geometry& shape, std::uint32_t bucket_real_size) {
      path.configure(shape.height, shape.bucket_total_size);
      bucket_real_buf.assign(bucket_real_size, block_t{});
      bucket_offset_buf.assign(bucket_real_size, 0);
    }

    sn::oram::tree::path_buffer<block_t> path;
    std::vector<block_t> bucket_real_buf;
    std::vector<std::int64_t> bucket_offset_buf;
  };

  struct eviction_scratch {
    void configure(const geometry& shape, std::uint32_t bucket_real_size) {
      path.configure(shape.height, shape.bucket_total_size);
      bucket_real_buf.assign(bucket_real_size, block_t{});
      bucket_offset_buf.assign(bucket_real_size, 0);
      const std::uint64_t path_real_slots =
          shape.non_overlapping_subpath_height * static_cast<std::uint64_t>(bucket_real_size);
      evict_path_blocks.assign(path_real_slots, block_t{});
    }

    sn::oram::tree::path_buffer<block_t> path;
    std::vector<block_t> bucket_real_buf;
    std::vector<std::int64_t> bucket_offset_buf;
    std::vector<block_t> evict_path_blocks;
  };

  struct eviction_plan_buffers {
    std::vector<std::uint64_t> subtree_leaves;
    std::vector<std::uint32_t> subtree_active_subpaths;

    sn::util::span<std::uint64_t> subtree_slot(std::size_t subtree_ix, std::size_t paths_per_subtree) noexcept {
      return sn::util::span<std::uint64_t>(subtree_leaves.data() + subtree_ix * paths_per_subtree, paths_per_subtree);
    }
  };

  explicit state(options_t opts, sn::util::log::logger logger = sn::util::log::create("zingoram:state")) :
      options_(std::move(opts)),
      geom_(derive_geometry(options_)),
      derived_(derive_parameters(options_, geom_)),
      uid_gen_(),
      prng_(),
      topology_(geom_.height, 2),
      forest_topology_(geom_.height, geom_.routing_depth),
      storage_(),
      logger_(std::move(logger)),
      stash_(
          fz_stash::make_stash<block_t>(
              derive_stash_config(options_, geom_, derived_), topology_, uid_gen_, logger_.child("stash")
          )
      ) {
    validate_epoch_options(options_, derived_);
    configure_eviction_plan_buffers();
#if defined(ORAM_DEBUG)
    assigned_blocks_.configure(options_.block_count);
#endif
  }

  void initialize(bool populate_buckets = true) {
    uid_gen_.reset();
    forest_topology_.reset(geom_.height, geom_.routing_depth);

    const std::uint32_t slot_count = static_cast<std::uint32_t>(geom_.bucket_total_size);
    const std::size_t n_alloc_nodes = static_cast<std::size_t>(geom_.node_count + 1ULL);

    // use allocator to plan storage layout
    allocator_.emplace(topology_, geom_.node_count, geom_.height, slot_count, logger_);

    // initialize bucket storage
    initialize_buckets(allocator_->bucket_ptrs(), slot_count, populate_buckets);

    auto make_epoch = [this](std::uint64_t node_id) {
      sn::oram::zingoram::detail::bucket_epoch_state e{};
      if (node_id != 0) {
        e.configure(options_.bucket_dummy_size);
      }
      return e;
    };
    epoch_states_.initialize(geom_.node_count, make_epoch, uid_gen_, "bucket_heap:epoch");

    sn::obliv::fill(eviction_plan_.subtree_active_subpaths.begin(), eviction_plan_.subtree_active_subpaths.end(), 0);

#if defined(ORAM_DEBUG)
    assigned_blocks_.reset();
#endif

    refresh_eviction_scratch();
    ensure_eviction_scratch_capacity(default_eviction_scratch_slots());
    metrics_.reset();
  }

  const geometry& shape() const noexcept { return geom_; }
  geometry& shape() noexcept { return geom_; }

  const options_t& options() const noexcept { return options_; }
  options_t& options() noexcept { return options_; }

  const derived_parameters& derived() const noexcept { return derived_; }

  void configure_access_scratch(access_scratch& scratch) const { scratch.configure(geom_, options_.bucket_real_size); }

  void configure_eviction_scratch(eviction_scratch& scratch) const {
    scratch.configure(geom_, options_.bucket_real_size);
  }

  void refresh_eviction_scratch() {
    for (auto& scratch : eviction_scratch_) {
      configure_eviction_scratch(scratch);
    }
  }

  void ensure_eviction_scratch_capacity(std::size_t slots) {
    const std::size_t needed = std::max<std::size_t>(slots, std::size_t{1});
    if (eviction_scratch_.size() < needed) {
      const std::size_t prev = eviction_scratch_.size();
      eviction_scratch_.resize(needed);
      for (std::size_t i = prev; i < needed; ++i) {
        configure_eviction_scratch(eviction_scratch_[i]);
      }
    }
  }

  [[nodiscard]] std::size_t eviction_scratch_capacity() const noexcept { return eviction_scratch_.size(); }

  [[nodiscard]] eviction_scratch& eviction_scratch_for_worker(std::size_t index) {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(index < eviction_scratch_.size(), "zingoram::state: eviction scratch index out of range");
#endif
    return eviction_scratch_[index];
  }

  eviction_plan_buffers& eviction_plan() noexcept { return eviction_plan_; }
  const eviction_plan_buffers& eviction_plan() const noexcept { return eviction_plan_; }

  std::uint32_t bucket_total_size() const noexcept { return static_cast<std::uint32_t>(geom_.bucket_total_size); }

  sn::oram::tree::topology& topology() noexcept { return topology_; }
  const sn::oram::tree::topology& topology() const noexcept { return topology_; }

  sn::oram::tree::forest_topology& forest_topology() noexcept { return forest_topology_; }
  const sn::oram::tree::forest_topology& forest_topology() const noexcept { return forest_topology_; }

  sn::oram::tree::bucket_heap<bucket_t>& storage() noexcept { return storage_; }
  const sn::oram::tree::bucket_heap<bucket_t>& storage() const noexcept { return storage_; }

  sn::oram::tree::bucket_heap<sn::oram::zingoram::detail::bucket_epoch_state>& epoch_states() noexcept {
    return epoch_states_;
  }
  const sn::oram::tree::bucket_heap<sn::oram::zingoram::detail::bucket_epoch_state>& epoch_states() const noexcept {
    return epoch_states_;
  }
  metrics& metrics_ref() noexcept { return metrics_; }
  const metrics& metrics_ref() const noexcept { return metrics_; }
  [[nodiscard]] sn::oram::zingoram::metrics_snapshot metrics_snapshot() const noexcept { return metrics_.snapshot(); }
  void reset_metrics() noexcept { metrics_.reset(); }
  [[nodiscard]] typename stash_t::metrics_snapshot stash_metrics_snapshot() const noexcept {
    return fz_stash::metrics_snapshot(stash_);
  }
  void reset_stash_metrics() noexcept { fz_stash::reset_metrics(stash_); }
  template <class Writer> void save_initial(Writer& writer) const {
    // Forest ancestors are virtual and have no initialized ciphertext slots.
    for (std::uint64_t node = 1ULL << geom_.routing_depth; node <= geom_.node_count; ++node)
      storage_[node].save_initial(writer);
    for (const auto& subtree : stash_.state.subtrees) {
      const auto blocks = subtree.storage.span();
      writer.write(blocks.data(), blocks.size() * sizeof(block_t));
    }
  }
  template <class Reader> void load_initial(Reader& reader) {
    initialize(false);
    for (std::uint64_t node = 1ULL << geom_.routing_depth; node <= geom_.node_count; ++node)
      storage_[node].load_initial(reader);
    for (auto& subtree : stash_.state.subtrees) {
      auto blocks = subtree.storage.span();
      reader.read(blocks.data(), blocks.size() * sizeof(block_t));
    }
  }
  sn::util::log::logger& log() noexcept { return logger_; }
  const sn::util::log::logger& log() const noexcept { return logger_; }

  stash_t& stash() noexcept { return stash_; }
  const stash_t& stash() const noexcept { return stash_; }

  sn::oram::uid_generator& uid_gen() noexcept { return uid_gen_; }
  const sn::oram::uid_generator& uid_gen() const noexcept { return uid_gen_; }

  sn::crypto::buffered_prng<>& prng() noexcept { return prng_; }
  const sn::crypto::buffered_prng<>& prng() const noexcept { return prng_; }

#if defined(ORAM_DEBUG)
  sn::oram::tree::assigned_block_map& assigned_blocks() noexcept { return assigned_blocks_; }
  const sn::oram::tree::assigned_block_map& assigned_blocks() const noexcept { return assigned_blocks_; }
#endif

private:
  void initialize_buckets(const std::vector<block_t*>& bucket_ptrs, std::uint32_t slot_count,
                          bool populate_buckets) {
    using store_t = typename Traits::block_store_t;

    // prepare metadata slab
    const auto meta_bd =
        detail::bucket_metadata_arena::planned(geom_.node_count, options_.bucket_real_size, options_.bucket_dummy_size);
    logger_.trcf(
        "zingoram::state: bucket metadata arena total=%s (per_bucket=%s valids=%s perm=%s realaddr=%s shuffle=%s)",
        sn::util::humanize::bytes(static_cast<std::uint64_t>(meta_bd.total)),
        sn::util::humanize::bytes(static_cast<std::uint64_t>(meta_bd.per_bucket)),
        sn::util::humanize::bytes(static_cast<std::uint64_t>(meta_bd.valids)),
        sn::util::humanize::bytes(static_cast<std::uint64_t>(meta_bd.permutation)),
        sn::util::humanize::bytes(static_cast<std::uint64_t>(meta_bd.real_addresses)),
        sn::util::humanize::bytes(static_cast<std::uint64_t>(meta_bd.shuffle))
    );
    metadata_arena_.configure(geom_.node_count, options_.bucket_real_size, options_.bucket_dummy_size);

    // base ptr of bucket blocks: for slab storage
    auto bucket_base_ptr = [&bucket_ptrs](std::uint64_t node_id) -> block_t* {
      return bucket_ptrs[static_cast<std::size_t>(node_id)];
    };

    // construct a bucket
    auto build_bucket = [this, &bucket_base_ptr, populate_buckets](auto&& make_store, std::uint64_t node_id) {
      if (node_id == 0) {
        return bucket_t{};
      }

      const std::uint32_t level = static_cast<std::uint32_t>(topology_.node_depth(node_id));
      block_t* base = bucket_base_ptr(node_id);
      store_t store = make_store(node_id, base);

      auto meta = metadata_arena_.view(static_cast<std::size_t>(node_id));
      bucket_t bucket(
          node_id, level, options_.bucket_real_size, options_.bucket_dummy_size, std::move(store), std::move(meta)
      );
      if (populate_buckets) bucket.initialize(uid_gen_, prng_);
      return bucket;
    };

    auto make_store = [slot_count](std::uint64_t node, block_t* base) {
      store_t store{};
      if constexpr (requires { store.configure_node(node, slot_count); })
        store.configure_node(node, slot_count);
      else store.configure(base, slot_count);
      return store;
    };

    auto make_bucket = [&](std::uint64_t node_id) { return build_bucket(make_store, node_id); };
    storage_.initialize(geom_.node_count, make_bucket, uid_gen_, "bucket_heap:bucket", populate_buckets);
  }

  static std::uint64_t derive_base_eviction_rate(const options_t& opts) {
    const auto eviction_rate = static_cast<std::uint64_t>(opts.eviction_rate);
    if (eviction_rate != 0) {
      return eviction_rate;
    }

    // default: use the theoretical maximum based on Z
    return analysis::max_eviction_rate(opts.bucket_real_size);
  }

  static std::uint64_t derive_stash_bound(const options_t& opts, std::uint64_t base_eviction_rate) {
    const auto stash_bound = opts.stash_bound;
    if (stash_bound != 0) {
      return stash_bound;
    }

    // default: use the theoretical minimum for target failure probability
    constexpr std::uint64_t k_security_parameter = 80;
    return analysis::stash_bound(opts.bucket_real_size, base_eviction_rate, k_security_parameter);
  }

  static geometry derive_geometry(const options_t& opts) {
    namespace log = sn::util::log;

    log::ensure(opts.block_count > 0, "zingoram::state: block_count must be positive");
    log::ensure(opts.bucket_real_size > 0, "zingoram::state: bucket_real_size must be positive");
    log::ensure(opts.bucket_dummy_size > 0, "zingoram::state: bucket_dummy_size must be positive");
    log::ensure(opts.evict_batch > 0, "zingoram::state: evict_batch must be positive");

    geometry geom{};
    geom.routing_depth = opts.routing_depth;
    geom.bucket_total_size = static_cast<std::uint64_t>(opts.bucket_real_size + opts.bucket_dummy_size);

    const std::uint64_t base_eviction_rate = derive_base_eviction_rate(opts);
    geom.height = analysis::zingoram_tree_height(opts.block_count, base_eviction_rate);

    log::ensure(geom.height < 63, "zingoram::state: tree height too large");
    log::ensure(geom.routing_depth <= geom.height, "zingoram::state: routing_depth exceeds height");
    log::ensure(geom.routing_depth < 32, "zingoram::state: routing_depth exceeds subtree count representation");

    geom.leaf_count = 1ULL << geom.height;
    geom.node_count = (geom.leaf_count << 1U) - 1U;

    const std::uint64_t bucket_real = static_cast<std::uint64_t>(opts.bucket_real_size);
    // total blocks along a path
    geom.path_block_count = (geom.height + 1ULL) * geom.bucket_total_size;
    // real blocks along a path
    geom.path_real_block_count = (geom.height + 1ULL) * bucket_real;

    geom.subtree_count = 1U << geom.routing_depth;
    geom.subtree_height = geom.height - geom.routing_depth;
    geom.subtree_leaf_count = 1ULL << geom.subtree_height;

    // nonexistent real blocks, which are above the routing depth
    geom.subpath_nonexistent_real_block_count = bucket_real * static_cast<std::uint64_t>(geom.routing_depth);
    log::ensure(
        geom.path_real_block_count >= geom.subpath_nonexistent_real_block_count,
        "zingoram::state: subpath_real_block_count underflow"
    );
    // real blocks along a subtree path
    geom.subpath_real_block_count = geom.path_real_block_count - geom.subpath_nonexistent_real_block_count;

    // depth of overlap for multi-path batch eviction
    geom.overlap_depth =
        sn::oram::tree::tree_overlap::evict_batch_size_to_overlap_depth(static_cast<std::size_t>(opts.evict_batch));
    // height of the non-overlapping part of each subtree path
    const std::uint64_t tree_height_plus_one = geom.height + 1ULL;
    log::ensure(
        tree_height_plus_one >= geom.routing_depth + geom.overlap_depth,
        "zingoram::state: invalid non-overlapping subtree height"
    );
    geom.non_overlapping_subpath_height = tree_height_plus_one - geom.routing_depth - geom.overlap_depth;

    return geom;
  }

  static derived_parameters derive_parameters(const options_t& opts, const geometry& geom) {
    namespace log = sn::util::log;

    derived_parameters params{};
    params.base_eviction_rate = derive_base_eviction_rate(opts);
    params.stash_bound = derive_stash_bound(opts, params.base_eviction_rate);

    log::ensure(geom.subtree_count > 0, "zingoram::state: subtree_count must be positive");
    log::ensure(opts.evict_batch > 0, "zingoram::state: evict_batch must be positive");

    // compute eviction rate and patch count adjusted for batch eviction
    params.batch_eviction_factor = geom.subtree_count * static_cast<std::uint64_t>(opts.evict_batch);
    log::ensure(params.batch_eviction_factor > 0, "zingoram::state: batch_eviction_factor must be positive");
    log::ensure(params.batch_eviction_factor <= UINT32_MAX, "zingoram::state: eviction schedule too large");
    params.adjusted_eviction_rate = params.base_eviction_rate * params.batch_eviction_factor;

    // compute number of pathreads (accesses between evictions)
    params.num_pathreads = params.adjusted_eviction_rate;
    log::ensure(params.num_pathreads > 0, "zingoram::state: num_pathreads must be positive");

    params.single_thread_access = opts.access_concurrency <= 1;

    return params;
  }

  options_t options_{};
  geometry geom_{};
  derived_parameters derived_{};
  sn::oram::uid_generator uid_gen_{};
  sn::crypto::buffered_prng<> prng_{};
  sn::oram::tree::topology topology_{};
  sn::oram::tree::forest_topology forest_topology_{};
  metrics metrics_{};
  sn::oram::tree::bucket_heap<bucket_t> storage_{};
  sn::oram::tree::bucket_heap<sn::oram::zingoram::detail::bucket_epoch_state> epoch_states_{};
  sn::util::log::logger logger_;
  stash_t stash_;
  eviction_plan_buffers eviction_plan_{};
#if defined(ORAM_DEBUG)
  sn::oram::tree::assigned_block_map assigned_blocks_;
#endif
  std::vector<eviction_scratch> eviction_scratch_{};
  std::optional<sn::oram::zingoram::allocator<block_t, typename Traits::block_store_t>> allocator_{};
  detail::bucket_metadata_arena metadata_arena_{};

  static stash_config derive_stash_config(
      const options_t& opts, const geometry& geom, const derived_parameters& derived
  ) {
    stash_config cfg{};
    cfg.tree.height = geom.height;
    cfg.tree.routing_depth = geom.routing_depth;
    cfg.tree.subtree_height = geom.subtree_height;
    cfg.tree.subtree_count = geom.subtree_count;
    cfg.tree.subpath_real_block_count = geom.subpath_real_block_count;
    cfg.tree.subpath_nonexistent_real_block_count = geom.subpath_nonexistent_real_block_count;
    cfg.shared_top_backend = opts.evict_backend == eviction_backend::shared_top && opts.evict_batch > 1;
    cfg.tree.overlap_depth = cfg.shared_top_backend ? 0ULL : geom.overlap_depth;
    cfg.tree.non_overlapping_subpath_height =
        cfg.shared_top_backend ? geom.subtree_height + 1ULL : geom.non_overlapping_subpath_height;

    cfg.bucket.real = static_cast<std::uint64_t>(opts.bucket_real_size);
    cfg.bucket.dummy = static_cast<std::uint64_t>(opts.bucket_dummy_size);

    cfg.limits.stash_bound = derived.stash_bound;
    cfg.limits.num_pathreads = derived.num_pathreads;
    cfg.limits.eviction_rate = derived.base_eviction_rate;
    cfg.limits.evict_batch = static_cast<std::uint64_t>(opts.evict_batch);
    cfg.limits.evictslots_per_path = cfg.tree.non_overlapping_subpath_height * cfg.bucket.real;
    sn::util::log::ensure(cfg.limits.evictslots_per_path > 0, "zingoram::state: evictslots_per_path must be positive");

    cfg.balls_and_bins_security = 80;
    cfg.shape.subtree_count = geom.subtree_count;
    cfg.disjoint_epoch_mode = disjoint_epoch_mode;
    cfg.single_thread_access = derived.single_thread_access;
    return cfg;
  }

  void configure_eviction_plan_buffers() {
    const std::size_t subtree_count = static_cast<std::size_t>(geom_.subtree_count);
    if (subtree_count == 0) {
      eviction_plan_.subtree_leaves.clear();
      eviction_plan_.subtree_active_subpaths.clear();
      return;
    }

    sn::util::log::ensure(options_.evict_batch > 0, "zingoram::state: evict_batch must be positive");
    eviction_plan_.subtree_leaves.assign(subtree_count * static_cast<std::size_t>(options_.evict_batch), 0);
    eviction_plan_.subtree_active_subpaths.assign(subtree_count, 0);
  }

  static void validate_epoch_options(const options_t& opts, const derived_parameters& derived) {
    if constexpr (disjoint_epoch_mode) {
      sn::util::log::ensure(opts.disjoint_epoch_window > 0, "zingoram::state: disjoint_epoch_window must be positive");
      sn::util::log::ensure(
          opts.disjoint_epoch_window % derived.num_pathreads == 0,
          "zingoram::state: disjoint_epoch_window must be a multiple of num_pathreads"
      );
    } else {
      sn::util::log::ensure(
          opts.disjoint_epoch_window == 0, "zingoram::state: disjoint_epoch_window must be zero in default epoch mode"
      );
    }
  }

  [[nodiscard]] std::size_t default_eviction_scratch_slots() const noexcept {
    return std::max<std::size_t>(geom_.subtree_count, 1);
  }
};

} // namespace sn::oram::zingoram
