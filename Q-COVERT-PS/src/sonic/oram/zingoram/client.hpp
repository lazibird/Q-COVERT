#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include <optional>

#include "sonic/oram/core/access_ops.hpp"

#include "sonic/oram/zingoram/access.hpp"
#include "sonic/oram/zingoram/bulk_setup.hpp"
#include "sonic/oram/zingoram/detail/access_gate.hpp"
#include "sonic/oram/zingoram/disjoint_state.hpp"
#include "sonic/oram/zingoram/eviction.hpp"
#include "sonic/oram/zingoram/schedule.hpp"
#include "sonic/oram/zingoram/state.hpp"
#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"
#include "sonic/threads/thread_team.hpp"

namespace sn::oram::zingoram {

namespace fz_stash = sn::oram::stash::forestzing;

template <typename Traits> class client {
public:
  using options_t = typename Traits::options_t;
  using state_type = state<Traits>;
  using block_t = typename Traits::block_t;
  using access_scratch = typename state_type::access_scratch;
  static constexpr bool disjoint_epoch_mode = Traits::disjoint_epoch_mode;

  client(options_t opts, sn::threads::thread_team eviction_team) :
      state_(std::move(opts)),
      gate_(state_.derived().adjusted_eviction_rate),
      eviction_schedule_(state_.topology(), static_cast<std::uint32_t>(state_.derived().batch_eviction_factor)),
      eviction_workers_(std::move(eviction_team)) {
    if (state_.options().evict_backend == eviction_backend::shared_top) {
      sn::util::log::ensure(
          state_.options().evict_batch <= state_.shape().subtree_leaf_count,
          "zingoram::client: shared-top E exceeds subtree leaf count"
      );
    }
    if constexpr (disjoint_epoch_mode) {
      disjoint_state_.emplace();
      disjoint_state_->configure(state_);
    }

    // log geometry on construction
    log_geometry();

    // validate eviction worker configuration
    const std::size_t subtree_count = static_cast<std::size_t>(state_.shape().subtree_count);
    sn::util::log::ensure(subtree_count > 0, "zingoram::client: subtree_count must be positive");
    const std::size_t eviction_worker_count = eviction_workers_.logical_threads();
    if (eviction_worker_count > 1) {
      sn::util::log::ensure(
          subtree_count % eviction_worker_count == 0,
          "zingoram::client: subtree_count must be divisible by eviction worker count"
      );
    }
  }

  ~client() {
    if constexpr (disjoint_epoch_mode) {
      if (disjoint_state_ && disjoint_state_->pending() > 0) {
        state_.log().dbgf("zingoram::client: destroyed with %d pending disjoint accesses", disjoint_state_->pending());
      }
    }
  }

  void initialize() {
    sn::util::log::ensure(!setup_started_, "zingoram::client: setup already started");
    setup_started_ = true;
    state_.initialize();
    state_.ensure_eviction_scratch_capacity(eviction_workers_.logical_threads());
    if constexpr (disjoint_epoch_mode) {
      disjoint_state_->reset(state_);
    }
    initialized_ = true;
    initial_state_.store(true, std::memory_order_relaxed);
  }

  void bulk_setup(std::span<const bulk_record<Traits::block_bytes>> records) {
    sn::util::log::ensure(!setup_started_, "zingoram::client: setup already started");
    setup_started_ = true;
    detail::bulk_setup<Traits>(state_, records);
    state_.ensure_eviction_scratch_capacity(eviction_workers_.logical_threads());
    if constexpr (disjoint_epoch_mode) {
      disjoint_state_->reset(state_);
    }
    initialized_ = true;
    initial_state_.store(true, std::memory_order_relaxed);
  }
  template <class Writer> void save_initial(Writer& writer) const {
    sn::util::log::ensure(initialized_ && initial_state_.load(std::memory_order_relaxed) &&
                          (!disjoint_epoch_mode || pending_epoch_accesses() == 0),
                          "zingoram::client: save requires idle initialized tree");
    writer.write(&state_.options().block_count, sizeof(state_.options().block_count));
    state_.save_initial(writer);
  }
  template <class Reader> void load_initial(Reader& reader) {
    sn::util::log::ensure(!setup_started_, "zingoram::client: setup already started");
    setup_started_ = true;
    std::uint64_t count = 0;
    reader.read(&count, sizeof(count));
    sn::util::log::ensure(count == state_.options().block_count, "zingoram::client: snapshot capacity mismatch");
    state_.load_initial(reader);
    state_.ensure_eviction_scratch_capacity(eviction_workers_.logical_threads());
    if constexpr (disjoint_epoch_mode) disjoint_state_->reset(state_);
    initialized_ = true;
    initial_state_.store(true, std::memory_order_relaxed);
  }

  void configure_access_scratch(access_scratch& scratch) const { state_.configure_access_scratch(scratch); }

  template <typename Mutator = sn::oram::read_write_mutator>
  void access(const sn::oram::access_request& req, access_scratch& scratch, Mutator&& mutator = Mutator{}) {
    sn::util::log::ensure(initialized_, "zingoram::client: access before successful setup");
    initial_state_.store(false, std::memory_order_relaxed);
    state_.metrics_ref().record_access();
    sn_prof_zone("zingoram.access");
    if constexpr (disjoint_epoch_mode) {
      access_disjoint(req, scratch, std::forward<Mutator>(mutator));
    } else {
      access_default(req, scratch, std::forward<Mutator>(mutator));
    }
  }

  void flush_epoch() {
    sn::util::log::ensure(initialized_, "zingoram::client: flush before successful setup");
    if constexpr (!disjoint_epoch_mode) {
      sn::util::log::fail("zingoram::client: flush_epoch is only available in disjoint epoch mode");
    }

    // flush pending blocks by repeated eviction
    auto& manager = *disjoint_state_;
    sn_prof_zone("zingoram.disjoint.flush");
    state_.ensure_eviction_scratch_capacity(eviction_workers_.logical_threads());
    flush_disjoint_epoch<Traits>(manager, state_, eviction_schedule_, eviction_workers_);
  }

  std::size_t pending_epoch_accesses() const {
    if constexpr (!disjoint_epoch_mode) {
      return 0;
    } else {
      return disjoint_state_->pending();
    }
  }

  void drop_epoch() {
    if constexpr (disjoint_epoch_mode) {
      disjoint_state_->drop_epoch(state_);
    }
  }

  template <typename Fn> void mutate_pending_payloads(Fn&& fn) {
    static_assert(disjoint_epoch_mode, "pending mutation requires disjoint epochs");
    sn::util::log::ensure(initialized_, "pending mutation before setup");
    disjoint_state_->mutate_payloads(std::forward<Fn>(fn));
  }

  state_type& state_ref() noexcept { return state_; }
  const state_type& state_ref() const noexcept { return state_; }
  [[nodiscard]] auto stash_metrics_snapshot() const noexcept { return state_.stash_metrics_snapshot(); }
  void reset_stash_metrics() noexcept { state_.reset_stash_metrics(); }

  const options_t& options() const noexcept { return state_.options(); }
  const typename state_type::geometry& shape() const noexcept { return state_.shape(); }
  std::uint32_t bucket_size() const noexcept { return state_.bucket_total_size(); }

private:
  template <typename Mutator>
  void access_default(const sn::oram::access_request& req, access_scratch& scratch, Mutator&& mutator) {
    sn_prof_zone("zingoram.access.default");
    // get a ticket for this access window
    auto ticket = gate_.enter();
    block_t updated_block;
    {
      sn_prof_zone("zingoram.access.path");
      // execute the tree access using scratch state
      updated_block = access_path<Traits>(state_, req, scratch, std::forward<Mutator>(mutator));
    }

    // store updated block as pathread inside the global stash
    {
      sn_prof_zone("zingoram.access.insert_pathread");
      fz_stash::insert_pathread(state_.stash(), updated_block);
    }

    // release the ticket; if we were the last to enter we become the eviction leader
    const bool leader = ticket.release();
    if (!leader) {
      return;
    }

    // leader waits for the window to drain before orchestrating eviction
    {
      sn_prof_zone("zingoram.eviction.leader");
      ticket.wait_until_drained();
      ticket.begin_eviction();

      // run eviction with exclusive access to the tree
      {
        sn_prof_zone("zingoram.eviction.execute");
        state_.ensure_eviction_scratch_capacity(eviction_workers_.logical_threads());
        evict<Traits>(state_, eviction_schedule_, eviction_workers_);
      }

      // reopen the gate for the next access window
      ticket.finish_eviction();
    }
  }

  template <typename Mutator>
  void access_disjoint(const sn::oram::access_request& req, access_scratch& scratch, Mutator&& mutator) {
    sn_prof_zone("zingoram.access.disjoint");
    block_t updated_block;
    {
      sn_prof_zone("zingoram.access.path");
      updated_block = access_path<Traits>(state_, req, scratch, std::forward<Mutator>(mutator));
    }

    disjoint_state_->record(updated_block);
  }

  void log_geometry() {
    const auto& geom = state_.shape();
    const auto& derived = state_.derived();
    const std::uint64_t total_data_bytes = state_.options().block_count * block_t::byte_size;
    state_.log().inff(
        "zingoram::client: height=%llu block_count=%llu total_data=%s routing_depth=%u subtree_count=%u evict_batch=%u "
        "eviction_rate=%d (base=%d) bucket_real=%u bucket_dummy=%u stash_bound=%llu access_concurrency=%u",
        geom.height, state_.options().block_count, sn::util::humanize::bytes(total_data_bytes), geom.routing_depth,
        geom.subtree_count, state_.options().evict_batch, derived.adjusted_eviction_rate, derived.base_eviction_rate,
        state_.options().bucket_real_size, state_.options().bucket_dummy_size, derived.stash_bound,
        state_.options().access_concurrency
    );

    if constexpr (disjoint_epoch_mode) {
      state_.log().inff(
          "zingoram::client: disjoint mode window=%llu num_pathreads=%llu",
          static_cast<unsigned long long>(state_.options().disjoint_epoch_window),
          static_cast<unsigned long long>(derived.num_pathreads)
      );
    }
  }

  state_type state_;
  detail::access_gate gate_;
  schedule eviction_schedule_;
  sn::threads::thread_team eviction_workers_;

  std::optional<disjoint_window_state<Traits>> disjoint_state_;
  bool setup_started_ = false;
  bool initialized_ = false;
  std::atomic<bool> initial_state_{false};
};

} // namespace sn::oram::zingoram
