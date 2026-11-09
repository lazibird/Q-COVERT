#include "sonic/sgxbridge/enclave/threadpool_provider.hpp"

#include "sonic/util/log.hpp"

namespace sn::sgxbridge::enclave {

namespace {

tp::result portal_acquire(void* context, const tp::request& request, tp::descriptor& desc) {
  auto* portal = static_cast<threadpool_portal*>(context);
  return portal->open(request, desc);
}

void portal_release(void* context, tp::threadpool_id id) {
  auto* portal = static_cast<threadpool_portal*>(context);
  const auto result = portal->close(id);
  if (!result.succeeded()) {
    sn::util::log::global_logger().err("threadpool close");
  }
}

}

tp::provider make_threadpool_provider(threadpool_portal& portal) {
  tp::provider provider{};
  provider.context = &portal;
  provider.acquire = &portal_acquire;
  provider.release = &portal_release;
  return provider;
}

}
