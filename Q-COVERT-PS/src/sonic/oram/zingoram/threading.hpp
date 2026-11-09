#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#include "sonic/oram/zingoram/traits.hpp"
#include "sonic/threads/parallelism.hpp"
#include "sonic/util/log.hpp"

namespace sn::oram::zingoram {

struct threading_input {
  std::size_t access = 1;
  std::size_t eviction = 0; // 0 => auto
  std::uint32_t routing_depth = 0;
  bool online_only = false;
};

struct threading {
  sn::threads::parallelism_config access{};
  sn::threads::parallelism_config eviction{};
  std::optional<sn::threads::parallelism_config> domain{};
};

[[nodiscard]] inline std::size_t subtree_count(std::uint32_t routing_depth) {
  sn::util::log::ensure(
      routing_depth < std::numeric_limits<std::size_t>::digits, "zingoram::threading: routing depth is too large"
  );
  return std::size_t{1} << routing_depth;
}

[[nodiscard]] inline std::size_t suggested_eviction_threads(std::uint32_t routing_depth) {
  return std::max<std::size_t>(subtree_count(routing_depth), 1);
}

[[nodiscard]] inline std::size_t fit_eviction_threads(std::uint32_t routing_depth, std::size_t limit) {
  if (limit <= 1) {
    return 1;
  }

  const std::size_t max_threads = suggested_eviction_threads(routing_depth);
  std::size_t fitted = std::min(max_threads, limit);
  while (max_threads % fitted != 0) {
    --fitted;
  }
  return std::max<std::size_t>(fitted, 1);
}

[[nodiscard]] inline bool valid_eviction_threads(std::uint32_t routing_depth, std::size_t eviction_threads) {
  if (eviction_threads == 0) {
    return false;
  }
  const std::size_t max_threads = suggested_eviction_threads(routing_depth);
  return max_threads % eviction_threads == 0;
}

template <epoch_mode Mode> [[nodiscard]] inline threading resolve_threading(const threading_input& in) {
  threading plan{};
  plan.access = sn::threads::resolve_parallelism(in.access);

  std::size_t eviction_logical = 1;
  if constexpr (Mode == epoch_mode::disjoint_epoch) {
    if (!in.online_only) {
      eviction_logical = in.eviction != 0 ? in.eviction : fit_eviction_threads(in.routing_depth, plan.access.logical);
    }
  } else {
    eviction_logical = in.eviction != 0 ? in.eviction : suggested_eviction_threads(in.routing_depth);
  }

  sn::util::log::ensure(
      valid_eviction_threads(in.routing_depth, eviction_logical),
      "zingoram::threading: eviction logical threads must divide subtree_count"
  );

  plan.eviction = sn::threads::resolve_parallelism(eviction_logical);
  if constexpr (Mode == epoch_mode::disjoint_epoch) {
    plan.domain = sn::threads::resolve_parallelism(std::max(plan.access.logical, plan.eviction.logical));
  }
  return plan;
}

} // namespace sn::oram::zingoram
