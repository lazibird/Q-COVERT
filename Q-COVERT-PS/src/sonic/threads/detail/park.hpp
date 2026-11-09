#pragma once

#include <atomic>
#include <climits>
#include <cstdint>

#include "sonic/threads/platform/futex.hpp"
#include "sonic/threads/platform/sgx_trts.hpp"
#include "sonic/threads/sync.hpp"

namespace sn::threads {

enum class park_mode { none, futex, sgx_cond };

constexpr park_mode kDefaultParkMode =
#if defined(SN_SGX_ENCLAVE)
    park_mode::sgx_cond;
#elif SONIC_THREADS_HAS_OS && defined(__linux__)
    park_mode::futex;
#else
    park_mode::none;
#endif

namespace detail {

inline constexpr int kParkWakeAll = INT_MAX;

inline constexpr bool park_supports_sleep(park_mode mode) noexcept {
  return mode == park_mode::futex || mode == park_mode::sgx_cond;
}

inline void park_wait(park_mode mode, std::atomic<std::uint32_t>& seq, std::uint32_t expected) noexcept {
  switch (mode) {
  case park_mode::futex:
#if SONIC_THREADS_HAS_OS && defined(__linux__)
    platform::futex_wait(seq, expected);
#else
    static_cast<void>(seq);
    static_cast<void>(expected);
    cpu_relax();
#endif
    break;
  case park_mode::sgx_cond:
#if defined(SN_SGX_ENCLAVE)
    platform::sgx_cond_wait(seq, expected);
#else
    static_cast<void>(seq);
    static_cast<void>(expected);
    cpu_relax();
#endif
    break;
  case park_mode::none:
  default:
    cpu_relax();
    break;
  }
}

inline void park_wake(park_mode mode, std::atomic<std::uint32_t>& seq, int count = kParkWakeAll) noexcept {
  switch (mode) {
  case park_mode::futex:
#if SONIC_THREADS_HAS_OS && defined(__linux__)
    platform::futex_wake(seq, count);
#else
    static_cast<void>(seq);
    static_cast<void>(count);
#endif
    break;
  case park_mode::sgx_cond:
#if defined(SN_SGX_ENCLAVE)
    platform::sgx_cond_wake(seq, count);
#else
    static_cast<void>(seq);
    static_cast<void>(count);
#endif
    break;
  case park_mode::none:
  default:
    break;
  }
}

}
}
