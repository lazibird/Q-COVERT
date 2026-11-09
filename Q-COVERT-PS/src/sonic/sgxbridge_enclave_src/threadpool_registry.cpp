#include "sonic/sgxbridge/enclave/threadpool_registry.hpp"

#include <stdexcept>

namespace sn::sgxbridge::tp {

pool_state::pool_state(std::size_t worker_count, std::size_t queue_capacity) : pool(worker_count) {
  (void) queue_capacity;
  attached.resize(worker_count, false);
  worker_capacity = static_cast<std::uint32_t>(worker_count);
}

void pool_state::mark_startup(status code, std::uint32_t detail) {
  tp::result desired{code, detail};
  auto current = startup;
  if (current.code == status::not_ready || current.code == status::ok) {
    startup = desired;
  }
}

std::shared_ptr<pool_state> registry::insert(threadpool_id id, std::size_t worker_count, std::size_t queue_capacity) {
  auto state = std::make_shared<pool_state>(worker_count, queue_capacity);
  {
    std::lock_guard lock(mutex_);
    pools_.emplace(id, state);
  }
  return state;
}

std::shared_ptr<pool_state> registry::find(threadpool_id id) {
  std::lock_guard lock(mutex_);
  auto it = pools_.find(id);
  if (it == pools_.end()) {
    return nullptr;
  }
  return it->second;
}

void registry::erase(threadpool_id id) {
  std::lock_guard lock(mutex_);
  pools_.erase(id);
}

}
