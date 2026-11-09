#pragma once

#include <cstdint>
#include <stdexcept>
#include <utility>

#include "sonic/threads/thread_pool.hpp"

namespace sn::sgxbridge::tp {

using threadpool_id = std::uint64_t;

struct worker_count {
  std::uint32_t value{0};
  constexpr worker_count() = default;
  constexpr explicit worker_count(std::uint32_t v) : value(v) {}
};

struct queue_capacity {
  std::uint32_t value{0};
  constexpr queue_capacity() = default;
  constexpr explicit queue_capacity(std::uint32_t v) : value(v) {}
};

enum class queue_policy : std::uint32_t {
  block_when_full = 0,
  reject_when_full = 1,
};

struct request {
  worker_count workers{worker_count{1}};
  queue_capacity queue{queue_capacity{0}};
  queue_policy policy{queue_policy::block_when_full};
  const char* label{nullptr};
};

inline request make_request(
    std::uint32_t workers, std::uint32_t queue_hint, queue_policy policy = queue_policy::block_when_full,
    const char* label = nullptr
) noexcept {
  return request{worker_count{workers}, queue_capacity{queue_hint}, policy, label};
}

enum class status : std::uint32_t {
  ok = 0,
  invalid_arguments = 1,
  not_found = 2,
  already_exists = 3,
  queue_full = 4,
  not_ready = 5,
  internal_error = 6,
  host_error = 7,
};

struct result {
  status code{status::internal_error};
  std::uint32_t detail{0};

  static constexpr result ok() noexcept { return result{status::ok, 0}; }
  [[nodiscard]] constexpr bool succeeded() const noexcept { return code == status::ok; }
};

struct descriptor {
  threadpool_id id{0};
  sn::threads::thread_pool* pool{nullptr};
};

using acquire_fn = result (*)(void*, const request&, tp::descriptor&);
using release_fn = void (*)(void*, threadpool_id);

struct provider {
  void* context{nullptr};
  acquire_fn acquire{nullptr};
  release_fn release{nullptr};

  [[nodiscard]] bool available() const noexcept { return acquire != nullptr && release != nullptr; }
};

class session {
public:
  session() = default;
  explicit session(const request& req) : request_(req) {}

  session(const session&) = delete;
  session& operator=(const session&) = delete;

  session(session&& other) noexcept { move_from(std::move(other)); }
  session& operator=(session&& other) noexcept {
    if (this != &other) {
      close();
      move_from(std::move(other));
    }
    return *this;
  }

  ~session() { close(); }

  [[nodiscard]] result open(const provider& prov, const request& req) {
    close();
    provider_ = prov;
    request_ = req;
    if (!provider_.available()) {
      return last_result_ = result{status::invalid_arguments, 0};
    }
    tp::descriptor desc{};
    last_result_ = provider_.acquire(provider_.context, request_, desc);
    if (!last_result_.succeeded() || desc.pool == nullptr) {
      descriptor_ = {};
      owned_ = false;
      return last_result_;
    }
    descriptor_ = desc;
    owned_ = true;
    return last_result_;
  }

  void close() noexcept {
    if (!owned_) {
      descriptor_ = {};
      return;
    }
    if (provider_.release != nullptr && descriptor_.id != 0) {
      provider_.release(provider_.context, descriptor_.id);
    }
    descriptor_ = {};
    owned_ = false;
  }

  [[nodiscard]] const request& request_ref() const noexcept { return request_; }
  [[nodiscard]] tp::descriptor descriptor() const noexcept { return descriptor_; }
  [[nodiscard]] sn::threads::thread_pool* pool() const noexcept { return descriptor_.pool; }
  [[nodiscard]] sn::threads::thread_pool& pool_ref() const {
    if (descriptor_.pool == nullptr) {
      throw std::logic_error("tp::session does not own a thread pool");
    }
    return *descriptor_.pool;
  }
  [[nodiscard]] result last_result() const noexcept { return last_result_; }
  explicit operator bool() const noexcept { return owned_ && descriptor_.pool != nullptr && last_result_.succeeded(); }

private:
  void move_from(session&& other) noexcept {
    provider_ = other.provider_;
    request_ = other.request_;
    descriptor_ = other.descriptor_;
    last_result_ = other.last_result_;
    owned_ = other.owned_;
    other.owned_ = false;
    other.descriptor_ = {};
    other.last_result_ = result{};
  }

  provider provider_{};
  tp::request request_{};
  tp::descriptor descriptor_{};
  result last_result_{status::invalid_arguments, 0};
  bool owned_{false};
};

[[nodiscard]] constexpr const char* describe(status value) noexcept {
  switch (value) {
  case status::ok:
    return "ok";
  case status::invalid_arguments:
    return "invalid arguments";
  case status::not_found:
    return "not found";
  case status::already_exists:
    return "already exists";
  case status::queue_full:
    return "queue full";
  case status::not_ready:
    return "not ready";
  case status::internal_error:
    return "internal error";
  case status::host_error:
    return "host error";
  }
  return "unknown";
}

[[nodiscard]] constexpr std::uint32_t to_u32(status value) noexcept { return static_cast<std::uint32_t>(value); }

[[nodiscard]] constexpr status from_u32(std::uint32_t value) noexcept { return static_cast<status>(value); }

}
