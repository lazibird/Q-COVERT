#include "sonic/sgxbridge/host/threadpool_manager.hpp"

#include <atomic>

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/edl/bridge_shim.hpp"
#include "sonic/threads/tuning.hpp"

namespace sn::sgxbridge::host {

namespace {

sn::util::log::logger& default_logger() {
  static sn::util::log::logger logger = sn::util::log::create("sgx.threadpool.host");
  return logger;
}

}

struct threadpool_manager::worker_state {
  std::atomic<bool> finished{false};
  std::atomic<std::uint32_t> status{static_cast<std::uint32_t>(tp::status::host_error)};
  sync::thread thread;
};

struct threadpool_manager::pool_record {
  tp::request request;
  std::vector<std::unique_ptr<worker_state>> workers;
  std::unique_ptr<tp::handshake_data> handshake;
  sn::threads::thread_group_reservation bindings{};
};

threadpool_manager::threadpool_manager() = default;
threadpool_manager::~threadpool_manager() { shutdown(); }

void threadpool_manager::configure(
    sgx_enclave_id_t id, sn::threads::thread_context threads, sn::util::log::logger* logger
) {
  shutdown();
  config_.emplace(
      configuration{
          .enclave_id = id,
          .threads = std::move(threads),
          .logger = logger != nullptr ? logger : &default_logger(),
      }
  );
}

void threadpool_manager::shutdown() {
  sync::lock_guard<sync::mutex> lock(mutex_);
  for (auto& entry : pools_) {
    if (entry.second) {
      (void) join_and_collect_locked(entry.first, *entry.second);
    }
  }
  pools_.clear();
  config_.reset();
}

tp::result threadpool_manager::create_pool(
    tp::threadpool_id id, const tp::request& request, tp::handshake_data*& handshake_ptr
) {
  sync::lock_guard<sync::mutex> lock(mutex_);
  handshake_ptr = nullptr;
  auto& log = logger();
  if (!config_.has_value()) {
    return {tp::status::host_error, 1};
  }
  if (pools_.find(id) != pools_.end()) {
    return {tp::status::already_exists, 0};
  }

  auto record = std::make_unique<pool_record>();
  record->request = request;
  record->handshake = std::make_unique<tp::handshake_data>();
  tp::reset_handshake(*record->handshake, request.workers.value);
  if (request.workers.value == 0) {
    tp::handshake_publish_status(*record->handshake, tp::status::ok);
  }

  try {
    record->bindings = config_->threads.reserve_group(request.workers.value, "sgx-host");
  } catch (...) {
    return {tp::status::internal_error, 3};
  }

  const auto spawn_result = spawn_workers_locked(id, request, *record, *record->handshake);
  if (!spawn_result.succeeded()) {
    log.err("threadpool spawn");
    return spawn_result;
  }

  handshake_ptr = record->handshake.get();
  pools_.emplace(id, std::move(record));
  return tp::result::ok();
}

tp::result threadpool_manager::destroy_pool(tp::threadpool_id id) {
  sync::lock_guard<sync::mutex> lock(mutex_);
  auto it = pools_.find(id);
  if (it == pools_.end()) {
    return {tp::status::not_found, 0};
  }
  auto result = join_and_collect_locked(id, *it->second);
  pools_.erase(it);
  return result;
}

tp::result threadpool_manager::spawn_workers_locked(
    tp::threadpool_id id, const tp::request& request, pool_record& record, tp::handshake_data& handshake
) {
  const std::uint32_t worker_count = request.workers.value;
  record.workers.reserve(worker_count);

  try {
    for (std::uint32_t idx = 0; idx < worker_count; ++idx) {
      auto state = std::make_unique<worker_state>();
      state->status.store(static_cast<std::uint32_t>(tp::status::not_ready), std::memory_order_relaxed);
      const auto binding = record.bindings.worker(idx);
      state->thread = sync::thread([this, id, idx, state_ptr = state.get(), &handshake, binding]() {
        sn::threads::apply_current_thread_binding(binding);
        const auto status = call_worker_entry(id, idx);
        state_ptr->status.store(static_cast<std::uint32_t>(status), std::memory_order_release);
        state_ptr->finished.store(true, std::memory_order_release);
        if (status != tp::status::ok) {
          tp::handshake_publish_status(handshake, status);
        } else {
          tp::handshake_report_ready(handshake);
        }
      });
      record.workers.push_back(std::move(state));
      tp::handshake_report_attach(handshake);
    }
  } catch (...) {
    logger().err("threadpool worker");
    for (auto& worker : record.workers) {
      if (worker && worker->thread.joinable()) {
        worker->thread.join();
      }
    }
    record.workers.clear();
    return {tp::status::internal_error, 0};
  }

  return tp::result::ok();
}

tp::result threadpool_manager::join_and_collect_locked(tp::threadpool_id id, pool_record& record) {
  for (auto& worker : record.workers) {
    if (worker && worker->thread.joinable()) {
      worker->thread.join();
    }
  }

  tp::status aggregate = tp::status::ok;
  std::uint32_t detail = 0;
  for (std::uint32_t idx = 0; idx < record.workers.size(); ++idx) {
    const auto& worker = record.workers[idx];
    if (!worker) {
      continue;
    }
    if (!worker->finished.load(std::memory_order_acquire)) {
      aggregate = tp::status::host_error;
      continue;
    }
    const auto status = tp::from_u32(worker->status.load(std::memory_order_acquire));
    if (status != tp::status::ok && aggregate == tp::status::ok) {
      aggregate = status;
      detail = idx;
    }
  }
  return {aggregate, detail};
}

tp::status threadpool_manager::call_worker_entry(tp::threadpool_id id, std::uint32_t worker_index) const {
  if (!config_.has_value()) {
    return tp::status::host_error;
  }
  std::uint32_t worker_status = static_cast<std::uint32_t>(tp::status::ok);
  const sgx_status_t status =
      sonic_sgxbridge_threadpool_worker_entry(config_->enclave_id, &worker_status, id, worker_index);
  if (status != SGX_SUCCESS) {
    logger().err("threadpool ecall");
    return tp::status::host_error;
  }
  return tp::from_u32(worker_status);
}

sn::util::log::logger& threadpool_manager::logger() const noexcept {
  if (config_.has_value() && config_->logger != nullptr) {
    return *config_->logger;
  }
  return default_logger();
}

}
