#pragma once

#include "sonic/util/cputimer.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#if !defined(SN_SGX_ENCLAVE)
#include <chrono>
#include <thread>
#endif

namespace sn::sgxbridge::time {

namespace detail {
#if defined(SN_SGX_ENCLAVE)
bool enclave_sleep_ms(std::uint32_t millis) noexcept;
std::uint64_t query_host_time_ns() noexcept;
#endif
}

inline constexpr std::uint32_t infinite_timeout = std::numeric_limits<std::uint32_t>::max();

class steady_clock {
public:
  using time_point = sn::util::cpu_timer::counter_type;
  using duration = sn::util::cpu_timer::counter_type;

  static time_point now() noexcept { return sn::util::cpu_timer::now(); }
};

inline double to_nanoseconds(steady_clock::duration duration) noexcept {
  return sn::util::cpu_timer::cycles_to_ns(duration);
}

inline std::uint32_t to_milliseconds(steady_clock::duration duration) noexcept {
  if (duration == std::numeric_limits<steady_clock::duration>::max()) {
    return infinite_timeout;
  }
  const double ns = to_nanoseconds(duration);
  if (!std::isfinite(ns) || ns <= 0.0) {
    return 0;
  }
  const double ms = ns / 1'000'000.0;
  if (!std::isfinite(ms) || ms >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
    return infinite_timeout;
  }
  return static_cast<std::uint32_t>(ms);
}

inline steady_clock::duration from_milliseconds(std::uint32_t millis) noexcept {
  if (millis == infinite_timeout) {
    return std::numeric_limits<steady_clock::duration>::max();
  }
  const double scale = sn::util::cpu_timer::nanoseconds_per_cycle();
  if (!std::isfinite(scale) || scale <= 0.0) {
    return 0;
  }
  const double cycles = (static_cast<double>(millis) * 1'000'000.0) / scale;
  if (!std::isfinite(cycles) || cycles <= 0.0) {
    return 0;
  }
  const double max_value = static_cast<double>(std::numeric_limits<steady_clock::duration>::max());
  if (cycles >= max_value) {
    return std::numeric_limits<steady_clock::duration>::max();
  }
  return static_cast<steady_clock::duration>(cycles);
}

inline steady_clock::duration since(steady_clock::time_point start) noexcept { return steady_clock::now() - start; }

struct deadline {
  steady_clock::time_point expiry{0};
  bool infinite{true};

  static deadline infinite_timeout_deadline() noexcept { return deadline{}; }

  static deadline from_timeout_ms(std::uint32_t timeout_ms) noexcept {
    if (timeout_ms == infinite_timeout) {
      return infinite_timeout_deadline();
    }
    const auto now = steady_clock::now();
    const auto delta = from_milliseconds(timeout_ms);
    if (delta == std::numeric_limits<steady_clock::duration>::max()) {
      return infinite_timeout_deadline();
    }
    const auto max_point = std::numeric_limits<steady_clock::time_point>::max();
    if (max_point - delta <= now) {
      return deadline{max_point, true};
    }
    return deadline{now + delta, false};
  }

  bool expired(steady_clock::time_point now = steady_clock::now()) const noexcept {
    if (infinite) {
      return false;
    }
    return now >= expiry;
  }

  std::uint32_t remaining_ms(steady_clock::time_point now = steady_clock::now()) const noexcept {
    if (infinite) {
      return infinite_timeout;
    }
    if (now >= expiry) {
      return 0;
    }
    return to_milliseconds(expiry - now);
  }
};

inline bool sleep_ms(std::uint32_t millis) noexcept {
  if (millis == 0) {
    return true;
  }
#if defined(SN_SGX_ENCLAVE)
  return detail::enclave_sleep_ms(millis);
#else
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
  return true;
#endif
}

inline bool sleep_for(steady_clock::duration duration) noexcept {
  const auto millis = to_milliseconds(duration);
  if (millis == 0) {
    return true;
  }
  if (millis == infinite_timeout) {
    return false;
  }
  return sleep_ms(millis);
}

inline bool sleep_until(const deadline& deadline_value) noexcept {
  if (deadline_value.infinite) {
    return false;
  }
  auto now = steady_clock::now();
  while (!deadline_value.expired(now)) {
    const auto remaining = deadline_value.remaining_ms(now);
    if (remaining == 0) {
      break;
    }
    const auto chunk = remaining > 50u ? 50u : remaining;
    if (!sleep_ms(chunk)) {
      return false;
    }
    now = steady_clock::now();
  }
  return true;
}

}
