#pragma once
// POD/SGX specialization of the migrated oblivious_topk block-sort/reduction
// network. Original module license: vendor/oblivious_topk.LICENSE.
#include "ringdoram/obliv.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
namespace qcovert {
inline std::size_t padded_size(std::size_t n) {
  if (n > (std::size_t{1} << (std::numeric_limits<std::size_t>::digits - 2)))
    throw std::length_error("network input too large");
  return std::bit_ceil(std::max(n, std::size_t{1}));
}
template <class T, class Less>
void exact_topk(std::vector<T> &items, std::size_t k, const T &padding, Less less) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (k > items.size())
    throw std::invalid_argument("top-k exceeds input size");
  if (k == 0) {
    items.clear();
    return;
  }
  const auto p = padded_size(k), blocks = padded_size((items.size() + p - 1) / p);
  if (blocks > std::numeric_limits<std::size_t>::max() / p)
    throw std::length_error("top-k overflow");
  items.resize(blocks * p, padding);
  auto gate = [&](std::size_t a, std::size_t b, bool ascending) {
    const bool exchange = ascending ? less(items[b], items[a]) : less(items[a], items[b]); // public direction
    ringdoram::obliv::OSwap(items[a], items[b], exchange);
  };
  for (std::size_t base = 0; base < items.size(); base += p)
    for (std::size_t seq = 2; seq <= p; seq *= 2)
      for (std::size_t d = seq / 2; d; d /= 2)
        for (std::size_t i = 0; i < p; ++i)
          if ((i ^ d) > i)
            gate(base + i, base + (i ^ d), (i & seq) == 0);
  for (std::size_t stride = 1; stride < blocks; stride *= 2) {
    for (std::size_t block = 0; block < blocks; block += 2 * stride) {
      const auto base = block * p, other = (block + stride) * p;
      for (std::size_t i = 0; i < p; ++i)
        gate(base + i, other + p - 1 - i, true);
      for (std::size_t d = p / 2; d; d /= 2)
        for (std::size_t i = 0; i < p; ++i)
          if ((i ^ d) > i)
            gate(base + i, base + (i ^ d), true);
    }
  }
  items.resize(k);
}
// Select compact keys first, then obliviously compact the matching payloads.
// This avoids moving large payloads through the full selection network. The
// membership scan and compaction have public loop bounds and memory addresses;
// source positions are never used to index the payload array. The final small
// network restores sorted top-k order (compaction alone does not sort by key).
template <class T> struct compact_topk_workspace {
  struct alignas(8) payload { T value; };
  std::vector<payload> payloads;
  std::vector<std::uint64_t> sources;
  std::vector<std::uint8_t> marks;
  ringdoram::obliv::compact_scratch compact;
};
template <class T, class KeyOf, class LessKey>
void exact_topk_compact(std::vector<T> &items, std::size_t k, const T &padding,
                        KeyOf key_of, LessKey less_key, compact_topk_workspace<T> &workspace) {
  using Key = std::decay_t<decltype(key_of(padding))>;
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_copyable_v<Key>);
  if (k > items.size())
    throw std::invalid_argument("top-k exceeds input size");
  if (k == 0) {
    items.clear();
    return;
  }
  // For an all-input selection there is no payload movement to avoid.
  if (k == items.size()) {
    exact_topk(items, k, padding, [&](const T &a, const T &b) {
      return less_key(key_of(a), key_of(b));
    });
    return;
  }
  struct alignas(16) record {
    Key key;
    std::uint64_t source;
  };
  const auto invalid = std::numeric_limits<std::uint64_t>::max();
  std::vector<record> keys;
  keys.reserve(items.size());
  for (std::size_t i = 0; i < items.size(); ++i)
    keys.push_back({key_of(items[i]), i});
  exact_topk(keys, k, record{key_of(padding), invalid}, [&](const record &a, const record &b) {
    return less_key(a.key, b.key) |
           ((!less_key(b.key, a.key)) & sn::obliv::ct_lt(a.source, b.source));
  });
  auto &sources = workspace.sources;
  sources.resize(k);
  for (std::size_t r = 0; r < k; ++r) sources[r] = keys[r].source;
  using payload = typename compact_topk_workspace<T>::payload;
  auto &payloads = workspace.payloads;
  auto &marks = workspace.marks;
  payloads.resize(items.size());
  marks.resize(items.size());
  for (std::size_t i = 0; i < items.size(); ++i) {
    std::uint32_t chosen = 0;
    std::size_t r = 0;
#if defined(__SSE2__)
    // SSE2 has 32-bit equality lanes. Pair adjacent limbs to compare two full
    // 64-bit source positions; scan every pair even after finding a match.
    const auto needle = _mm_set1_epi64x(static_cast<std::int64_t>(i));
    auto matches = _mm_setzero_si128();
    for (; r + 1 < k; r += 2) {
      const auto values = _mm_loadu_si128(reinterpret_cast<const __m128i *>(sources.data() + r));
      const auto limbs = _mm_cmpeq_epi32(values, needle);
      const auto pairs = _mm_and_si128(limbs, _mm_shuffle_epi32(limbs, _MM_SHUFFLE(2, 3, 0, 1)));
      matches = _mm_or_si128(matches, pairs);
    }
    chosen = static_cast<std::uint32_t>(_mm_movemask_epi8(matches));
#endif
    for (; r < k; ++r)
      chosen |= std::uint32_t(sn::obliv::ct_eq(sources[r], std::uint64_t(i)));
    marks[i] = std::uint8_t(chosen != 0);
    payloads[i].value = items[i];
  }
  ringdoram::obliv::OCompact(std::span<payload>(payloads), marks, workspace.compact);
  items.resize(k);
  for (std::size_t i = 0; i < k; ++i)
    items[i] = payloads[i].value;
  exact_topk(items, k, padding, [&](const T &a, const T &b) {
    return less_key(key_of(a), key_of(b));
  });
}
template <class T, class KeyOf, class LessKey>
void exact_topk_compact(std::vector<T> &items, std::size_t k, const T &padding,
                        KeyOf key_of, LessKey less_key) {
  compact_topk_workspace<T> workspace;
  exact_topk_compact(items, k, padding, key_of, less_key, workspace);
}
// Each selected cluster occupies one contiguous, public region. Split that
// region into rank_partitions[r] nearly equal contiguous partitions, select
// one oblivious minimum per partition, then use the exact selection network.
// Public partition boundaries may cross storage-block boundaries.
template <class T, class Less>
void partition_topk(std::vector<T> &items, std::size_t k, std::size_t region,
                    std::span<const std::size_t> rank_partitions, const T &padding, Less less) {
  static_assert(std::is_trivially_copyable_v<T>);
  if (!region || rank_partitions.size() > items.size() / region ||
      rank_partitions.size() * region != items.size())
    throw std::invalid_argument("partition top-k shape mismatch");
  std::size_t total = 0;
  for (auto quota : rank_partitions) {
    if (quota > region || quota > std::numeric_limits<std::size_t>::max() - total)
      throw std::invalid_argument("partition quota exceeds cluster capacity");
    total += quota;
  }
  if (total < k)
    throw std::invalid_argument("partition representatives fewer than k2");
  std::vector<T> representatives;
  representatives.reserve(total);
  for (std::size_t r = 0; r < rank_partitions.size(); ++r) {
    const auto quota = rank_partitions[r];
    for (std::size_t p = 0; p < quota; ++p) {
      const auto begin = p * region / quota;
      const auto end = (p + 1) * region / quota;
      T best = padding;
      for (std::size_t j = begin; j < end; ++j)
        sn::obliv::ct_set_data(&best, items[r * region + j], less(items[r * region + j], best));
      representatives.push_back(best);
    }
  }
  exact_topk(representatives, k, padding, less);
  items = std::move(representatives);
}
} // namespace qcovert
