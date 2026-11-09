// Isolate centroid distance/selection without rebuilding or changing ORAM state.
#include "qcovert/codec.hpp"
#include "qcovert/topk.hpp"
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace {
using L = qcovert::layout<128, std::uint8_t>;
struct centroid_record {
  L::centroid c{};
  std::uint64_t cid = qcovert::invalid_id, counter = 0;
  double score = qcovert::infinity;
};
std::uint64_t ordered(double value) {
  std::uint64_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  bits = ringdoram::obliv::OSelect(std::uint64_t{0}, bits, (bits << 1) == 0);
  return bits ^ ((std::uint64_t{0} - (bits >> 63)) | (std::uint64_t{1} << 63));
}
auto key(const centroid_record &c) { return std::array<std::uint64_t, 2>{ordered(c.score), c.cid}; }
bool less_key(const auto &a, const auto &b) {
  return sn::obliv::ct_lt(a[0], b[0]) |
         (sn::obliv::ct_eq(a[0], b[0]) & sn::obliv::ct_lt(a[1], b[1]));
}
auto now() { return std::chrono::steady_clock::now(); }
double elapsed_ms(auto start) { return std::chrono::duration<double, std::milli>(now() - start).count(); }
template <class T> T read(std::istream &in) {
  T x{};
  if (!in.read(reinterpret_cast<char *>(&x), sizeof(x))) throw std::runtime_error("truncated input");
  return x;
}
std::size_t positive(const char *text) {
  std::size_t consumed = 0;
  const auto n = std::stoull(text, &consumed);
  if (!n || text[consumed] || text[0] == '-' || n > 1000000)
    throw std::invalid_argument("expected positive integer <= 1000000");
  return n;
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc < 3 || argc > 6) {
      std::cerr << "usage: qcovert_centroid_benchmark prepared.bin queries.fvecs [nprobe=100] [queries=100] [repeats=3]\n";
      return 2;
    }
    const auto probes = argc > 3 ? positive(argv[3]) : 100;
    const auto count = argc > 4 ? positive(argv[4]) : 100;
    const auto repeats = argc > 5 ? positive(argv[5]) : 3;
    std::ifstream prepared(argv[1], std::ios::binary);
    if (!prepared) throw std::runtime_error("cannot open prepared file");
    if (read<std::uint64_t>(prepared) != qcovert::wire::magic || read<std::uint64_t>(prepared) != 128)
      throw std::runtime_error("expected 128-dimensional prepared file");
    const auto scalar = read<std::uint64_t>(prepared);
    if (scalar != 1 && scalar != 2) throw std::runtime_error("unsupported scalar type");
    (void)read<std::uint64_t>(prepared); // N: vector payloads are not needed.
    const auto nc = read<std::uint64_t>(prepared);
    (void)read<std::uint64_t>(prepared); // tau
    if (!nc || nc > 1000000 || probes > nc) throw std::runtime_error("invalid centroid count/nprobe");
    prepared.seekg((128 + 1) * L::padded * sizeof(float), std::ios::cur);
    std::vector<centroid_record> centroids(nc);
    for (std::size_t i = 0; i < nc; ++i) {
      auto &c = centroids[i];
      c.cid = i;
      c.c.valid = read<std::uint64_t>(prepared);
      if (c.c.valid > 1) throw std::runtime_error("invalid centroid validity flag");
      for (auto &x : c.c.values) {
        x = read<float>(prepared);
        if (!std::isfinite(x)) throw std::runtime_error("nonfinite centroid");
      }
      for (auto &x : c.c.rotated) x = read<float>(prepared);
    }
    std::ifstream input(argv[2], std::ios::binary);
    if (!input) throw std::runtime_error("cannot open fvecs query file");
    std::vector<std::array<float, 128>> queries(count);
    for (auto &query : queries) {
      if (read<std::uint32_t>(input) != 128) throw std::runtime_error("expected 128-dimensional fvecs");
      for (auto &x : query) {
        x = read<float>(input);
        if (!std::isfinite(x) || (scalar == 2 && (x < 0 || x > 255 || x != std::trunc(x))))
          throw std::runtime_error("query incompatible with prepared scalar type");
      }
    }
    double distance_ms = 0, old_ms = 0, new_ms = 0;
    qcovert::compact_topk_workspace<centroid_record> workspace;
    // One untimed query warms both implementations; alternate their order to
    // avoid consistently favouring the second implementation's warmed caches.
    for (std::size_t round = 0; round <= repeats; ++round) {
      for (std::size_t q = 0; q < (round ? count : 1); ++q) {
        auto scores = centroids;
        auto start = now();
        for (auto &c : scores) {
          c.score = 0;
          for (std::size_t d = 0; d < 128; ++d) {
            const double delta = double(queries[q][d]) - c.c.values[d];
            c.score += delta * delta;
          }
          c.score = ringdoram::obliv::OSelect(c.score, qcovert::infinity, c.c.valid != 0);
        }
        const auto distance = elapsed_ms(start);
        auto before = scores, after = scores;
        double old_time = 0, new_time = 0;
        const auto old_select = [&] {
          auto t = now();
          qcovert::exact_topk(before, probes, centroid_record{}, [](const auto &a, const auto &b) {
            return less_key(key(a), key(b));
          });
          old_time = elapsed_ms(t);
        };
        const auto new_select = [&] {
          auto t = now();
          qcovert::exact_topk_compact(after, probes, centroid_record{}, key,
                                    [](const auto &a, const auto &b) { return less_key(a, b); }, workspace);
          new_time = elapsed_ms(t);
        };
        if ((round + q) % 2) { old_select(); new_select(); }
        else { new_select(); old_select(); }
        for (std::size_t i = 0; i < probes; ++i)
          if (before[i].cid != after[i].cid || before[i].score != after[i].score ||
              before[i].counter != after[i].counter || before[i].c.valid != after[i].c.valid ||
              before[i].c.values != after[i].c.values || before[i].c.rotated != after[i].c.rotated)
            throw std::runtime_error("old/new selected centroids differ");
        if (round) { distance_ms += distance; old_ms += old_time; new_ms += new_time; }
      }
    }
    const auto trials = double(count) * repeats;
    distance_ms /= trials;
    old_ms /= trials;
    new_ms /= trials;
    std::cout << "clusters,n_probe,queries,repeats,record_bytes,distance_ms,old_topk_ms,new_topk_ms,old_candidate_clusters_ms,new_candidate_clusters_ms,reduction_pct,identical_results\n"
              << std::fixed << std::setprecision(6) << nc << ',' << probes << ',' << count << ',' << repeats
              << ',' << sizeof(centroid_record) << ',' << distance_ms << ',' << old_ms << ',' << new_ms
              << ',' << distance_ms + old_ms << ',' << distance_ms + new_ms << ','
              << 100 * (old_ms - new_ms) / (distance_ms + old_ms) << ",1\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
