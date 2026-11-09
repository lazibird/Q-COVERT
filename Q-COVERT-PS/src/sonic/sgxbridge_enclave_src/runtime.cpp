#include "sonic/sgxbridge/enclave/runtime.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>

#include <sgx_error.h>
#include <sgx_thread.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/sgxbridge/edl/bridge_shim.hpp"
#include "sonic/threads/thread_pool.hpp"
#include "sonic/util/log.hpp"

namespace {

std::atomic<sn::sgxbridge::common::enclave_execution_context*> g_execution_context{nullptr};

}

namespace sn::sgxbridge::enclave {

void set_execution_context(common::enclave_execution_context* ctx) {
  g_execution_context.store(ctx, std::memory_order_release);
}

common::enclave_execution_context* current_execution_context() {
  return g_execution_context.load(std::memory_order_acquire);
}

std::uint64_t current_thread_uid() noexcept {
  const auto self = static_cast<std::uint64_t>(sgx_thread_self());
  sn::util::log::ensure(self != 0, "sgx_thread_self returned null thread id");
  return self;
}

void reset_thread_uid() noexcept {}

execution_context_guard::execution_context_guard(common::enclave_execution_context& ctx) {
  previous_ = g_execution_context.exchange(&ctx, std::memory_order_acq_rel);

  sn::threads::thread_pool::reset_worker_index();
  reset_thread_uid();
}

execution_context_guard::~execution_context_guard() { g_execution_context.store(previous_, std::memory_order_release); }

}

extern "C" void snsgxcxx_stream_sink(const char* message) {
  if (message == nullptr || message[0] == '\0') {
    return;
  }
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  if (ctx == nullptr || ctx->host_cookie == nullptr) {
    return;
  }
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  (void) sonic_sgxbridge_log_sink(&ocall_status, ctx->host_cookie, message);
}

namespace sn::util::detail {

double query_sgx_cycle_scale() {
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  if (ctx == nullptr || ctx->host_cookie == nullptr) {
    return 1.0;
  }
  if (ctx->has_cycle_scale && ctx->ns_per_cycle > 0.0) {
    return ctx->ns_per_cycle;
  }
  double ns_per_cycle = 1.0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_calculate_cycle_scale(&ocall_status, ctx->host_cookie, &ns_per_cycle);
  if (status != SGX_SUCCESS || ocall_status != SGX_SUCCESS || !std::isfinite(ns_per_cycle) || ns_per_cycle <= 0.0) {
    return 1.0;
  }
  ctx->ns_per_cycle = ns_per_cycle;
  ctx->has_cycle_scale = true;
  return ns_per_cycle;
}

}

namespace sn::sgxbridge::time::detail {

bool enclave_sleep_ms(std::uint32_t millis) noexcept {
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  if (ctx == nullptr || ctx->host_cookie == nullptr) {
    return false;
  }
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_sleep_ms(&ocall_status, ctx->host_cookie, millis);
  return status == SGX_SUCCESS && ocall_status == SGX_SUCCESS;
}

std::uint64_t query_host_time_ns() noexcept {
  auto* ctx = sn::sgxbridge::enclave::current_execution_context();
  if (ctx == nullptr || ctx->host_cookie == nullptr) {
    return 0;
  }
  std::uint64_t value = 0;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  const sgx_status_t status = sonic_sgxbridge_query_time_ns(&ocall_status, ctx->host_cookie, &value);
  if (status != SGX_SUCCESS || ocall_status != SGX_SUCCESS) {
    return 0;
  }
  return value;
}

}
