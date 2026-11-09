#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "sonic/oram/core/uid_generator.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"

namespace sn::oram::tree {

#if defined(ORAM_DEBUG)
using debug_uid = std::uint64_t;
#endif

namespace detail {
template <std::size_t BlockBytes> struct block_alignment {
  static constexpr std::size_t base = alignof(std::uint64_t);
#if defined(__AVX512F__)
  static constexpr std::size_t max_supported = 64;
#elif defined(__AVX2__)
  static constexpr std::size_t max_supported = 32;
#elif defined(__SSE2__)
  static constexpr std::size_t max_supported = 16;
#else
  static constexpr std::size_t max_supported = base;
#endif

  static constexpr std::size_t desired = []() constexpr {
    if constexpr (BlockBytes >= 64) {
      return std::size_t{64};
    }
    if constexpr (BlockBytes >= 32) {
      return std::size_t{32};
    }
    if constexpr (BlockBytes >= 16) {
      return std::size_t{16};
    }
    return base;
  }();

  static constexpr std::size_t limited = desired > max_supported ? max_supported : desired;
  static constexpr std::size_t value = limited < base ? base : limited;
};
} // namespace detail

// fixed-size oram block with associated metadata
template <std::size_t BlockBytes> struct alignas(detail::block_alignment<BlockBytes>::value) block {
  static constexpr std::size_t byte_size = BlockBytes;
  static constexpr std::int64_t dummy_address = -1;

  // data buffer
  std::array<std::uint8_t, BlockBytes> data{};
  std::int64_t address = dummy_address; // logical address; -1 marks dummy blocks
  std::int64_t leaf_ix = -1;            // assigned leaf index
  std::uint64_t extra = 0;              // misc scratch field
#if defined(ORAM_DEBUG)
  std::uint64_t uid = 0; // unique identifier (debug only)
#endif

  void set_dummy([[maybe_unused]] uid_generator& gen) noexcept {
    address = -1;
    leaf_ix = -1;
#if defined(ORAM_DEBUG)
    uid = gen.next();
#endif
    extra = 0;
  }

  static block make_dummy(uid_generator& gen) noexcept {
    block blk;
    blk.set_dummy(gen);
    return blk;
  }

  void set_dummy_cond(sn::obliv::choice cond, [[maybe_unused]] uid_generator& gen) noexcept {
    const bool cond_value = cond.unwrap();

    sn::obliv::ct_set(&address, dummy_address, cond_value);
    sn::obliv::ct_set(&leaf_ix, dummy_address, cond_value);
    sn::obliv::ct_set(&extra, static_cast<std::uint64_t>(0), cond_value);
#if defined(ORAM_DEBUG)
    const auto new_uid = gen.next();
    sn::obliv::ct_set(&uid, new_uid, cond_value);
#endif
  }

  [[nodiscard]] sn::obliv::choice is_real() const noexcept { return sn::obliv::choice(address >= 0); }
  [[nodiscard]] sn::obliv::choice is_dummy() const noexcept { return !is_real(); }
};

} // namespace sn::oram::tree
