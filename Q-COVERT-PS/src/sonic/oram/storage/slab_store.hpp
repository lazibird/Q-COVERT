#pragma once

#include <cstdint>

#include "sonic/util/log.hpp"
#include "sonic/util/span.hpp"

#include "sonic/oram/storage/pin_mode.hpp"

namespace sn::oram::zingoram::storage {

// trivial in-memory store backed by slab; should melt away in optimization
template <typename Block> class slab_store {
public:
  using block_t = Block;

  class guard_t {
  public:
    guard_t() = default;
    guard_t(block_t* ptr, std::uint32_t len) noexcept : data_(ptr), len_(len) {}

    guard_t(const guard_t&) = delete;
    guard_t& operator=(const guard_t&) = delete;
    guard_t(guard_t&&) noexcept = default;
    guard_t& operator=(guard_t&&) noexcept = default;
    ~guard_t() noexcept = default;

    [[nodiscard]] sn::util::span<block_t> span() const noexcept {
      return sn::util::span<block_t>(data_, static_cast<std::size_t>(len_));
    }

    [[nodiscard]] block_t* data() const noexcept { return data_; }
    [[nodiscard]] std::uint32_t size() const noexcept { return len_; }

    void mark_dirty() const noexcept {}

  private:
    block_t* data_ = nullptr;
    std::uint32_t len_ = 0;
  };

  static constexpr bool is_trivial = true;
  static constexpr bool contiguous = true;

  void configure(block_t* base, std::uint32_t len) noexcept {
    base_ = base;
    len_ = len;
  }

  [[nodiscard]] guard_t pin(pin_mode) const noexcept { return guard_t{base_, len_}; }

  [[nodiscard]] block_t read_block(std::uint32_t offset) const noexcept {
#if defined(ORAM_DEBUG)
    sn::util::log::ensure(offset < len_, "slab_store: read_block offset out of range");
#endif
    return base_[offset];
  }

  void prefetch() const noexcept {}

private:
  block_t* base_ = nullptr;
  std::uint32_t len_ = 0;
};

} // namespace sn::oram::zingoram::storage
