#pragma once

#include <atomic>
#include <cstdint>

#include "sonic/sgxbridge/common/threadpool.hpp"
#include "sonic/threads/helpers.hpp"
#include "sonic/util/log.hpp"

namespace sn::sgxbridge::tp {

using stop_token = sn::threads::stop_token;
using task_group = sn::threads::task_group;
using blocking_executor = sn::threads::blocking_executor;

inline bool acquire_session(session& target, const provider& prov, const request& req, sn::util::log::logger& logger) {
  if (!prov.available()) {
    logger.err("threadpool provider");
    return false;
  }
  const auto result = target.open(prov, req);
  if (!result.succeeded() || target.pool() == nullptr) {
    logger.err("threadpool acquire");
    return false;
  }
  return true;
}

class background_task {
public:
  background_task() = default;
  background_task(const background_task&) = delete;
  background_task& operator=(const background_task&) = delete;
  background_task(background_task&&) = delete;
  background_task& operator=(background_task&&) = delete;

  template <typename Fn>
  bool start(
      const provider& prov, const request& req, sn::util::log::logger& logger, std::atomic<bool>& stop_flag,
      std::size_t task_count, Fn&& fn
  ) {
    if (group_.running()) {
      logger.err("background task");
      return false;
    }
    if (!acquire_session(session_, prov, req, logger)) {
      return false;
    }
    if (session_.pool() == nullptr) {
      logger.err("background task");
      session_.close();
      return false;
    }
    if (!group_.start(session_.pool(), stop_flag, task_count, std::forward<Fn>(fn))) {
      logger.err("background task");
      session_.close();
      return false;
    }
    label_ = req.label;
    return true;
  }

  template <typename Fn>
  bool start(
      const provider& prov, const request& req, sn::util::log::logger& logger, std::atomic<bool>& stop_flag, Fn&& fn
  ) {
    return start(prov, req, logger, stop_flag, 1, std::forward<Fn>(fn));
  }

  [[nodiscard]] bool running() const noexcept { return group_.running(); }

  void request_stop() noexcept { group_.request_stop(); }

  void stop() {
    stop([]() noexcept {});
  }

  template <typename Fn> void stop(Fn&& before_join) { stop_impl(std::forward<Fn>(before_join)); }

private:
  template <typename Fn> void stop_impl(Fn&& before_join) {
    before_join();
    group_.stop();
    session_.close();
    label_ = nullptr;
  }

  session session_{};
  sn::threads::managed_task_group group_{};
  const char* label_{nullptr};
};

}
