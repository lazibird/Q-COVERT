#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <pthread.h>

#include "sonic/threads/thread_pool.hpp"
#include "sonic/threads/tuning.hpp"
#include "sonic/util/profiling.hpp"

namespace sn::threads {

class pthread_thread_pool {
public:
  explicit pthread_thread_pool(std::size_t worker_count) :
      pool_(worker_count), workers_(worker_count), args_(worker_count), worker_bindings_(worker_count) {
    start_workers();
  }

  explicit pthread_thread_pool(thread_group_reservation reservation, std::string label = {}) :
      pool_(reservation.worker_count()),
      workers_(reservation.worker_count()),
      args_(reservation.worker_count()),
      worker_bindings_(reservation.workers()),
      reservation_(std::move(reservation)),
      label_(std::move(label)) {
    start_workers();
  }

  explicit pthread_thread_pool(const thread_context& threads, std::size_t worker_count, std::string_view label = {}) :
      pthread_thread_pool(threads.reserve_group(worker_count, label), std::string(label)) {}

  ~pthread_thread_pool() {
    request_stop();
    join_started_workers(workers_.size());
  }

  pthread_thread_pool(const pthread_thread_pool&) = delete;
  pthread_thread_pool& operator=(const pthread_thread_pool&) = delete;
  pthread_thread_pool(pthread_thread_pool&&) = delete;
  pthread_thread_pool& operator=(pthread_thread_pool&&) = delete;

  thread_pool& pool() noexcept { return pool_; }
  const thread_pool& pool() const noexcept { return pool_; }
  std::size_t worker_count() const noexcept { return workers_.size(); }

private:
  struct worker_record {
    pthread_t handle{};
    std::size_t index = 0;
    bool started = false;
  };

  struct worker_arg {
    pthread_thread_pool* self = nullptr;
    std::size_t index = 0;
  };

  static void* worker_entry(void* raw) noexcept {
    auto* arg = static_cast<worker_arg*>(raw);
    arg->self->run_worker(arg->index);
    return nullptr;
  }

  void start_workers() {
    for (std::size_t ix = 0; ix < workers_.size(); ++ix) {
      workers_[ix].index = ix;
      args_[ix] = worker_arg{this, ix};
      const int rc = ::pthread_create(&workers_[ix].handle, nullptr, &pthread_thread_pool::worker_entry, &args_[ix]);
      if (rc != 0) {
        request_stop();
        join_started_workers(ix);
        throw std::runtime_error("pthread_thread_pool: pthread_create failed");
      }
      workers_[ix].started = true;
    }
  }

  void run_worker(std::size_t index) noexcept {
    try {
      if (index < worker_bindings_.size()) {
        apply_current_thread_binding(worker_bindings_[index]);
      }
      auto slot = pool_.attach_worker(index);
      {
        char name[64];
        const char* label = label_.empty() ? "pool" : label_.c_str();
        std::snprintf(name, sizeof(name), "%s-%zu", label, index);
        sn::prof::set_thread_name(name, static_cast<std::int32_t>(index + 1));
      }
      pool_.worker_loop(slot);
      pool_.detach_worker(slot);
    } catch (...) {
      pool_.request_stop();
    }
  }

  void request_stop() noexcept { pool_.request_stop(); }

  void join_started_workers(std::size_t count) noexcept {
    for (std::size_t ix = 0; ix < count; ++ix) {
      if (!workers_[ix].started) {
        continue;
      }
      void* ignored = nullptr;
      ::pthread_join(workers_[ix].handle, &ignored);
      workers_[ix].started = false;
    }
  }

  thread_pool pool_;
  std::vector<worker_record> workers_;
  std::vector<worker_arg> args_;
  std::vector<thread_binding> worker_bindings_;
  thread_group_reservation reservation_{};
  std::string label_{};
};

}
