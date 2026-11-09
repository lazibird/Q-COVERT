#pragma once
#include "bison/types.hpp"
#include "qcovert/codec.hpp"
namespace bison {
inline constexpr std::uint64_t dataset_magic = 0x3130304f44534942ULL; // BISDO001
template <class L> constexpr std::size_t wire_node_bytes() {
  return 16 + L::dim * sizeof(typename L::scalar) + L::subspaces + L::degree * (8 + L::subspaces);
}
template <class L> std::vector<std::uint8_t> encode(const typename L::dataset &data) {
  qcovert::wire::writer w;
  for (auto x : std::array<std::uint64_t, 12>{dataset_magic, L::dim, L::degree, L::subspaces,
      sizeof(typename L::scalar) == 1 ? 2ULL : 1ULL, data.nodes.size(), data.medoids.size(),
      0, 0, data.book.rotated, 0, 0}) w.put(x);
  for (auto x : data.book.pivots) w.put(x);
  for (auto x : data.book.mean) w.put(x);
  for (auto x : data.book.offsets) w.put(x);
  for (auto x : data.book.rotation) w.put(x);
  for (const auto &m : data.medoids) {
    w.put(m.id); for (auto x : m.centroid) w.put(x); for (auto x : m.pq) w.put(x);
  }
  auto put = [&](const auto &n) {
    w.put(n.id); w.put(n.degree); w.put(n.reserved);
    for (auto x : n.embedding) w.put(x);
    for (auto x : n.pq) w.put(x);
    for (const auto &e : n.neighbors) { w.put(e.id); for (auto x : e.pq) w.put(x); }
  };
  for (const auto &n : data.nodes) put(n);
  return std::move(w.bytes);
}
template <class L> typename L::dataset decode(std::span<const std::uint8_t> bytes, std::size_t max_n) {
  qcovert::wire::reader r{bytes};
  if (r.get<std::uint64_t>() != dataset_magic || r.get<std::uint64_t>() != L::dim ||
      r.get<std::uint64_t>() != L::degree || r.get<std::uint64_t>() != L::subspaces ||
      r.get<std::uint64_t>() != (sizeof(typename L::scalar) == 1 ? 2ULL : 1ULL))
    throw std::invalid_argument("BiSON prepared layout mismatch; configure BISON_DIM/DEGREE/PQ_BYTES/SCALAR");
  const auto n = r.get<std::uint64_t>(), medoids = r.get<std::uint64_t>(), cache = r.get<std::uint64_t>();
  typename L::dataset data;
  data.cached_rounds = r.get<std::uint64_t>(); data.book.rotated = r.get<std::uint64_t>();
  const auto metric = r.get<std::uint64_t>(), reserved = r.get<std::uint64_t>();
  if (!n || n > max_n || !medoids || medoids > n || cache > n || data.cached_rounds > 1000000 ||
      data.book.rotated > 1 || metric || reserved)
    throw std::invalid_argument("invalid BiSON prepared counts/metric");
  const auto model_bytes = (256 * L::dim + L::dim + L::dim * L::dim) * sizeof(float) + (L::subspaces + 1) * 4;
  const auto expected = add(model_bytes, add(multiply(medoids, 8 + 4 * L::dim + L::subspaces),
      multiply(add(n, cache), wire_node_bytes<L>())));
  if (bytes.size() - r.pos != expected) throw std::invalid_argument("BiSON prepared length mismatch");
  for (auto &x : data.book.pivots) x = r.get<float>();
  for (auto &x : data.book.mean) x = r.get<float>();
  for (auto &x : data.book.offsets) x = r.get<std::uint32_t>();
  for (auto &x : data.book.rotation) x = r.get<float>();
  data.medoids.resize(medoids);
  for (auto &m : data.medoids) {
    m.id = r.get<std::uint64_t>();
    for (auto &x : m.centroid) x = r.get<float>();
    for (auto &x : m.pq) x = r.get<std::uint8_t>();
  }
  auto get = [&](auto &v) {
    v.id = r.get<std::uint64_t>(); v.degree = r.get<std::uint32_t>(); v.reserved = r.get<std::uint32_t>();
    for (auto &x : v.embedding) x = r.get<typename L::scalar>();
    for (auto &x : v.pq) x = r.get<std::uint8_t>();
    for (auto &e : v.neighbors) { e.id = r.get<std::uint64_t>(); for (auto &x : e.pq) x = r.get<std::uint8_t>(); }
  };
  data.nodes.resize(n); data.legacy_cache_count = cache;
  for (auto &v : data.nodes) get(v);
  // Legacy BISDO001 BFS records are irrelevant to Appendix F queries.
  // Skip them without materializing an enclave graph cache.
  r.pos += multiply(cache, wire_node_bytes<L>());
  return data;
}
} // namespace bison
