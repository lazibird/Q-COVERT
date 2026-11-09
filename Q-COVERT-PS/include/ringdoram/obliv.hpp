#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <functional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

#include "sonic/crypto/buffered_prng.hpp"
#include "sonic/obliv/ops/bulk_primitives.hpp"
#include "sonic/obliv/ops/core_ops.hpp"
#include "sonic/obliv/ops/struct_ops.hpp"
#include "sonic/obliv/types/choice.hpp"
#include "sonic/sortshuffle/par/bitonic.hpp"
#include "sonic/sortshuffle/par/orshuffle.hpp"
#include "sonic/sortshuffle/ser/bitonic.hpp"
#include "sonic/sortshuffle/ser/orshuffle.hpp"
#include "sonic/threads/thread_team.hpp"

namespace ringdoram::obliv {

using choice = sn::obliv::choice;
using secure_random = sn::crypto::buffered_prng<>;

template <typename T>
[[nodiscard]] inline T OSelect(const T& when_true, const T& when_false, bool condition) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "OSelect requires trivially-copyable values");
  if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
    return sn::obliv::ct_select<T>(when_true, when_false, condition);
  } else if constexpr (std::is_floating_point_v<T> && sizeof(T) == 4) {
    std::uint32_t a, b;
    std::memcpy(&a, &when_true, 4);
    std::memcpy(&b, &when_false, 4);
    const auto bits = sn::obliv::ct_select(a, b, condition);
    T result;
    std::memcpy(&result, &bits, 4);
    return result;
  } else {
    return sn::obliv::ct_select_data<T>(when_true, when_false, condition);
  }
}

template <typename T> inline void OSwap(T& left, T& right, bool condition) noexcept {
  static_assert(std::is_trivially_copyable_v<T>, "OSwap requires trivially-copyable values");
  if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>) {
    sn::obliv::ct_swap<T>(&left, &right, condition);
  } else {
    sn::obliv::ct_swap_data<T>(&left, &right, condition);
  }
}

// Items must expose leaf_node_id and bucket_id, and be sorted by leaf_node_id.
// A nonzero first_bucket_depth excludes virtual forest levels above subtree roots.
template <typename T>
inline std::size_t OAdjustBucketID(
    std::span<T> items, std::uint32_t height, std::uint32_t bucket_real_size, std::uint64_t stash_bucket_id,
    std::uint32_t first_bucket_depth = 0
) {
  return sn::obliv::oadjust_bucket_id(
      sn::util::span<T>(items.data(), items.size()), height, bucket_real_size, stash_bucket_id, first_bucket_depth
  );
}

// Valid keys must be unique positions; dummies use invalid_key. The input
// length is a public power of two, and valid entries precede dummies.
template <typename T, typename KeyExtractor>
inline void OPlace(std::span<T> items, KeyExtractor key, std::uint64_t invalid_key) {
  sn::obliv::oplace(sn::util::span<T>(items.data(), items.size()), key, invalid_key);
}

template <typename T, typename KeyExtractor, typename Compare>
inline void OSort(std::span<T> values, KeyExtractor key, Compare compare) {
  sn::sortshuffle::ser::bitonic::bitonic_sort(values.data(), values.size(), key, compare);
}

template <typename T> inline void OSort(std::span<T> values) {
  sn::sortshuffle::ser::bitonic::bitonic_sort(values.data(), values.size());
}

template <typename T, typename KeyExtractor, typename Compare>
inline void OSortParallel(
    std::span<T> values, sn::threads::thread_team& workers, KeyExtractor key, Compare compare
) {
  sn::sortshuffle::par::bitonic_sort(values.data(), values.size(), workers, key, compare);
}

template <typename T> inline void OSortParallel(std::span<T> values, sn::threads::thread_team& workers) {
  sn::sortshuffle::par::bitonic_sort(values.data(), values.size(), workers);
}

class compact_scratch {
public:
  void reserve(std::size_t count) {
    marks_.resize(count);
    prefix_.resize(count + 1);
  }

private:
  template <typename T>
  friend void OCompact(std::span<T>, std::span<const std::uint8_t>, compact_scratch&);
  template <typename T>
  friend void OCompactParallel(
      std::span<T>, std::span<const std::uint8_t>, compact_scratch&, sn::threads::thread_team&
  );

  std::vector<std::uint8_t> marks_{};
  std::vector<std::size_t> prefix_{};
};

template <typename T>
inline void OCompact(std::span<T> values, std::span<const std::uint8_t> marks, compact_scratch& scratch) {
  if (values.size() != marks.size()) {
    throw std::invalid_argument("OCompact: values and marks must have equal length");
  }
  scratch.reserve(values.size());
  std::copy(marks.begin(), marks.end(), scratch.marks_.begin());
  sn::sortshuffle::ser::orshuffle::orcompact(
      values.data(), values.size(), scratch.marks_.data(), scratch.prefix_.data()
  );
}

template <typename T>
inline void OCompactParallel(
    std::span<T> values, std::span<const std::uint8_t> marks, compact_scratch& scratch,
    sn::threads::thread_team& workers
) {
  if (values.size() != marks.size()) {
    throw std::invalid_argument("OCompactParallel: values and marks must have equal length");
  }
  scratch.reserve(values.size());
  std::copy(marks.begin(), marks.end(), scratch.marks_.begin());
  sn::sortshuffle::par::orshuffle::orcompact(
      values.data(), values.size(), scratch.marks_.data(), scratch.prefix_.data(), workers
  );
}

class shuffle_scratch {
public:
  void reserve(std::size_t count) {
    marks_.resize(count);
    prefix_.resize(count + 1);
    keys_.resize(count);
  }

private:
  template <typename T, typename Rng>
  friend void OShuffle(std::span<T>, shuffle_scratch&, Rng&);
  template <typename T, typename Rng>
  friend void OShuffleBitonic(std::span<T>, shuffle_scratch&, Rng&);
  template <typename T, typename Rng>
  friend void OShuffleBitonicParallel(
      std::span<T>, shuffle_scratch&, Rng&, sn::threads::thread_team&
  );

  std::vector<std::uint8_t> marks_{};
  std::vector<std::size_t> prefix_{};
  std::vector<std::uint64_t> keys_{};
};

// ORShuffle from Sasy et al. The output is an oblivious random permutation.
template <typename T, typename Rng>
inline void OShuffle(std::span<T> values, shuffle_scratch& scratch, Rng& random) {
  scratch.reserve(values.size());
  sn::sortshuffle::ser::orshuffle::orshuffle(
      values.data(), values.size(), scratch.marks_.data(), scratch.prefix_.data(), random
  );
}

// Random-key bitonic shuffle used by SONIC for small buffers.
template <typename T, typename Rng>
inline void OShuffleBitonic(std::span<T> values, shuffle_scratch& scratch, Rng& random) {
  scratch.reserve(values.size());
  sn::sortshuffle::ser::bitonic::bitonic_shuffle_with_keys(
      values.data(), values.size(), scratch.keys_.data(), random
  );
}

template <typename T, typename Rng>
inline void OShuffleBitonicParallel(
    std::span<T> values, shuffle_scratch& scratch, Rng& random, sn::threads::thread_team& workers
) {
  scratch.reserve(values.size());
  random.random_bytes(
      reinterpret_cast<std::uint8_t*>(scratch.keys_.data()), scratch.keys_.size() * sizeof(std::uint64_t)
  );
  sn::sortshuffle::par::bitonic_sort_with_keys(
      values.data(), values.size(), scratch.keys_.data(), workers
  );
}

} // namespace ringdoram::obliv
