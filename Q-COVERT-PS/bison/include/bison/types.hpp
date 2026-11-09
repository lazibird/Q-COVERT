#pragma once
#include "qcovert/types.hpp"
#include "ringdoram/obliv.hpp"
#include <cmath>
#include <span>
namespace bison {
using qcovert::invalid_id;
using qcovert::infinity;
using qcovert::result;
inline std::size_t multiply(std::size_t a, std::size_t b) {
  if (b && a > SIZE_MAX / b) throw std::length_error("BiSON size overflow");
  return a * b;
}
inline std::size_t add(std::size_t a, std::size_t b) {
  if (a > SIZE_MAX - b) throw std::length_error("BiSON size overflow");
  return a + b;
}
inline std::uint32_t score_key(float x) {
  std::uint32_t bits;
  std::memcpy(&bits, &x, sizeof(bits));
  bits = ringdoram::obliv::OSelect(std::uint32_t{0}, bits, (bits << 1) == 0);
  return bits ^ ((std::uint32_t{0} - (bits >> 31)) | (std::uint32_t{1} << 31));
}
inline bool nearer(float a, std::uint64_t aid, float b, std::uint64_t bid) {
  return sn::obliv::ct_lt(score_key(a), score_key(b)) |
      (sn::obliv::ct_eq(score_key(a), score_key(b)) & sn::obliv::ct_lt(aid, bid));
}
struct parameters { std::size_t iterations = 7, beamwidth = 13, l_search = 125, k = 10; };
struct oram_parameters { std::uint32_t z = 8, s = 16, a = 4, evict_batch = 1; std::uint64_t stash = 0; };
struct candidate {
  std::uint64_t id = invalid_id;
  float distance = std::numeric_limits<float>::infinity();
  std::uint32_t reserved = 0; // public zero padding; no expanded/tombstone state
};
inline bool candidate_less(const candidate &a, const candidate &b) {
  return nearer(a.distance, a.id, b.distance, b.id);
}
struct measurement {
  using clock_fn = std::uint64_t (*)();
  clock_fn clock = nullptr;
  std::uint64_t total_ns = 0, oram_ns = 0, eviction_ns = 0, sets_ns = 0;
  std::uint64_t pq_ns = 0, exact_ns = 0, position_ns = 0, cache_ns = 0;
};
// Public-data tests only: exposing these IDs on private queries breaks privacy.
struct diagnostics {
  std::uint64_t entry = invalid_id;
  std::vector<std::uint64_t> expanded, fetched;
  std::vector<std::vector<candidate>> candidates;
};
template <std::size_t D, std::size_t R, std::size_t P, class Scalar>
struct layout {
  static_assert(D && D <= 4096 && R && P && P <= D);
  static_assert(std::is_same_v<Scalar, float> || std::is_same_v<Scalar, std::uint8_t>);
  static constexpr std::size_t dim = D, degree = R, subspaces = P, centroids = 256;
  using scalar = Scalar;
  using query = std::array<float, D>;
  using vector = std::array<Scalar, D>;
  using code = std::array<std::uint8_t, P>;
  struct neighbor { std::uint64_t id = invalid_id; code pq{}; };
  struct alignas(16) node {
    std::uint64_t id = invalid_id;
    std::uint32_t degree = 0, reserved = 0;
    vector embedding{};
    code pq{};
    std::array<neighbor, R> neighbors{};
  };
  struct model {
    std::array<float, 256 * D> pivots{}; // dimension-major, as DiskANN tables_tr
    query mean{};
    std::array<std::uint32_t, P + 1> offsets{};
    alignas(16) std::array<float, D * D> rotation{}; // row-major input/output convention
    std::uint64_t rotated = 0;
  };
  struct medoid { std::uint64_t id = invalid_id; query centroid{}; code pq{}; };
  struct dataset {
    model book{};
    std::vector<node> nodes, cache;
    std::vector<medoid> medoids;
    std::uint64_t cached_rounds = 0; // legacy prepared metadata, never affects queries
    std::uint64_t legacy_cache_count = 0;
  };
  using distance_table = std::array<float, P * 256>;
  static float exact(const query &q, const vector &v) {
    float sum = 0;
    for (std::size_t d = 0; d < D; ++d) { const float diff = q[d] - float(v[d]); sum += diff * diff; }
    return sum;
  }
  static float centroid_distance(const query &q, const query &v) {
    float sum = 0;
    for (std::size_t d = 0; d < D; ++d) { const float diff = q[d] - v[d]; sum += diff * diff; }
    return sum;
  }
  static distance_table make_table(const query &q, const model &book) {
    query centered{}, transformed{};
    for (std::size_t d = 0; d < D; ++d) centered[d] = q[d] - book.mean[d];
    if (book.rotated) { // public model flag
      for (std::size_t d = 0; d < D; ++d)
        for (std::size_t j = 0; j < D; ++j) transformed[d] += centered[j] * book.rotation[j * D + d];
    } else transformed = centered;
    distance_table table{};
    for (std::size_t p = 0; p < P; ++p)
      for (std::size_t d = book.offsets[p]; d < book.offsets[p + 1]; ++d)
        for (std::size_t c = 0; c < 256; ++c) {
          const double diff = book.pivots[d * 256 + c] - transformed[d];
          table[p * 256 + c] += float(diff * diff);
        }
    return table;
  }
  static float approximate(const code &pq, const distance_table &table) {
    float sum = 0;
    for (std::size_t p = 0; p < P; ++p) {
      float chosen = 0;
      for (std::size_t c = 0; c < 256; ++c)
        chosen = ringdoram::obliv::OSelect(table[p * 256 + c], chosen,
            sn::obliv::ct_eq(std::uint64_t(pq[p]), std::uint64_t(c)));
      sum += chosen;
    }
    return sum;
  }
};
} // namespace bison
