#pragma once
#include "qcovert/crypto.hpp"
#include "qcovert/types.hpp"
#include "sonic/crypto/random.hpp"
#include <bit>
#include <span>
#include <stdexcept>
namespace qcovert {
// Version 1: little-endian, IEEE float32, field-by-field; no compiler padding.
namespace wire {
static_assert(std::endian::native == std::endian::little && sizeof(float) == 4 &&
              std::numeric_limits<float>::is_iec559);
struct writer {
  std::vector<std::uint8_t> bytes;
  template <class T> void put(const T &x) {
    auto *p = reinterpret_cast<const std::uint8_t *>(&x);
    bytes.insert(bytes.end(), p, p + sizeof(T));
  }
};
struct reader {
  std::span<const std::uint8_t> bytes;
  std::size_t pos = 0;
  template <class T> T get() {
    if (sizeof(T) > bytes.size() - pos)
      throw std::invalid_argument("truncated Q-COVERT file");
    T x;
    std::memcpy(&x, bytes.data() + pos, sizeof(T));
    pos += sizeof(T);
    return x;
  }
};
inline constexpr std::uint64_t magic = 0x3150435452564351ULL;
} // namespace wire
template <std::size_t D, class Scalar>
std::vector<std::uint8_t> encode_dataset(const typename layout<D, Scalar>::dataset &data) {
  wire::writer w;
  w.put(wire::magic);
  w.put(std::uint64_t(D));
  w.put(std::uint64_t(std::is_same_v<Scalar, float> ? 1 : 2));
  w.put(std::uint64_t(data.entries.size()));
  w.put(std::uint64_t(data.centroids.size()));
  w.put(data.tau);
  for (auto x : data.rotation)
    w.put(x);
  for (auto x : data.offsets)
    w.put(x);
  for (const auto &c : data.centroids) {
    w.put(c.valid);
    for (auto x : c.values)
      w.put(x);
    for (auto x : c.rotated)
      w.put(x);
  }
  for (const auto &e : data.entries) {
    w.put(e.cid);
    for (auto x : e.values)
      w.put(x);
    for (auto x : e.quantized.signs)
      w.put(x);
    w.put(e.quantized.norm);
    w.put(e.quantized.x0);
  }
  return std::move(w.bytes);
}
template <std::size_t D, class Scalar>
typename layout<D, Scalar>::dataset decode_dataset(std::span<const std::uint8_t> bytes, std::size_t max_n,
                                                   std::size_t max_nc) {
  using L = layout<D, Scalar>;
  wire::reader r{bytes};
  if (r.get<std::uint64_t>() != wire::magic || r.get<std::uint64_t>() != D ||
      r.get<std::uint64_t>() != (std::is_same_v<Scalar, float> ? 1 : 2))
    throw std::invalid_argument("dataset version/dimension/type mismatch");
  const auto n = r.get<std::uint64_t>(), nc = r.get<std::uint64_t>(), tau = r.get<std::uint64_t>();
  if (!n || !nc || !tau || n > max_n || nc > max_nc)
    throw std::invalid_argument("dataset exceeds configured limits");
  constexpr std::size_t model_bytes = (D + 1) * L::padded * 4, centroid_bytes = 8 + 4 * (D + L::padded),
                        entry_bytes = 8 + D * sizeof(Scalar) + L::padded / 8 + 8;
  std::size_t remaining = bytes.size() - r.pos;
  if (remaining < model_bytes)
    throw std::invalid_argument("truncated model");
  remaining -= model_bytes;
  if (nc > remaining / centroid_bytes)
    throw std::invalid_argument("truncated centroids");
  remaining -= nc * centroid_bytes;
  if (n > remaining / entry_bytes || remaining != n * entry_bytes)
    throw std::invalid_argument("dataset length mismatch");
  typename L::dataset data;
  data.tau = tau;
  data.entries.resize(n);
  data.centroids.resize(nc);
  data.rotation.resize(D * L::padded);
  for (auto &x : data.rotation)
    x = r.get<float>();
  for (auto &x : data.offsets)
    x = r.get<float>();
  for (auto &c : data.centroids) {
    c.valid = r.get<std::uint64_t>();
    for (auto &x : c.values)
      x = r.get<float>();
    for (auto &x : c.rotated)
      x = r.get<float>();
  }
  for (auto &e : data.entries) {
    e.cid = r.get<std::uint64_t>();
    for (auto &x : e.values)
      x = r.get<Scalar>();
    for (auto &x : e.quantized.signs)
      x = r.get<std::uint64_t>();
    e.quantized.norm = r.get<float>();
    e.quantized.x0 = r.get<float>();
  }
  return data;
}
inline std::vector<std::uint8_t> encrypt_dataset(std::span<const std::uint8_t> plain, const key128 &key) {
  nonce96 iv{};
  sn::crypto::random_device{}.fill(iv.data(), iv.size());
  if (plain.size() > std::numeric_limits<std::size_t>::max() - 28)
    throw std::length_error("dataset too large");
  std::vector<std::uint8_t> out(plain.size() + 28);
  std::memcpy(out.data(), iv.data(), 12);
  encrypt(key, iv, plain.data(), plain.size(), out.data() + 12, out.data() + 12 + plain.size());
  return out;
}
inline std::vector<std::uint8_t> decrypt_dataset(std::span<const std::uint8_t> cipher, const key128 &key) {
  if (cipher.size() < 28)
    throw std::invalid_argument("truncated encrypted dataset");
  nonce96 iv{};
  std::memcpy(iv.data(), cipher.data(), 12);
  std::vector<std::uint8_t> out(cipher.size() - 28);
  decrypt(key, iv, cipher.data() + 12, out.size(), cipher.data() + 12 + out.size(), out.data());
  return out;
}
} // namespace qcovert
