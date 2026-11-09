#pragma once

#include <cstdint>

#if defined(SONIC_ORAM_METRICS)
#include <atomic>
#endif

namespace sn::oram::stash::forestzing {

// maximum observed values for each metric
struct stash_metrics_maxima {
  std::uint64_t global_snapshot_real_max = 0;
  std::uint64_t subtree_routed_real_max = 0;
  std::uint64_t subtree_stash_treetop_real_max = 0;
  std::uint64_t subtree_stash_overlap_real_max = 0;
  // deferred: the actual probabilistic overflow stash
  std::uint64_t subtree_stash_deferred_real_max = 0;
  std::uint64_t subtree_stash_real_max = 0;
};

struct stash_metrics_snapshot {
  std::uint64_t global_snapshot_capacity = 0;
  std::uint64_t subtree_stash_total_capacity = 0;
  std::uint64_t routed_pathreads_capacity = 0;
  std::uint64_t subtree_stash_treetop_capacity = 0;
  std::uint64_t subtree_stash_overlap_capacity = 0;
  std::uint64_t subtree_stash_deferred_capacity = 0;
  // relocated capacity is the buckets we expand the subtree stash with
  std::uint64_t subtree_stash_relocated_capacity = 0;
  // the actual probabilistic overflow capacity
  std::uint64_t subtree_stash_overflow_capacity = 0;
  stash_metrics_maxima maxima{};
};

#if defined(SONIC_ORAM_METRICS)
class stash_metrics {
public:
  stash_metrics() = default;
  stash_metrics(const stash_metrics&) = delete;
  stash_metrics& operator=(const stash_metrics&) = delete;

  stash_metrics(stash_metrics&& other) noexcept { move_from(other); }

  stash_metrics& operator=(stash_metrics&& other) noexcept {
    if (this != &other) {
      move_from(other);
    }
    return *this;
  }

  void reset() noexcept {
    global_snapshot_real_max_.store(0, std::memory_order_relaxed);
    subtree_routed_real_max_.store(0, std::memory_order_relaxed);
    subtree_stash_treetop_real_max_.store(0, std::memory_order_relaxed);
    subtree_stash_overlap_real_max_.store(0, std::memory_order_relaxed);
    subtree_stash_deferred_real_max_.store(0, std::memory_order_relaxed);
    subtree_stash_real_max_.store(0, std::memory_order_relaxed);
  }

  void observe_global_snapshot_real(std::uint64_t value) noexcept { update_max(global_snapshot_real_max_, value); }

  void observe_subtree_routed_real(std::uint64_t value) noexcept { update_max(subtree_routed_real_max_, value); }

  void observe_subtree_stash_treetop_real(std::uint64_t value) noexcept {
    update_max(subtree_stash_treetop_real_max_, value);
  }

  void observe_subtree_stash_overlap_real(std::uint64_t value) noexcept {
    update_max(subtree_stash_overlap_real_max_, value);
  }

  void observe_subtree_stash_deferred_real(std::uint64_t value) noexcept {
    update_max(subtree_stash_deferred_real_max_, value);
  }

  void observe_subtree_stash_real(std::uint64_t value) noexcept { update_max(subtree_stash_real_max_, value); }

  [[nodiscard]] stash_metrics_maxima maxima() const noexcept {
    stash_metrics_maxima out{};
    out.global_snapshot_real_max = global_snapshot_real_max_.load(std::memory_order_relaxed);
    out.subtree_routed_real_max = subtree_routed_real_max_.load(std::memory_order_relaxed);
    out.subtree_stash_treetop_real_max = subtree_stash_treetop_real_max_.load(std::memory_order_relaxed);
    out.subtree_stash_overlap_real_max = subtree_stash_overlap_real_max_.load(std::memory_order_relaxed);
    out.subtree_stash_deferred_real_max = subtree_stash_deferred_real_max_.load(std::memory_order_relaxed);
    out.subtree_stash_real_max = subtree_stash_real_max_.load(std::memory_order_relaxed);
    return out;
  }

private:
  static void update_max(std::atomic<std::uint64_t>& maximum, std::uint64_t value) noexcept {
    std::uint64_t current = maximum.load(std::memory_order_relaxed);
    while (value > current && !maximum.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
  }

  void move_from(const stash_metrics& other) noexcept {
    global_snapshot_real_max_.store(other.global_snapshot_real_max_.load(std::memory_order_relaxed));
    subtree_routed_real_max_.store(other.subtree_routed_real_max_.load(std::memory_order_relaxed));
    subtree_stash_treetop_real_max_.store(other.subtree_stash_treetop_real_max_.load(std::memory_order_relaxed));
    subtree_stash_overlap_real_max_.store(other.subtree_stash_overlap_real_max_.load(std::memory_order_relaxed));
    subtree_stash_deferred_real_max_.store(other.subtree_stash_deferred_real_max_.load(std::memory_order_relaxed));
    subtree_stash_real_max_.store(other.subtree_stash_real_max_.load(std::memory_order_relaxed));
  }

  std::atomic<std::uint64_t> global_snapshot_real_max_{0};
  std::atomic<std::uint64_t> subtree_routed_real_max_{0};
  std::atomic<std::uint64_t> subtree_stash_treetop_real_max_{0};
  std::atomic<std::uint64_t> subtree_stash_overlap_real_max_{0};
  std::atomic<std::uint64_t> subtree_stash_deferred_real_max_{0};
  std::atomic<std::uint64_t> subtree_stash_real_max_{0};
};
#endif

} // namespace sn::oram::stash::forestzing
