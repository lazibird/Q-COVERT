#pragma once

#include "sonic/obliv/ops/core_ops.hpp"
#include <cstdint>

namespace sn {
namespace obliv {

class choice {
private:

  uint8_t value_;

public:
  constexpr choice() noexcept : value_(0) {}

  constexpr explicit choice(bool b) noexcept : value_(static_cast<uint8_t>(b)) {}


  static constexpr choice true_value() noexcept { return choice{true}; }
  static constexpr choice false_value() noexcept { return choice{false}; }


  constexpr bool unwrap() const noexcept { return static_cast<bool>(value_); }


  constexpr choice operator!() const noexcept { return choice{static_cast<bool>(value_ ^ 1u)}; }


  constexpr choice operator&&(const choice& other) const noexcept {
    return choice{static_cast<bool>(value_ & other.value_)};
  }


  constexpr choice operator||(const choice& other) const noexcept {
    return choice{static_cast<bool>(value_ | other.value_)};
  }


  constexpr choice operator^(const choice& other) const noexcept {
    return choice{static_cast<bool>(value_ ^ other.value_)};
  }

  choice operator==(const choice& other) const noexcept {
      return choice {
        ct_eq<uint8_t>(value_, other.value_)
      };
}

choice operator!=(const choice& other) const noexcept {
  return !(*this == other);
}
};

}
}
