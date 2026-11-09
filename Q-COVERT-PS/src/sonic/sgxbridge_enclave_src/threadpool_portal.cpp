#include "sonic/sgxbridge/enclave/threadpool_portal.hpp"

#include <algorithm>
#include <stdexcept>

#include <sgx_error.h>

#include "sonic/sgxbridge/enclave/runtime.hpp"
#include "sonic/sgxbridge/edl/bridge_shim.hpp"
#include "sonic/threads/sync.hpp"
#include "sonic/util/log.hpp"

namespace sn::sgxbridge::enclave {

namespace {

std::atomic<tp::threadpool_id> g_next_pool_id{1};
constexpr std::uint32_t kHandshakePollMs = 1;

sn::util::log::logger& portal_logger() {
  static sn::util::log::logger logger = sn::util::log::create("sgx.tp.portal");
  return logger;
}

inline std::uint32_t queue_capacity_hint(const tp::request& request) {
  const auto workers = request.workers.value;
  auto queue = request.queue.value;
  if (queue == 0) {
    const auto worker_hint = workers == 0 ? 1u : workers;
    queue = std::max(worker_hint * 4u, 1u);
  }
  return queue;
}

inline tp::queue_policy normalize_policy(tp::queue_policy policy) {
  return policy == tp::queue_policy::reject_when_full ? tp::queue_policy::reject_when_full
                                                      : tp::queue_policy::block_when_full;
}

}

threadpool_portal::threadpool_portal(common::enclave_execution_context* ctx) : ctx_(ctx) {}

static common::enclave_execution_context* require_context(common::enclave_execution_context* ctx) {
  if (ctx == nullptr || ctx->registry == nullptr || ctx->host_cookie == nullptr) {
    throw std::runtime_error("threadpool portal");
  }
  return ctx;
}

static tp::status handshake_status(tp::handshake_data* handshake) {
  if (handshake == nullptr) {
    return tp::status::not_ready;
  }
  return static_cast<tp::status>(handshake->status_word.load(std::memory_order_acquire));
}

static void update_handshake(tp::handshake_data* handshake, tp::status status) {
  if (handshake == nullptr) {
    return;
  }
  tp::handshake_publish_status(*handshake, status);
}

tp::registry& threadpool_portal::registry() { return *require_context(ctx_)->registry; }

tp::threadpool_id threadpool_portal::next_pool_id() { return g_next_pool_id.fetch_add(1, std::memory_order_relaxed); }

tp::result threadpool_portal::create_on_host(
    tp::threadpool_id id, const tp::request& request, tp::handshake_data*& handshake
) {
  handshake = nullptr;
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  void* handshake_ptr = nullptr;
  std::uint32_t host_status = tp::to_u32(tp::status::internal_error);
  std::uint32_t host_detail = 0;
  const auto normalized_workers = request.workers.value;
  const auto normalized_policy = normalize_policy(request.policy);
  const sgx_status_t status = sonic_sgxbridge_threadpool_host_create(
      &ocall_status, ctx_->host_cookie, id, normalized_workers, queue_capacity_hint(request),
      static_cast<std::uint32_t>(normalized_policy), &handshake_ptr, &host_status, &host_detail
  );
  if (status != SGX_SUCCESS) {
    return {tp::status::host_error, static_cast<std::uint32_t>(status)};
  }
  if (ocall_status != SGX_SUCCESS) {
    return {tp::status::host_error, static_cast<std::uint32_t>(ocall_status)};
  }
  handshake = static_cast<tp::handshake_data*>(handshake_ptr);
  sn::util::log::ensure(handshake != nullptr, "threadpool handshake");
  auto result = tp::result{tp::from_u32(host_status), host_detail};
  return result;
}

tp::result threadpool_portal::destroy_on_host(tp::threadpool_id id) {
  sgx_status_t ocall_status = SGX_ERROR_UNEXPECTED;
  std::uint32_t host_status = tp::to_u32(tp::status::internal_error);
  std::uint32_t host_detail = 0;
  const sgx_status_t status =
      sonic_sgxbridge_threadpool_host_destroy(&ocall_status, ctx_->host_cookie, id, &host_status, &host_detail);
  if (status != SGX_SUCCESS) {
    return {tp::status::host_error, static_cast<std::uint32_t>(status)};
  }
  if (ocall_status != SGX_SUCCESS) {
    return {tp::status::host_error, static_cast<std::uint32_t>(ocall_status)};
  }
  return {tp::from_u32(host_status), host_detail};
}

tp::result threadpool_portal::open(const tp::request& request, tp::descriptor& desc) {
  require_context(ctx_);
  const auto workers = request.workers.value;
  const auto pool_id = next_pool_id();
  auto state = registry().insert(pool_id, workers, queue_capacity_hint(request));

  tp::handshake_data* handshake = nullptr;
  auto host_result = create_on_host(pool_id, request, handshake);
  if (!host_result.succeeded()) {
    portal_logger().err("portal create");
    registry().erase(pool_id);
    return host_result;
  }
  state->handshake = handshake;
  if (workers == 0) {
    sync::lock_guard<sync::mutex> lock(state->mutex);
    state->mark_startup(tp::status::ok);
  }

  tp::result startup_snapshot{tp::status::not_ready, 0};
  {
    sync::unique_lock<sync::mutex> lock(state->mutex);
    while (state->attached_count.load(std::memory_order_acquire) < workers) {
      if (state->startup.code != tp::status::not_ready && state->startup.code != tp::status::ok) {
        break;
      }
      auto handshake_code = handshake_status(handshake);
      if (handshake_code != tp::status::not_ready && handshake_code != tp::status::ok) {
        state->mark_startup(handshake_code);
        break;
      }
      lock.unlock();
      sn::sgxbridge::time::sleep_ms(kHandshakePollMs);
      lock.lock();
    }
    startup_snapshot = state->startup;
  }

  const auto attached = state->attached_count.load(std::memory_order_acquire);
  if (attached < workers) {
    auto failure = startup_snapshot.code == tp::status::not_ready ? tp::status::not_ready : startup_snapshot.code;
    update_handshake(handshake, failure);
    destroy_on_host(pool_id);
    registry().erase(pool_id);
    portal_logger().err("portal attach");
    return {failure, startup_snapshot.detail};
  }

  desc.id = pool_id;
  desc.pool = &state->pool;
  return tp::result::ok();
}

tp::result threadpool_portal::close(tp::threadpool_id id) {
  auto state = registry().find(id);
  if (!state) {
    return {tp::status::not_found, 0};
  }
  bool expected = false;
  if (state->stop_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    state->pool.request_stop();
  }

  auto host_result = destroy_on_host(id);
  if (!host_result.succeeded()) {
    return host_result;
  }

  while (state->attached_count.load(std::memory_order_acquire) != 0) {
    sn::threads::cpu_relax();
  }
  registry().erase(id);
  return tp::result::ok();
}

tp::result threadpool_portal::force_stop(tp::threadpool_id id) {
  auto state = registry().find(id);
  if (!state) {
    return {tp::status::not_found, 0};
  }
  bool expected = false;
  if (state->stop_requested.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    state->pool.request_stop();
  }
  auto host_result = destroy_on_host(id);
  if (!host_result.succeeded()) {
    return host_result;
  }
  while (state->attached_count.load(std::memory_order_acquire) != 0) {
    sn::threads::cpu_relax();
  }
  registry().erase(id);
  return tp::result::ok();
}

}
