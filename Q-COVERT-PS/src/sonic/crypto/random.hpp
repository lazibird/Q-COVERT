#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "sonic/crypto/error.hpp"
#include "sonic/crypto/detail/backend_utils.hpp"
#include "sonic/util/span.hpp"

namespace sn::crypto {

class random_device {
public:
  random_device() = default;

  void fill(sn::util::span<std::uint8_t> out) const;

  void fill(std::uint8_t* dst, std::size_t len) const;

  template <typename T> void fill_trivial(T& value) const {
    static_assert(std::is_trivially_copyable<T>::value, "fill_trivial expects trivially copyable type");
    fill(reinterpret_cast<std::uint8_t*>(&value), sizeof(T));
  }
};

}
