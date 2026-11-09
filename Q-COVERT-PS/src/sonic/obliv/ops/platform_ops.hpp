#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <type_traits>

#include "sonic/util/memcpy.hpp"

namespace sn {
namespace obliv {

namespace detail {

constexpr bool kCustomMemcpyEnabled = sn::mem::custom_memcpy_enabled();

#if defined(__clang__) || defined(__GNUC__)
#define SONIC_OBLIV_FORCE_INLINE inline __attribute__((always_inline))
#else
#define SONIC_OBLIV_FORCE_INLINE inline
#endif

template <typename T> SONIC_OBLIV_FORCE_INLINE void fill_trivial(T* dst, std::size_t count, const T& value) {
  sn::mem::fill_trivial(dst, count, value);
}

SONIC_OBLIV_FORCE_INLINE void obliv_memcpy_bytes(void* dst, const void* src, std::size_t byte_count) {
  sn::mem::copy_bytes(dst, src, byte_count);
}

template <typename It> using iterator_category_t = typename std::iterator_traits<It>::iterator_category;

template <typename It>
constexpr bool is_random_access_iterator_v =
    std::is_base_of_v<std::random_access_iterator_tag, iterator_category_t<It>>;

template <typename It> inline auto iterator_to_address(It it) {

#if defined(__cpp_lib_to_address) && __cpp_lib_to_address >= 201711L && !defined(__LIBCPP_SGX) && !defined(_LIBCPP_SGX)
  return std::to_address(it);
#else
  if constexpr (std::is_pointer_v<It>) {
    return it;
  } else {
    return std::addressof(*it);
  }
#endif
}

template <typename T> using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

#undef SONIC_OBLIV_FORCE_INLINE

}

[[nodiscard]] constexpr bool custom_memcpy_enabled() { return detail::kCustomMemcpyEnabled; }

inline void* memcpy(void* dst, const void* src, std::size_t byte_count) {
  detail::obliv_memcpy_bytes(dst, src, byte_count);
  return dst;
}

inline void* memset(void* dst, int value, std::size_t byte_count) {
  const auto byte = static_cast<std::uint8_t>(value);
  detail::fill_trivial(static_cast<std::uint8_t*>(dst), byte_count, byte);
  return dst;
}

template <typename InputIt, typename OutputIt> OutputIt copy(InputIt first, InputIt last, OutputIt result) {
  if (first == last) {
    return result;
  }

  constexpr bool random_access =
      detail::is_random_access_iterator_v<InputIt> && detail::is_random_access_iterator_v<OutputIt>;

  if constexpr (random_access) {
    using input_value_t = typename std::iterator_traits<InputIt>::value_type;
    using output_value_t = typename std::iterator_traits<OutputIt>::value_type;
    constexpr bool trivially_copyable = std::is_trivially_copyable_v<detail::remove_cvref_t<input_value_t>> &&
                                        std::is_trivially_copyable_v<detail::remove_cvref_t<output_value_t>>;
    constexpr bool same_size = sizeof(input_value_t) == sizeof(output_value_t);

    if constexpr (trivially_copyable && same_size) {
      const auto count = static_cast<std::size_t>(last - first);
      if (count == 0) {
        return result;
      }
      auto* src_ptr = detail::iterator_to_address(first);
      auto* dst_ptr = detail::iterator_to_address(result);
      detail::obliv_memcpy_bytes(
          static_cast<void*>(dst_ptr), static_cast<const void*>(src_ptr), count * sizeof(input_value_t)
      );
      using diff_t = typename std::iterator_traits<OutputIt>::difference_type;
      return std::next(result, static_cast<diff_t>(count));
    } else {
      return std::copy(first, last, result);
    }
  } else {
    return std::copy(first, last, result);
  }
}

template <typename InputIt, typename Size, typename OutputIt>
OutputIt copy_n(InputIt first, Size count, OutputIt result) {
  if constexpr (std::is_integral_v<Size>) {
    if (count <= 0) {
      return result;
    }
  } else {
    if (count == Size{}) {
      return result;
    }
  }

  constexpr bool random_access =
      detail::is_random_access_iterator_v<InputIt> && detail::is_random_access_iterator_v<OutputIt>;

  if constexpr (random_access) {
    using input_value_t = typename std::iterator_traits<InputIt>::value_type;
    using output_value_t = typename std::iterator_traits<OutputIt>::value_type;
    constexpr bool trivially_copyable = std::is_trivially_copyable_v<detail::remove_cvref_t<input_value_t>> &&
                                        std::is_trivially_copyable_v<detail::remove_cvref_t<output_value_t>>;
    constexpr bool same_size = sizeof(input_value_t) == sizeof(output_value_t);

    if constexpr (trivially_copyable && same_size) {
      const auto n = static_cast<std::size_t>(count);
      if (n == 0) {
        return result;
      }
      auto* src_ptr = detail::iterator_to_address(first);
      auto* dst_ptr = detail::iterator_to_address(result);
      detail::obliv_memcpy_bytes(
          static_cast<void*>(dst_ptr), static_cast<const void*>(src_ptr), n * sizeof(input_value_t)
      );
      using diff_t = typename std::iterator_traits<OutputIt>::difference_type;
      return std::next(result, static_cast<diff_t>(n));
    } else {
      return std::copy_n(first, count, result);
    }
  } else {
    return std::copy_n(first, count, result);
  }
}

template <typename ForwardIt, typename T> void fill(ForwardIt first, ForwardIt last, const T& value) {
  if (first == last) {
    return;
  }

  using value_type = typename std::iterator_traits<ForwardIt>::value_type;

  constexpr bool random_access = detail::is_random_access_iterator_v<ForwardIt>;
  constexpr bool trivially_assignable = std::is_trivially_copy_assignable_v<value_type>;

  if constexpr (random_access && trivially_assignable) {
    const auto count = static_cast<std::size_t>(last - first);
    if (count == 0) {
      return;
    }
    auto* dst_ptr = detail::iterator_to_address(first);
    const value_type converted = static_cast<value_type>(value);

    if constexpr (sizeof(value_type) == 1 && std::is_integral_v<detail::remove_cvref_t<value_type>>) {
      sn::mem::fill_bytes(reinterpret_cast<std::uint8_t*>(dst_ptr), static_cast<std::uint8_t>(converted), count);
    } else {
      detail::fill_trivial(dst_ptr, count, converted);
    }
  } else {
    std::fill(first, last, value);
  }
}

}
}
