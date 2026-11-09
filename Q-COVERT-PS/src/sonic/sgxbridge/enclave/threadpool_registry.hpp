#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "sonic/sgxbridge/common/sync.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/sgxbridge/common/threadpool_handshake.hpp"
#include "sonic/sgxbridge/common/time.hpp"
#include "sonic/threads/thread_pool.hpp"

namespace sn::sgxbridge::tp {

class pool_state {
public:
  pool_state(std::size_t worker_count, std::size_t queue_capacity);

  sn::threads::thread_pool pool;
  std::vector<bool> attached;
  std::atomic<std::uint32_t> attached_count{0};
  std::atomic<bool> stop_requested{false};
  sync::mutex mutex;
  tp::handshake_data* handshake{nullptr};
  tp::result startup{status::not_ready, 0};

  std::uint32_t worker_capacity{0};

  void mark_startup(status code, std::uint32_t detail = 0);
};

class registry {
public:
  registry() = default;
  ~registry() = default;

  std::shared_ptr<pool_state> insert(threadpool_id id, std::size_t worker_count, std::size_t queue_capacity);
  std::shared_ptr<pool_state> find(threadpool_id id);
  void erase(threadpool_id id);

private:
  sync::mutex mutex_;
  std::unordered_map<threadpool_id, std::shared_ptr<pool_state>> pools_;
};

}
