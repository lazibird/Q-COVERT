#pragma once

#include <cstdint>

namespace sn::oram::zingoram::detail {

// ticket snapshot for a single epoch window
struct epoch_ticket {
  std::uint64_t ticket = 0;
  std::uint64_t base = 0;
  std::uint32_t span = 0;
  std::uint8_t phase = 0;

  [[nodiscard]] std::uint64_t limit() const noexcept { return base + static_cast<std::uint64_t>(span); }

  [[nodiscard]] std::uint32_t rank() const noexcept { return static_cast<std::uint32_t>(ticket - base); }

  [[nodiscard]] std::uint32_t span_value() const noexcept { return span; }

  [[nodiscard]] std::uint8_t phase_bit() const noexcept { return phase; }

  [[nodiscard]] bool is_owner() const noexcept { return (rank() + 1U) == span; }
};

} // namespace sn::oram::zingoram::detail
