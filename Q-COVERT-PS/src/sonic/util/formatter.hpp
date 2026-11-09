#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "sonic/util/picoformat.hpp"
#include "sonic/util/span.hpp"

namespace sn::util::format {

namespace detail {

constexpr std::array<std::string_view, 9> binary_suffixes{"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"};

constexpr std::array<std::string_view, 9> decimal_suffixes{"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};

template <typename Iterator> std::string format_range(Iterator first, Iterator last) {
  std::string out;
  out.push_back('[');
  bool first_elem = true;
  for (; first != last; ++first) {
    if (!first_elem) {
      out.append(", ");
    }
    out.append(pfm::format("%s", *first));
    first_elem = false;
  }
  out.push_back(']');
  return out;
}

}

enum class byte_unit_style { binary, decimal };

struct byte_format_spec {
  int precision = 2;
  byte_unit_style style = byte_unit_style::binary;
  bool include_space = true;
};

inline std::string format_bytes(std::uint64_t bytes, byte_format_spec spec = {}) {
  const auto& suffixes = spec.style == byte_unit_style::binary ? detail::binary_suffixes : detail::decimal_suffixes;
  const double base = spec.style == byte_unit_style::binary ? 1024.0 : 1000.0;

  if (bytes == 0) {
    return "0 B";
  }

  double value = static_cast<double>(bytes);
  std::size_t unit_index = 0;
  const std::size_t last_index = suffixes.size() - 1;
  while (value >= base && unit_index < last_index) {
    value /= base;
    ++unit_index;
  }

  if (unit_index == 0) {
    return pfm::format("%llu B", static_cast<unsigned long long>(bytes));
  }

  const char* fmt = spec.include_space ? "%.*f %s" : "%.*f%s";
  return pfm::format(fmt, spec.precision, value, suffixes[unit_index].data());
}

template <typename T> std::string format_vec(sn::util::span<T> span) {
  return detail::format_range(span.begin(), span.end());
}

template <typename T, std::size_t N> std::string format_vec(const std::array<T, N>& arr) {
  return detail::format_range(arr.begin(), arr.end());
}

template <typename T, typename Alloc> std::string format_vec(const std::vector<T, Alloc>& vec) {
  return detail::format_range(vec.begin(), vec.end());
}

template <typename T> std::string format_vec(std::initializer_list<T> values) {
  return detail::format_range(values.begin(), values.end());
}

template <typename Range> std::string format_vec(const Range& range) {
  using std::begin;
  using std::end;
  return detail::format_range(begin(range), end(range));
}

}
