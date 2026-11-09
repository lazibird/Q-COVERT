#pragma once

#include "sonic/util/oe/math_compat.hpp"
#include <cmath>
#include <cstdint>

#if !defined(SONIC_NO_OS) && !defined(__SGXSDK_ENCLAVE) && !defined(__OPENENCLAVE_ENCLAVE)
#include <chrono>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#include <immintrin.h>
#else
#include <x86intrin.h>
#include <immintrin.h>
#endif
#endif

namespace sn::util {

namespace math {
template <typename T> inline bool is_finite(T value) noexcept {
#if defined(__OPENENCLAVE__) || defined(__OPENENCLAVE_ENCLAVE)
  return !(isnan(static_cast<double>(value)) || isinf(static_cast<double>(value)));
#else
  return std::isfinite(value);
#endif
}
}

namespace detail {

inline double sanitize_cycle_scale(double measured) noexcept {
  if (!math::is_finite(measured) || measured <= 0.0) {
    return 1.0;
  }
  return measured;
}

#if defined(__aarch64__) || defined(_M_ARM64)

inline std::uint64_t read_cycle_counter() noexcept {
  asm volatile("isb" ::: "memory");
#if defined(__has_builtin)
#if __has_builtin(__builtin_readcyclecounter)
  {
    const auto builtin_value = __builtin_readcyclecounter();
    asm volatile("isb" ::: "memory");
    return static_cast<std::uint64_t>(builtin_value);
  }
#endif
#endif
  std::uint64_t value = 0;
  asm volatile("mrs %0, cntvct_el0" : "=r"(value));
  asm volatile("isb" ::: "memory");
  return value;
}

inline double nanoseconds_per_cycle_impl() noexcept {
  static const double scale = []() noexcept {
    std::uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    const double measured = (freq == 0) ? 0.0 : 1.0e9 / static_cast<double>(freq);
    return sanitize_cycle_scale(measured);
  }();
  return scale;
}

#elif defined(__x86_64__) || defined(_M_X64)

inline std::uint64_t read_cycle_counter() noexcept {
#if defined(_MSC_VER)
  _mm_lfence();
  const auto value = __rdtsc();
  _mm_lfence();
  return value;
#else
  unsigned long long value = 0;
#if defined(__has_builtin)
#if __has_builtin(__builtin_readcyclecounter)
  __asm__ __volatile__("lfence" ::: "memory");
  value = __builtin_readcyclecounter();
  __asm__ __volatile__("lfence" ::: "memory");
  return static_cast<std::uint64_t>(value);
#endif
#endif
  std::uint32_t lo = 0;
  std::uint32_t hi = 0;
  __asm__ __volatile__("lfence\nrdtsc\nlfence" : "=a"(lo), "=d"(hi)::"memory");
  return (static_cast<std::uint64_t>(hi) << 32) | static_cast<std::uint64_t>(lo);
#endif
}

#if defined(__SGXSDK_ENCLAVE)
double query_sgx_cycle_scale();
#elif defined(__OPENENCLAVE_ENCLAVE)
double query_openenclave_cycle_scale();
#endif

inline double nanoseconds_per_cycle_impl() noexcept {
#if defined(__SGXSDK_ENCLAVE)
  static const double scale = sanitize_cycle_scale(query_sgx_cycle_scale());
  return scale;
#elif defined(__OPENENCLAVE_ENCLAVE)
#if defined(SONIC_NO_OS)
  static const double scale = 1.0;
#else
  static const double scale = sanitize_cycle_scale(query_openenclave_cycle_scale());
#endif
  return scale;
#elif defined(SONIC_NO_OS)
  static const double scale = 1.0;
  return scale;
#else
  static const double scale = []() noexcept {
    using clock = std::chrono::steady_clock;
    auto t0 = clock::now();
    auto c0 = read_cycle_counter();
    auto t1 = t0;
    auto c1 = c0;
    constexpr auto min_span = std::chrono::microseconds(500);
    do {
      t1 = clock::now();
      c1 = read_cycle_counter();
    } while ((t1 - t0) < min_span);

    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    const std::uint64_t cycles = c1 - c0;
    const double measured = (cycles == 0) ? 0.0 : static_cast<double>(ns) / static_cast<double>(cycles);
    return sanitize_cycle_scale(measured);
  }();
  return scale;
#endif
}

#else

inline std::uint64_t read_cycle_counter() noexcept {
#if defined(SONIC_NO_OS)
  return 0;
#else
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
#endif
}

inline double nanoseconds_per_cycle_impl() noexcept { return 1.0; }

#endif

}

class cpu_timer {
public:
  using counter_type = std::uint64_t;

  cpu_timer() noexcept : start_(now()) {}

  static counter_type now() noexcept { return detail::read_cycle_counter(); }

  static double cycles_to_ns(counter_type cycles) noexcept {
    return static_cast<double>(cycles) * detail::nanoseconds_per_cycle_impl();
  }

  static double nanoseconds_per_cycle() noexcept { return detail::nanoseconds_per_cycle_impl(); }

  void reset() noexcept { start_ = now(); }

  counter_type raw_elapsed() const noexcept { return now() - start_; }

  double elapsed_ns() const noexcept { return cycles_to_ns(raw_elapsed()); }

private:
  counter_type start_;
};

}
