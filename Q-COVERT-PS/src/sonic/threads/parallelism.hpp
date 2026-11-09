#pragma once

#include <cstddef>

namespace sn::threads {

inline constexpr std::size_t background_threads_for_parallelism(std::size_t logical_parallelism) noexcept {
  return logical_parallelism <= 1 ? std::size_t{0} : logical_parallelism - 1;
}

inline constexpr std::size_t logical_parallelism_from_background(std::size_t background_threads) noexcept {
  return background_threads == 0 ? std::size_t{1} : background_threads + 1;
}

inline constexpr bool requires_background_threads(std::size_t logical_parallelism) noexcept {
  return background_threads_for_parallelism(logical_parallelism) > 0;
}

struct parallelism_config {
  std::size_t logical = 1;
  std::size_t background = 0;
};

inline constexpr parallelism_config resolve_parallelism(std::size_t requested, std::size_t fallback = 1) noexcept {
  const std::size_t base = requested == 0 ? fallback : requested;
  const std::size_t logical = base == 0 ? std::size_t{1} : base;
  return {logical, background_threads_for_parallelism(logical)};
}

static_assert(background_threads_for_parallelism(0) == 0, "parallelism conversion must allow zero");
static_assert(background_threads_for_parallelism(1) == 0, "single-thread parallelism has no background workers");
static_assert(background_threads_for_parallelism(4) == 3, "parallelism conversion mismatch");
static_assert(logical_parallelism_from_background(0) == 1, "zero background implies single logical thread");
static_assert(logical_parallelism_from_background(5) == 6, "background to logical conversion mismatch");

}
