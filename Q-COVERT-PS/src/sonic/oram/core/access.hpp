#pragma once

#include <cstdint>

#include "sonic/util/span.hpp"

namespace sn::oram {

struct access_request {
  std::int64_t address = 0;
  std::int64_t cur_leaf = 0;
  std::int64_t new_leaf = 0;
  bool is_write = false;
  sn::util::span<std::uint8_t> in;
  sn::util::span<std::uint8_t> out;
};

// empty scratch marker for orams that don't require per-thread access scratch
struct no_access_scratch {};

} // namespace sn::oram
