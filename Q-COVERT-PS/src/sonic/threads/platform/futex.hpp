#pragma once

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>

#include "sonic/threads/sync.hpp"

namespace sn::threads::platform {

#if SONIC_THREADS_HAS_OS && defined(__linux__)

#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

inline void futex_wait(std::atomic<std::uint32_t>& addr, std::uint32_t expected) noexcept {

  if (addr.load(std::memory_order_relaxed) != expected) {
    return;
  }

  for (;;) {
    const int rc = static_cast<int>(::syscall(
        SYS_futex, reinterpret_cast<int*>(&addr), FUTEX_WAIT_PRIVATE, static_cast<int>(expected), nullptr, nullptr, 0
    ));
    if (rc == 0) {
      return;
    }
    if (errno == EINTR) {
      continue;
    }

    return;
  }
}

inline void futex_wake(std::atomic<std::uint32_t>& addr, int count = INT_MAX) noexcept {
  (void) ::syscall(SYS_futex, reinterpret_cast<int*>(&addr), FUTEX_WAKE_PRIVATE, count, nullptr, nullptr, 0);
}

#else

inline void futex_wait(std::atomic<std::uint32_t>&, std::uint32_t) noexcept { cpu_relax(); }
inline void futex_wake(std::atomic<std::uint32_t>&, int = INT_MAX) noexcept {}

#endif

}
