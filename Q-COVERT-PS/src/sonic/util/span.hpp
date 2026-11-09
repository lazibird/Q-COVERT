#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace sn::util {

template <typename T> class span {
  static_assert(std::is_object_v<T>, "sn::util::span<T>: T must be an object type");
  static_assert(!std::is_abstract_v<T>, "sn::util::span<T>: T must not be abstract");

public:
  using element_type = T;
  using value_type = T;
  using pointer = T*;
  using const_pointer = const T*;
  using reference = T&;
  using const_reference = const T&;
  using iterator = T*;
  using const_iterator = const T*;
  using size_type = std::size_t;
  using difference_type = std::ptrdiff_t;

  constexpr span() noexcept : data_(nullptr), size_(0) {}

  constexpr span(pointer ptr, size_type count) noexcept : data_(ptr), size_(count) {}

  template <size_type N> constexpr span(T (&arr)[N]) noexcept : data_(arr), size_(N) {}

  template <typename U, size_type N, typename = std::enable_if_t<std::is_same_v<U, std::remove_const_t<T>>>>
  constexpr span(std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N) {}

  template <typename U, size_type N, typename = std::enable_if_t<std::is_same_v<const U, T>>>
  constexpr span(const std::array<U, N>& arr) noexcept : data_(arr.data()), size_(N) {}

  template <
      typename U, typename = std::enable_if_t<std::is_same_v<U, std::remove_const_t<T>> && !std::is_same_v<U, bool>>>
  span(std::vector<U>& vec) noexcept : data_(vec.data()), size_(vec.size()) {}

  template <typename U, typename = std::enable_if_t<std::is_same_v<const U, T> && !std::is_same_v<U, bool>>>
  span(const std::vector<U>& vec) noexcept : data_(vec.data()), size_(vec.size()) {}

  template <typename U, typename = std::enable_if_t<std::is_same_v<const U, T>>>
  constexpr span(const span<U>& other) noexcept : data_(other.data()), size_(other.size()) {}

  constexpr span(const span&) noexcept = default;
  constexpr span(span&&) noexcept = default;
  constexpr span& operator=(const span&) noexcept = default;
  constexpr span& operator=(span&&) noexcept = default;

  constexpr pointer data() const noexcept { return data_; }
  constexpr size_type size() const noexcept { return size_; }
  constexpr bool empty() const noexcept { return size_ == 0; }

  constexpr iterator begin() const noexcept { return data_; }
  constexpr iterator end() const noexcept { return data_ + size_; }

  constexpr reference operator[](size_type idx) const noexcept { return data_[idx]; }

  constexpr span<T> subspan(size_type offset, size_type count) const noexcept {
    assert(offset <= size_ && count <= (size_ - offset));
    return span<T>(data_ + offset, count);
  }

private:
  pointer data_;
  size_type size_;
};

}
