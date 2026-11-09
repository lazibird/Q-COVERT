#pragma once

#include <cstdint>

#if defined(ORAM_DEBUG)
#include <atomic>
#endif

namespace sn::oram {

class uid_generator {
public:
  using value_type = std::uint64_t;

#if defined(ORAM_DEBUG)
  uid_generator() = default;
  explicit uid_generator(value_type start) : next_uid_(start) {}

  value_type next() noexcept { return next_uid_.fetch_add(1, std::memory_order_relaxed); }
  void reset(value_type value = 1) noexcept { next_uid_.store(value, std::memory_order_relaxed); }
  [[nodiscard]] value_type current() const noexcept { return next_uid_.load(std::memory_order_relaxed); }
#else
  uid_generator() = default;
  explicit uid_generator([[maybe_unused]] value_type start) {}

  value_type next() noexcept { return 0; }
  void reset([[maybe_unused]] value_type value = 1) noexcept {}
  [[nodiscard]] value_type current() const noexcept { return 0; }
#endif

private:
#if defined(ORAM_DEBUG)
  std::atomic<value_type> next_uid_{1};
#endif
};

} // namespace sn::oram
