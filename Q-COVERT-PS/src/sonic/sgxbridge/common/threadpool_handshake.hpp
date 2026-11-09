#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "sonic/sgxbridge/common/threadpool.hpp"

namespace sn::sgxbridge::tp {

struct handshake_data {
  std::uint32_t expected_workers{0};
  std::atomic<std::uint32_t> attached_workers{0};
  std::atomic<std::uint32_t> ready_workers{0};
  std::atomic<std::uint32_t> status_word{static_cast<std::uint32_t>(status::not_ready)};
};

inline void reset_handshake(handshake_data& data, std::uint32_t expected) noexcept {
  data.expected_workers = expected;
  data.attached_workers.store(0, std::memory_order_relaxed);
  data.ready_workers.store(0, std::memory_order_relaxed);
  data.status_word.store(static_cast<std::uint32_t>(status::not_ready), std::memory_order_relaxed);
}

inline void handshake_report_attach(handshake_data& data) noexcept {
  data.attached_workers.fetch_add(1, std::memory_order_acq_rel);
}

inline void handshake_report_ready(handshake_data& data) noexcept {
  data.ready_workers.fetch_add(1, std::memory_order_acq_rel);
}

inline void handshake_publish_status(handshake_data& data, status value) noexcept {
  data.status_word.store(static_cast<std::uint32_t>(value), std::memory_order_release);
}

inline status handshake_status(const handshake_data& data) noexcept {
  return static_cast<status>(data.status_word.load(std::memory_order_acquire));
}

inline std::uint32_t handshake_attached(const handshake_data& data) noexcept {
  return data.attached_workers.load(std::memory_order_acquire);
}

inline std::uint32_t handshake_ready(const handshake_data& data) noexcept {
  return data.ready_workers.load(std::memory_order_acquire);
}

}

static_assert(std::is_trivially_copyable_v<sn::sgxbridge::tp::handshake_data>, "handshake_data must remain POD");
