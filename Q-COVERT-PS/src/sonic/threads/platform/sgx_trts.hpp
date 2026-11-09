#pragma once

#include <atomic>
#include <climits>
#include <cstdint>

#include "sonic/threads/sync.hpp"

#if defined(SN_SGX_ENCLAVE)
#include <sgx_thread.h>
#endif

namespace sn::threads::platform {

#if defined(SN_SGX_ENCLAVE)

class sgx_cond_park {
public:
  sgx_cond_park() noexcept {
    sgx_thread_mutex_init(&mutex_, nullptr);
    sgx_thread_cond_init(&cond_, nullptr);
  }

  ~sgx_cond_park() {
    sgx_thread_cond_destroy(&cond_);
    sgx_thread_mutex_destroy(&mutex_);
  }

  void wait(std::atomic<std::uint32_t>& seq, std::uint32_t expected) noexcept {
    if (seq.load(std::memory_order_relaxed) != expected) {
      return;
    }
    sgx_thread_mutex_lock(&mutex_);
    while (seq.load(std::memory_order_relaxed) == expected) {
      sgx_thread_cond_wait(&cond_, &mutex_);
    }
    sgx_thread_mutex_unlock(&mutex_);
  }

  void wake(std::atomic<std::uint32_t>& , int count) noexcept {
    sgx_thread_mutex_lock(&mutex_);
    if (count == 1) {
      sgx_thread_cond_signal(&cond_);
    } else {
      sgx_thread_cond_broadcast(&cond_);
    }
    sgx_thread_mutex_unlock(&mutex_);
  }

private:
  sgx_thread_mutex_t mutex_{};
  sgx_thread_cond_t cond_{};
};

inline sgx_cond_park& sgx_cond() noexcept {
  static sgx_cond_park instance;
  return instance;
}

inline void sgx_cond_wait(std::atomic<std::uint32_t>& seq, std::uint32_t expected) noexcept {
  sgx_cond().wait(seq, expected);
}

inline void sgx_cond_wake(std::atomic<std::uint32_t>& seq, int count = INT_MAX) noexcept {
  (void) count;
  sgx_cond().wake(seq, count);
}

#else

inline void sgx_cond_wait(std::atomic<std::uint32_t>&, std::uint32_t) noexcept { cpu_relax(); }
inline void sgx_cond_wake(std::atomic<std::uint32_t>&, int = INT_MAX) noexcept {}

#endif

}
