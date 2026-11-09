#pragma once
#include "compass/types.hpp"
#include "qcovert/codec.hpp"
namespace compass {
inline constexpr std::uint64_t dataset_magic = 0x3330304f44504d43ULL; // CMPDO003
template <class L> std::vector<std::uint8_t> encode(const typename L::dataset &data) {
  qcovert::wire::writer w;
  for (auto x : std::array<std::uint64_t, 9>{dataset_magic, L::dim, L::degree, L::subspaces, L::centroids,
      sizeof(typename L::scalar) == 1 ? 2ULL : 1ULL, std::uint64_t(data.distance), data.nodes.size(), data.entry}) w.put(x);
  for (auto x : std::array<std::uint64_t, 3>{data.upper_nodes.size(), data.scan_nodes.size(), data.scan_level}) w.put(x);
  for (auto x : data.book) w.put(x);
  auto put_node = [&](const auto &n) {
    w.put(n.id);
    for (auto x : n.embedding) w.put(x);
    for (const auto &e : n.neighbors) { w.put(e.id); for (auto x : e.pq) w.put(x); }
  };
  for (const auto &n : data.nodes) put_node(n);
  for (const auto &n : data.upper_nodes) put_node(n);
  for (const auto &n : data.scan_nodes) put_node(n);
  return std::move(w.bytes);
}
template <class L> typename L::dataset decode(std::span<const std::uint8_t> bytes, std::size_t max_n) {
  qcovert::wire::reader r{bytes};
  const auto magic = r.get<std::uint64_t>();
  if (magic != dataset_magic)
    throw std::invalid_argument("Compass requires CMPDO003 data; repack HNSW/PQ or convert CMPDO002 with prepare.py --convert-prepared");
  if (r.get<std::uint64_t>() != L::dim ||
      r.get<std::uint64_t>() != L::degree || r.get<std::uint64_t>() != L::subspaces ||
      r.get<std::uint64_t>() != L::centroids || r.get<std::uint64_t>() != (sizeof(typename L::scalar) == 1 ? 2ULL : 1ULL))
    throw std::invalid_argument("Compass dataset layout mismatch; configure COMPASS_* to match preparation");
  typename L::dataset data;
  data.distance = metric(r.get<std::uint64_t>());
  const auto n = r.get<std::uint64_t>(); data.entry = r.get<std::uint64_t>();
  const auto upper = r.get<std::uint64_t>(), scan = r.get<std::uint64_t>();
  data.scan_level = r.get<std::uint64_t>();
  if (upper > n || !scan || scan > n || data.scan_level > 2 || L::degree % 2 ||
      (data.scan_level == 0 && (upper || scan != 1)) || (data.scan_level != 0 && !upper))
    throw std::invalid_argument("invalid Compass hierarchy counts");
  constexpr auto model_bytes = L::dim * L::centroids * sizeof(float);
  constexpr auto node_bytes = 8 + L::dim * sizeof(typename L::scalar) + L::degree * (8 + L::subspaces);
  constexpr auto upper_bytes = 8 + L::dim * sizeof(typename L::scalar) + (L::degree / 2) * (8 + L::subspaces);
  const auto remaining = bytes.size() - r.pos;
  if (!n || n > max_n || remaining < model_bytes ||
      n > (remaining - model_bytes) / node_bytes ||
      remaining - model_bytes - n * node_bytes != multiply(upper + scan, upper_bytes))
    throw std::invalid_argument("Compass dataset length/limit mismatch");
  data.nodes.resize(n);
  data.upper_nodes.resize(upper); data.scan_nodes.resize(scan);
  for (auto &x : data.book) x = r.get<float>();
  auto get_node = [&](auto &v) {
    v.id = r.get<std::uint64_t>();
    for (auto &x : v.embedding) x = r.get<typename L::scalar>();
    for (auto &e : v.neighbors) { e.id = r.get<std::uint64_t>(); for (auto &x : e.pq) x = r.get<std::uint8_t>(); }
  };
  for (auto &v : data.nodes) get_node(v);
  for (auto &v : data.upper_nodes) get_node(v);
  for (auto &v : data.scan_nodes) get_node(v);
  return data;
}
} // namespace compass
