#include <array>
#include <cassert>
#include <cstdint>

#include "ringdoram/obliv.hpp"
#include "sonic/threads/platform/pthread_thread_pool.hpp"

int main() {
  std::uint64_t left = 10;
  std::uint64_t right = 20;
  const auto selected = ringdoram::obliv::OSelect(left, right, true);
  ringdoram::obliv::OSwap(left, right, true);
  assert(selected == 10 && left == 20 && right == 10);

  sn::threads::pthread_thread_pool pool(3);
  sn::threads::thread_team workers(pool.pool(), 4);

  std::array<std::uint64_t, 8> values{{7, 3, 6, 1, 5, 0, 4, 2}};
  ringdoram::obliv::OSortParallel<std::uint64_t>(values, workers);

  const std::array<std::uint8_t, 8> keep{{1, 0, 1, 0, 1, 0, 1, 0}};
  ringdoram::obliv::compact_scratch compact;
  ringdoram::obliv::OCompactParallel<std::uint64_t>(values, keep, compact, workers);

  ringdoram::obliv::secure_random random;
  ringdoram::obliv::shuffle_scratch shuffle;
  ringdoram::obliv::OShuffle<std::uint64_t>(values, shuffle, random);
  return 0;
}
