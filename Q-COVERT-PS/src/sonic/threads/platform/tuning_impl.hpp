#pragma once

#include <utility>

namespace sn::threads::detail {

struct reservation_result {
  std::vector<thread_binding> bindings;
  std::vector<std::size_t> slot_indices;
};

std::shared_ptr<thread_context_impl> make_thread_context_impl(thread_policy policy);
void bind_current_thread_impl(thread_context_impl& impl);
reservation_result reserve_group_impl(thread_context_impl& impl, std::size_t worker_count, std::string_view label);
void release_slots_impl(thread_context_impl& impl, const std::vector<std::size_t>& slot_indices) noexcept;
void apply_current_thread_binding_impl(const thread_binding& binding);

}

#if defined(SONIC_THREADS_HAS_OS) && SONIC_THREADS_HAS_OS && defined(__linux__)
#include "sonic/threads/platform/tuning_linux.hpp"
#else
#include "sonic/threads/platform/tuning_stub.hpp"
#endif

namespace sn::threads {

inline thread_group_reservation::thread_group_reservation(
    std::shared_ptr<detail::thread_context_impl> impl, std::vector<thread_binding> bindings,
    std::vector<std::size_t> slot_indices
) noexcept :
    impl_(std::move(impl)), bindings_(std::move(bindings)), slot_indices_(std::move(slot_indices)) {}

inline thread_group_reservation::~thread_group_reservation() { release(); }

inline thread_group_reservation::thread_group_reservation(thread_group_reservation&& other) noexcept = default;

inline thread_group_reservation& thread_group_reservation::operator=(thread_group_reservation&& other) noexcept {
  if (this != &other) {
    release();
    impl_ = std::move(other.impl_);
    bindings_ = std::move(other.bindings_);
    slot_indices_ = std::move(other.slot_indices_);
  }
  return *this;
}

inline void thread_group_reservation::release() noexcept {
  if (impl_ && !slot_indices_.empty()) {
    detail::release_slots_impl(*impl_, slot_indices_);
  }
  impl_.reset();
  bindings_.clear();
  slot_indices_.clear();
}

inline thread_context::thread_context() : thread_context(thread_policy{}) {}

inline thread_context::thread_context(thread_policy policy) : impl_(detail::make_thread_context_impl(policy)) {}

inline const thread_policy& thread_context::policy() const noexcept { return impl_->policy; }

inline void thread_context::bind_current_thread() const { detail::bind_current_thread_impl(*impl_); }

inline thread_group_reservation thread_context::reserve_group(std::size_t worker_count, std::string_view label) const {
  auto allocation = detail::reserve_group_impl(*impl_, worker_count, label);
  return thread_group_reservation(impl_, std::move(allocation.bindings), std::move(allocation.slot_indices));
}

inline void apply_current_thread_binding(const thread_binding& binding) {
  detail::apply_current_thread_binding_impl(binding);
}

}
