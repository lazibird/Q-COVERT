#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <thread>

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/util/cputimer.hpp"

extern "C" sgx_status_t sonic_sgxbridge_log_sink(void* host_state_ptr, const char* message) {
  if (host_state_ptr == nullptr || message == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  auto len = std::strlen(message);
  if (len == 0) {
    return SGX_SUCCESS;
  }
  std::fwrite(message, 1, len, stdout);
  std::fflush(stdout);
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_calculate_cycle_scale(void*, double* ns_per_cycle) {
  if (ns_per_cycle == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }

  using steady_clock = std::chrono::steady_clock;
  constexpr auto kMinSample = std::chrono::microseconds(500);

  const auto t0 = steady_clock::now();
  const auto c0 = sn::util::cpu_timer::now();

  auto t1 = t0;
  auto c1 = c0;
  while ((t1 - t0) < kMinSample) {
    std::this_thread::sleep_for(std::chrono::microseconds(50));
    t1 = steady_clock::now();
    c1 = sn::util::cpu_timer::now();
  }

  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
  const std::uint64_t cycles = c1 - c0;
  if (ns <= 0 || cycles == 0) {
    *ns_per_cycle = 1.0;
    return SGX_SUCCESS;
  }

  const double scale = static_cast<double>(ns) / static_cast<double>(cycles);
  if (!std::isfinite(scale) || scale <= 0.0) {
    *ns_per_cycle = 1.0;
    return SGX_SUCCESS;
  }

  *ns_per_cycle = scale;
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_sleep_ms(void*, std::uint32_t millis) {
  std::this_thread::sleep_for(std::chrono::milliseconds(millis));
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_query_time_ns(void*, std::uint64_t* out_nanoseconds) {
  if (out_nanoseconds == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  *out_nanoseconds = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  return SGX_SUCCESS;
}
