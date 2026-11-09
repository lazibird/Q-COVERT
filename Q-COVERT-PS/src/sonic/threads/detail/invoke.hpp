#pragma once

#include <cstddef>
#include <type_traits>

namespace sn::threads::detail {

template <typename Fn, typename Index>
inline void invoke_range_callable(Fn& fn, Index idx, std::size_t worker_index) noexcept {
  if constexpr (std::is_invocable_v<Fn&, Index, std::size_t>) {
    static_assert(
        std::is_nothrow_invocable_v<Fn&, Index, std::size_t>,
        "parallel callable"
    );
    fn(idx, worker_index);
  } else {
    static_assert(std::is_invocable_v<Fn&, Index>, "parallel callable");
    static_assert(std::is_nothrow_invocable_v<Fn&, Index>, "parallel callable");
    fn(idx);
    static_cast<void>(worker_index);
  }
}

template <typename Fn>
inline void invoke_worker_callable(Fn& fn, std::size_t logical_index, std::size_t worker_index) noexcept {
  if constexpr (std::is_invocable_v<Fn&, std::size_t, std::size_t>) {
    static_assert(
        std::is_nothrow_invocable_v<Fn&, std::size_t, std::size_t>,
        "thread callable"
    );
    fn(logical_index, worker_index);
  } else {
    static_assert(std::is_invocable_v<Fn&, std::size_t>, "thread callable");
    static_assert(
        std::is_nothrow_invocable_v<Fn&, std::size_t>,
        "thread callable"
    );
    fn(logical_index);
    static_cast<void>(worker_index);
  }
}

}
