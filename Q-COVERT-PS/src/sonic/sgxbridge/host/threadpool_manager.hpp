#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <sgx_eid.h>

#include "sonic/sgxbridge/common/sync.hpp"
#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/sgxbridge/common/threadpool_handshake.hpp"
#include "sonic/sgxbridge/common/threadpool_support.hpp"
#include "sonic/util/log.hpp"
#include "sonic/threads/tuning.hpp"

namespace sn::sgxbridge::host {

class threadpool_manager {
public:
  threadpool_manager();
  ~threadpool_manager();

  void configure(sgx_enclave_id_t id, sn::threads::thread_context threads, sn::util::log::logger* logger);
  void shutdown();

  tp::result create_pool(tp::threadpool_id id, const tp::request& request, tp::handshake_data*& handshake);
  tp::result destroy_pool(tp::threadpool_id id);

  [[nodiscard]] bool ready() const noexcept { return config_.has_value(); }

private:
  struct worker_state;
  struct pool_record;
  struct configuration {
    sgx_enclave_id_t enclave_id{0};
    sn::threads::thread_context threads{};
    sn::util::log::logger* logger{nullptr};
  };

  tp::result spawn_workers_locked(
      tp::threadpool_id id, const tp::request& request, pool_record& record, tp::handshake_data& handshake
  );
  tp::result join_and_collect_locked(tp::threadpool_id id, pool_record& record);
  tp::status call_worker_entry(tp::threadpool_id id, std::uint32_t worker_index) const;
  [[nodiscard]] sn::util::log::logger& logger() const noexcept;

  std::optional<configuration> config_{};
  sync::mutex mutex_;
  std::unordered_map<tp::threadpool_id, std::unique_ptr<pool_record>> pools_;
};

}
