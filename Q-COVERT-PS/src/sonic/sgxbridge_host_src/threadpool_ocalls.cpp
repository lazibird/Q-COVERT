#include <cstdint>

#include <sgx_error.h>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/sgxbridge/common/threadpool_handshake.hpp"
#include "sonic/sgxbridge/common/threadpool_services.hpp"
#include "sonic/sgxbridge/host/threadpool_manager.hpp"

namespace {

sn::sgxbridge::host::threadpool_manager* resolve_manager(void* host_state_ptr) {
  if (host_state_ptr == nullptr) {
    return nullptr;
  }
  auto* ctx = static_cast<sn::sgxbridge::common::host_execution_context*>(host_state_ptr);
  if (ctx->threadpools == nullptr) {
    return nullptr;
  }
  return ctx->threadpools->manager;
}

}

extern "C" sgx_status_t sonic_sgxbridge_threadpool_host_create(
    void* host_state_ptr, std::uint64_t pool_id, std::uint32_t worker_count, std::uint32_t queue_capacity,
    std::uint32_t policy_value, void** out_handshake, std::uint32_t* out_status, std::uint32_t* out_detail
) {
  if (out_status == nullptr || out_detail == nullptr || out_handshake == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(sn::sgxbridge::tp::status::invalid_arguments);
  *out_detail = 0;
  *out_handshake = nullptr;
  auto* manager = resolve_manager(host_state_ptr);
  if (manager == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }

  sn::sgxbridge::tp::request req{};
  req.workers = sn::sgxbridge::tp::worker_count{worker_count};
  req.queue = sn::sgxbridge::tp::queue_capacity{queue_capacity};
  req.policy = policy_value == static_cast<std::uint32_t>(sn::sgxbridge::tp::queue_policy::reject_when_full)
                   ? sn::sgxbridge::tp::queue_policy::reject_when_full
                   : sn::sgxbridge::tp::queue_policy::block_when_full;

  sn::sgxbridge::tp::handshake_data* handshake_ptr = nullptr;
  const auto result = manager->create_pool(pool_id, req, handshake_ptr);
  *out_status = sn::sgxbridge::tp::to_u32(result.code);
  *out_detail = result.detail;
  if (result.succeeded() && handshake_ptr != nullptr) {
    *out_handshake = static_cast<void*>(handshake_ptr);
  }
  return SGX_SUCCESS;
}

extern "C" sgx_status_t sonic_sgxbridge_threadpool_host_destroy(
    void* host_state_ptr, std::uint64_t pool_id, std::uint32_t* out_status, std::uint32_t* out_detail
) {
  if (out_status == nullptr || out_detail == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  *out_status = static_cast<std::uint32_t>(sn::sgxbridge::tp::status::invalid_arguments);
  *out_detail = 0;
  auto* manager = resolve_manager(host_state_ptr);
  if (manager == nullptr) {
    return SGX_ERROR_INVALID_PARAMETER;
  }
  const auto result = manager->destroy_pool(pool_id);
  *out_status = sn::sgxbridge::tp::to_u32(result.code);
  *out_detail = result.detail;
  return SGX_SUCCESS;
}
