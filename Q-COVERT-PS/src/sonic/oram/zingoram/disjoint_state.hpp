#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "sonic/oram/zingoram/state.hpp"
#include "sonic/oram/zingoram/eviction.hpp"
#include "sonic/util/log.hpp"
#include "sonic/util/humanize.hpp"
#include "sonic/util/profiling.hpp"
#include "sonic/util/span.hpp"

#if defined(ORAM_DEBUG)
#include <mutex>
#include <unordered_set>
#endif

namespace sn::oram::zingoram {

namespace fz_stash = sn::oram::stash::forestzing;

template <typename Traits> class disjoint_window_state {
public:
  using state_type = state<Traits>;
  using block_t = typename Traits::block_t;

  void configure(state_type& st) {
    sn_prof_zone("zingoram.disjoint.configure");
    window_size_ = static_cast<std::size_t>(st.options().disjoint_epoch_window);
    chunk_size_ = static_cast<std::size_t>(st.derived().num_pathreads);
    sn::util::log::ensure(window_size_ > 0, "disjoint_window_state: window must be positive");
    sn::util::log::ensure(chunk_size_ > 0, "disjoint_window_state: chunk must be positive");
    sn::util::log::ensure(
        window_size_ % chunk_size_ == 0, "disjoint_window_state: window must be a multiple of chunk size"
    );
    slots_.resize(window_size_);
    const std::size_t bytes = window_size_ * sizeof(block_t);
    st.log().trcf(
        "disjoint_window_state: allocate slots window=%zu chunk=%zu size=%s", window_size_, chunk_size_,
        sn::util::humanize::bytes(bytes)
    );
#if defined(ORAM_DEBUG)
    // initialize a bitmap to validate uniqueness of addresses within an epoch
    debug_address_bitmap_.reserve(window_size_);
#endif
    reset(st);
  }

  void reset(state_type& st) {
    sn_prof_zone("zingoram.disjoint.reset");
    next_.store(0, std::memory_order_relaxed);
#if defined(ORAM_DEBUG)
    std::scoped_lock lock(debug_mutex_);
    debug_address_bitmap_.clear();
#endif
  }

  void record(const block_t& block) {
    sn_prof_zone("zingoram.disjoint.record");
    const std::size_t slot = next_.fetch_add(1, std::memory_order_relaxed);
    sn::util::log::ensure(slot < window_size_, "disjoint_window_state: window exceeded");
    slots_[slot] = block;
#if defined(ORAM_DEBUG)
    if (block.address >= 0) {
      std::scoped_lock lock(debug_mutex_);
      const bool inserted = debug_address_bitmap_.insert(block.address).second;
      sn::util::log::ensure(inserted, "disjoint_window_state: duplicate address encountered within epoch");
    }
#endif
  }

  [[nodiscard]] std::size_t pending() const noexcept { return next_.load(std::memory_order_relaxed); }

  // Caller has joined all readers. Visit every pending payload, never select a
  // slot by a secret address. The callback must preserve block address/leaf.
  template <typename Fn> void mutate_payloads(Fn&& fn) {
    const auto count = pending();
    for (std::size_t i = 0; i < count; ++i) fn(slots_[i].data);
  }

  void drop_epoch(state_type& st) noexcept { reset(st); }

  template <typename Fn> void flush(state_type& st, Fn&& consume_chunk) {
    sn_prof_zone("zingoram.disjoint.flush_internal");
    const std::size_t count = next_.load(std::memory_order_acquire);
    if (count == 0) {
      reset(st);
      return;
    }

    sn::util::log::ensure(chunk_size_ > 0, "disjoint_window_state: invalid chunk size");

    const std::size_t full_chunks = count / chunk_size_;
    const std::size_t remainder = count % chunk_size_;

    const block_t* data = slots_.data();
    for (std::size_t offset = 0; offset < full_chunks * chunk_size_; offset += chunk_size_) {
      sn::util::span<const block_t> chunk(data + offset, chunk_size_);
      consume_chunk(chunk);
    }

    if (remainder > 0) {
      const std::size_t pad_start = full_chunks * chunk_size_ + remainder;
      auto& uid_gen = st.uid_gen();
      for (std::size_t idx = pad_start; idx < (full_chunks + 1ULL) * chunk_size_; ++idx) {
        slots_[idx].set_dummy(uid_gen);
      }
      sn::util::span<const block_t> chunk(data + full_chunks * chunk_size_, chunk_size_);
      consume_chunk(chunk);
    }

    reset(st);
  }

private:
  std::vector<block_t> slots_{};
  std::size_t window_size_{0};
  std::size_t chunk_size_{0};
  std::atomic<std::size_t> next_{0};
#if defined(ORAM_DEBUG)
  std::mutex debug_mutex_;
  std::unordered_set<std::int64_t> debug_address_bitmap_;
#endif
};

template <typename Traits>
void flush_disjoint_epoch(
    disjoint_window_state<Traits>& manager, state<Traits>& st, schedule& sched, sn::threads::thread_team& workers
) {
  using block_t = typename Traits::block_t;

#if defined(ORAM_DEBUG)
  sn::util::log::ensure(
      manager.pending() <= st.options().disjoint_epoch_window, "flush_epoch: pending count exceeds window"
  );
  st.log().trcf(
      "zingoram::client: flushing disjoint epoch with %d/%d pending accesses", manager.pending(),
      st.options().disjoint_epoch_window
  );
#endif

  // if no pending accesses, just reset
  if (manager.pending() == 0) {
    manager.reset(st);
    return;
  }

  // flush pending blocks in chunks, evicting for each chunk
  manager.flush(st, [&](sn::util::span<const block_t> chunk) {
    // deliver chunk of pathreads to global stash
    {
      sn_prof_zone("zingoram.disjoint.chunk_to_stash");
      fz_stash::insert_pathread_batch(st.stash(), chunk);
    }
    // run eviction along scheduled paths
    {
      sn_prof_zone("zingoram.disjoint.chunk_evict");
      evict<Traits>(st, sched, workers);
    }
  });
}

} // namespace sn::oram::zingoram
