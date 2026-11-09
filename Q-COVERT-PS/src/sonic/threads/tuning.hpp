#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace sn::threads {

enum class thread_affinity {
  inherit = 0,
  dedicated = 1,
};

enum class thread_smt {
  avoid = 0,
  allow = 1,
};

struct thread_policy {
  thread_affinity affinity = thread_affinity::inherit;
  thread_smt smt = thread_smt::avoid;
};

inline constexpr const char* describe(thread_affinity value) noexcept {
  switch (value) {
  case thread_affinity::inherit:
    return "inherit";
  case thread_affinity::dedicated:
    return "dedicated";
  }
  return "inherit";
}

inline constexpr const char* describe(thread_smt value) noexcept {
  switch (value) {
  case thread_smt::avoid:
    return "avoid";
  case thread_smt::allow:
    return "allow";
  }
  return "avoid";
}

struct thread_binding {
  std::optional<std::size_t> cpu{};
};

namespace detail {

struct thread_context_impl;

}

class thread_group_reservation {
public:
  thread_group_reservation() = default;
  ~thread_group_reservation();

  thread_group_reservation(const thread_group_reservation&) = delete;
  thread_group_reservation& operator=(const thread_group_reservation&) = delete;
  thread_group_reservation(thread_group_reservation&& other) noexcept;
  thread_group_reservation& operator=(thread_group_reservation&& other) noexcept;

  [[nodiscard]] std::size_t worker_count() const noexcept { return bindings_.size(); }
  [[nodiscard]] const std::vector<thread_binding>& workers() const noexcept { return bindings_; }
  [[nodiscard]] const thread_binding& worker(std::size_t index) const noexcept { return bindings_[index]; }

private:
  friend class thread_context;

  thread_group_reservation(
      std::shared_ptr<detail::thread_context_impl> impl, std::vector<thread_binding> bindings,
      std::vector<std::size_t> slot_indices
  ) noexcept;

  void release() noexcept;

  std::shared_ptr<detail::thread_context_impl> impl_{};
  std::vector<thread_binding> bindings_{};
  std::vector<std::size_t> slot_indices_{};
};

class thread_context {
public:
  thread_context();
  explicit thread_context(thread_policy policy);

  [[nodiscard]] const thread_policy& policy() const noexcept;
  void bind_current_thread() const;
  [[nodiscard]] thread_group_reservation reserve_group(std::size_t worker_count, std::string_view label = {}) const;

private:
  std::shared_ptr<detail::thread_context_impl> impl_{};
};

void apply_current_thread_binding(const thread_binding& binding);

}

#include "sonic/threads/platform/tuning_impl.hpp"
