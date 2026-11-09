#pragma once

#include <cstdint>

#if defined(SONIC_ORAM_METRICS)
#include <atomic>
#endif

namespace sn::oram::zingoram {

struct metrics_snapshot {
  std::uint64_t access_ops = 0;
  std::uint64_t evict_ops = 0;

  [[nodiscard]] std::uint64_t total_path_ops() const noexcept { return access_ops + evict_ops; }
};

#if defined(SONIC_ORAM_METRICS)
struct metrics {
  std::atomic<std::uint64_t> access_ops{0};
  std::atomic<std::uint64_t> evict_ops{0};

  void reset() noexcept {
    access_ops.store(0, std::memory_order_relaxed);
    evict_ops.store(0, std::memory_order_relaxed);
  }

  void record_access(std::uint64_t count = 1) noexcept { access_ops.fetch_add(count, std::memory_order_relaxed); }

  void record_evict(std::uint64_t count = 1) noexcept { evict_ops.fetch_add(count, std::memory_order_relaxed); }

  [[nodiscard]] metrics_snapshot snapshot() const noexcept {
    metrics_snapshot s{};
    s.access_ops = access_ops.load(std::memory_order_relaxed);
    s.evict_ops = evict_ops.load(std::memory_order_relaxed);
    return s;
  }

  [[nodiscard]] std::uint64_t total_path_ops() const noexcept {
    return access_ops.load(std::memory_order_relaxed) + evict_ops.load(std::memory_order_relaxed);
  }
};
#else
struct metrics {
  void reset() noexcept {}
  void record_access(std::uint64_t = 1) noexcept {}
  void record_evict(std::uint64_t = 1) noexcept {}

  [[nodiscard]] metrics_snapshot snapshot() const noexcept { return metrics_snapshot{}; }
  [[nodiscard]] std::uint64_t total_path_ops() const noexcept { return 0; }
};
#endif

inline std::uint64_t total_path_ops(const metrics_snapshot& snapshot) noexcept { return snapshot.total_path_ops(); }

} // namespace sn::oram::zingoram
