#pragma once
#include "qcovert/types.hpp"
#include "ringdoram/obliv.hpp"
#include <bit>
#include <cmath>
#include <span>
namespace compass {
using qcovert::invalid_id;
using qcovert::infinity;
using qcovert::result;
enum class metric : std::uint64_t { l2 = 0, inner_product = 1 };
struct parameters {
  std::size_t ef = 20, ef_spec = 4, ef_n = 12, k = 10, upper_steps = 1;
};
struct oram_parameters {
  std::uint32_t z = 8, s = 16, a = 4, evict_batch = 1;
  std::uint64_t stash = 0;
};
inline std::size_t multiply(std::size_t a, std::size_t b) {
  if (b && a > SIZE_MAX / b) throw std::length_error("Compass size overflow");
  return a * b;
}
inline std::uint64_t score_key(double x) {
  std::uint64_t bits;
  std::memcpy(&bits, &x, sizeof(bits));
  bits = ringdoram::obliv::OSelect(std::uint64_t{0}, bits, (bits << 1) == 0);
  return bits ^ ((std::uint64_t{0} - (bits >> 63)) | (std::uint64_t{1} << 63));
}
inline bool nearer(double a, std::uint64_t aid, double b, std::uint64_t bid) {
  const auto ka = score_key(a), kb = score_key(b);
  return sn::obliv::ct_lt(ka, kb) | (sn::obliv::ct_eq(ka, kb) & sn::obliv::ct_lt(aid, bid));
}
template <std::size_t D, std::size_t M, std::size_t P, std::size_t K = 256, class Scalar = float>
struct layout {
  static_assert(D && M && P && D % P == 0 && K && K <= 256);
  static_assert(std::is_same_v<Scalar, float> || std::is_same_v<Scalar, std::uint8_t>);
  static constexpr std::size_t dim = D, degree = M, subspaces = P, centroids = K, subdim = D / P;
  using scalar = Scalar;
  using vector = std::array<Scalar, D>;
  using query = std::array<float, D>;
  using code = std::array<std::uint8_t, P>;
  struct neighbor { std::uint64_t id = invalid_id; code pq{}; };
  template <std::size_t R> struct alignas(8) layer_node {
    std::uint64_t id = invalid_id;
    vector embedding{};
    std::array<neighbor, R> neighbors{};
  };
  using node = layer_node<M>;
  using upper_node = layer_node<M / 2>;
  using codebook = std::array<float, P * K * subdim>;
  using distance_table = std::array<double, P * K>;
  struct dataset {
    metric distance = metric::l2;
    std::uint64_t entry = 0;
    codebook book{};
    std::vector<node> nodes;
    std::uint64_t scan_level = 0;
    std::vector<upper_node> upper_nodes, scan_nodes;
  };
  static double exact(const query &q, const vector &v, metric m) {
    double sum = 0;
    for (std::size_t d = 0; d < D; ++d) {
      const double x = q[d], y = v[d];
      // Metric is a public index parameter.
      sum += m == metric::l2 ? (x - y) * (x - y) : -x * y;
    }
    return sum;
  }
  static distance_table make_table(const query &q, const codebook &book, metric m) {
    distance_table table{};
    for (std::size_t p = 0; p < P; ++p)
      for (std::size_t c = 0; c < K; ++c)
        for (std::size_t d = 0; d < subdim; ++d) {
          const double x = q[p * subdim + d], y = book[(p * K + c) * subdim + d];
          table[p * K + c] += m == metric::l2 ? (x - y) * (x - y) : -x * y;
        }
    return table;
  }
  static double approximate(const code &pq, const distance_table &table) {
    double sum = 0;
    for (std::size_t p = 0; p < P; ++p) {
      double selected = 0;
      // Scan every centroid. The PQ byte never addresses memory.
      for (std::size_t c = 0; c < K; ++c)
        sn::obliv::ct_set_data(&selected, table[p * K + c],
                              sn::obliv::ct_eq(std::uint64_t(pq[p]), std::uint64_t(c)));
      sum += selected;
    }
    return sum;
  }
};
// Explicitly opt-in diagnostics for public experiments only. Never enable on
// private queries: these records disclose IDs and effective candidate counts.
struct diagnostics {
  std::size_t rounds = 0, logical_reads = 0, batch_size = 0;
  std::uint64_t scan_entry = invalid_id, bottom_entry = invalid_id;
  std::vector<std::uint64_t> upper_expanded, upper_fetched;
  std::vector<std::uint64_t> expanded, fetched;
  std::vector<std::size_t> pending_candidates;
};
struct measurement {
  using clock_fn = std::uint64_t (*)();
  clock_fn clock = nullptr;
  std::uint64_t total_ns = 0, oram_ns = 0, eviction_ns = 0;
  std::uint64_t sets_ns = 0, pq_ns = 0, exact_ns = 0, position_ns = 0;
  std::uint64_t upper_scan_ns = 0;
};
} // namespace compass
