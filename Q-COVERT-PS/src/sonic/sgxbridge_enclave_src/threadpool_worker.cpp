#include "sonic/sgxbridge/enclave/runtime.hpp"

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"
#include "sonic/util/log.hpp"

namespace sn::sgxbridge::enclave {

namespace {

sn::util::log::logger& worker_logger() {
  static sn::util::log::logger logger = sn::util::log::create("sgx.tp.worker");
  return logger;
}

}

static tp::status worker_entry(tp::threadpool_id pool_id, std::uint32_t worker_index) {
  auto* ctx = current_execution_context();
  if (ctx == nullptr || ctx->registry == nullptr) {
    worker_logger().err("worker context");
    return tp::status::invalid_arguments;
  }
  auto state = ctx->registry->find(pool_id);
  if (!state) {
    worker_logger().err("worker pool");
    return tp::status::not_found;
  }
  if (worker_index >= state->attached.size()) {
    {
      sync::lock_guard<sync::mutex> guard(state->mutex);
      state->mark_startup(tp::status::invalid_arguments, worker_index);
    }
    worker_logger().err("worker index");
    return tp::status::invalid_arguments;
  }

  sn::threads::worker_slot slot;
  {
    sync::unique_lock<sync::mutex> lock(state->mutex);
    if (state->attached[worker_index]) {
      worker_logger().err("worker attached");
      return tp::status::already_exists;
    }
    try {
      slot = state->pool.attach_worker(worker_index);
      state->attached[worker_index] = true;
    } catch (...) {
      state->mark_startup(tp::status::internal_error, worker_index);
      worker_logger().err("worker attach");
      return tp::status::internal_error;
    }
    const auto attached_now = state->attached_count.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (attached_now == state->worker_capacity) {
      state->mark_startup(tp::status::ok);
    }
  }

  bool loop_failed = false;
  try {
    state->pool.worker_loop(slot);
  } catch (...) {
    loop_failed = true;
    worker_logger().err("worker loop");
  }

  state->pool.detach_worker(slot);
  state->attached_count.fetch_sub(1, std::memory_order_acq_rel);
  {
    sync::unique_lock<sync::mutex> lock(state->mutex);
    state->attached[worker_index] = false;
  }

  (void) loop_failed;
  return tp::status::ok;
}

}

extern "C" std::uint32_t sonic_sgxbridge_threadpool_worker_entry(std::uint64_t pool_id, std::uint32_t worker_index) {
  const auto status = sn::sgxbridge::enclave::worker_entry(pool_id, worker_index);
  return sn::sgxbridge::tp::to_u32(status);
}
