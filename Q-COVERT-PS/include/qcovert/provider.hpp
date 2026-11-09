#pragma once
// Trusted OFFLINE provider only. Eigen, clustering, data-dependent code and I/O
// in this header must not be used for online enclave query processing.
#include "qcovert/codec.hpp"
#include "qcovert/quantization/rabitq.h"
#include <fstream>
#include <span>
namespace qcovert {
template <std::size_t D, class Scalar>
typename layout<D, Scalar>::dataset
prepare_dataset(std::span<const std::array<Scalar, D>> vectors, std::span<const std::uint64_t> labels,
                std::span<const std::array<float, D>> centroids, std::size_t tau, std::size_t public_nc,
                std::uint32_t seed = 42) {
  using L = layout<D, Scalar>;
  if (vectors.empty() || labels.size() != vectors.size() || centroids.empty() ||
      public_nc < centroids.size() || !tau)
    throw std::invalid_argument("invalid offline dataset shape");
  typename L::dataset out;
  out.tau = tau;
  out.entries.resize(vectors.size());
  out.centroids.resize(public_nc);
  out.rotation.resize(D * L::padded);
  ivf_rabitq::RabitQ quantizer(D, 4, seed);
  const auto &matrix = quantizer.rotation_matrix();
  for (std::size_t d = 0; d < D; ++d)
    for (std::size_t j = 0; j < L::padded; ++j)
      out.rotation[d * L::padded + j] = matrix(d, j);
  std::copy(quantizer.query_offsets().begin(), quantizer.query_offsets().end(), out.offsets.begin());
  std::vector<std::size_t> counts(centroids.size());
  for (std::size_t i = 0; i < centroids.size(); ++i) {
    out.centroids[i].values = centroids[i];
    out.centroids[i].valid = 1;
    auto r = quantizer.prepare(centroids[i].data());
    std::copy(r.values.begin(), r.values.end(), out.centroids[i].rotated.begin());
  }
  for (std::size_t i = 0; i < vectors.size(); ++i) {
    auto cid = labels[i];
    if (cid >= centroids.size() || ++counts[cid] > tau)
      throw std::invalid_argument("bad cluster label/capacity");
    std::array<float, D> fp{};
    for (std::size_t j = 0; j < D; ++j)
      fp[j] = float(vectors[i][j]);
    auto code = quantizer.quantize_vector(fp.data(), centroids[cid].data());
    auto &e = out.entries[i];
    e.cid = cid;
    e.values = vectors[i];
    e.quantized.norm = code.norm;
    e.quantized.x0 = code.x0;
    std::copy(code.signs.begin(), code.signs.end(), e.quantized.signs.begin());
  }
  for (std::size_t i = 0; i < counts.size(); ++i)
    out.centroids[i].valid = counts[i] != 0;
  return out;
}
inline void save_file(const std::string &path, std::span<const std::uint8_t> bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size()))
    throw std::runtime_error("cannot save dataset");
}
inline std::vector<std::uint8_t> load_file(const std::string &path, std::size_t max_bytes) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in)
    throw std::runtime_error("cannot open dataset");
  const auto length = in.tellg();
  if (length < 0 || static_cast<std::uint64_t>(length) > max_bytes)
    throw std::length_error("dataset file too large");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
  in.seekg(0);
  if (!in.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
    throw std::runtime_error("cannot read dataset");
  return bytes;
}
} // namespace qcovert
