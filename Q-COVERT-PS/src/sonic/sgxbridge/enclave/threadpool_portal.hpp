#pragma once

#include <atomic>

#include "sonic/sgxbridge/common/execution_context.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"

namespace sn::sgxbridge::enclave {

class threadpool_portal {
public:
  explicit threadpool_portal(common::enclave_execution_context* ctx);

  tp::result open(const tp::request& request, tp::descriptor& desc);
  tp::result close(tp::threadpool_id id);
  tp::result force_stop(tp::threadpool_id id);

private:
  common::enclave_execution_context* ctx_{nullptr};

  tp::registry& registry();
  tp::threadpool_id next_pool_id();
  tp::result create_on_host(tp::threadpool_id id, const tp::request& request, tp::handshake_data*& handshake);
  tp::result destroy_on_host(tp::threadpool_id id);
};

}
