#pragma once

#include <memory>
#include <string_view>
#include <vector>

namespace sn::threads::detail {

struct thread_context_impl {
  thread_policy policy{};
};

inline std::shared_ptr<thread_context_impl> make_thread_context_impl(thread_policy policy) {
  auto impl = std::make_shared<thread_context_impl>();
  impl->policy = policy;
  return impl;
}

inline void bind_current_thread_impl(thread_context_impl&) {}

inline reservation_result reserve_group_impl(thread_context_impl&, std::size_t worker_count, std::string_view) {
  reservation_result result{};
  result.bindings.resize(worker_count);
  return result;
}

inline void release_slots_impl(thread_context_impl&, const std::vector<std::size_t>&) noexcept {}

inline void apply_current_thread_binding_impl(const thread_binding&) {}

}
