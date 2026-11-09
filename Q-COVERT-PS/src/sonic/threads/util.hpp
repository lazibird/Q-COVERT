#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace sn::threads {

inline std::pair<std::size_t, std::size_t> partition_evenly(
    std::size_t logical_index, std::size_t total_items, std::size_t partitions
) noexcept {
  if (partitions == 0 || logical_index >= partitions) {
    return {0, 0};
  }

  const std::size_t base = total_items / partitions;
  const std::size_t remainder = total_items % partitions;
  const std::size_t chunk_begin = logical_index * base + (logical_index < remainder ? logical_index : remainder);
  const std::size_t chunk_end = chunk_begin + base + (logical_index < remainder ? 1 : 0);

  return {chunk_begin, chunk_end};
}

}
