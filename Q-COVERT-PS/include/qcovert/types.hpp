#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>
namespace qcovert {
inline constexpr std::uint64_t invalid_id = std::numeric_limits<std::uint64_t>::max();
inline constexpr double infinity = std::numeric_limits<double>::infinity();
template <std::size_t D, class Scalar> struct layout {
  static_assert(D > 0 && D <= 65536 &&
                (std::is_same_v<Scalar, float> || std::is_same_v<Scalar, std::uint8_t>));
  static constexpr std::size_t dim = D, padded = (D + 63) / 64 * 64;
  using vector = std::array<Scalar, D>;
  struct code {
    std::array<std::uint64_t, padded / 64> signs{};
    float norm = 0, x0 = 0;
  };
  struct entry {
    std::uint64_t cid = 0;
    vector values{};
    code quantized{};
  };
  struct centroid {
    std::array<float, D> values{};
    std::array<float, padded> rotated{};
    std::uint64_t valid = 0;
  };
  struct dataset {
    std::uint64_t tau = 0;
    std::vector<entry> entries;
    std::vector<centroid> centroids;
    // Heap-backed: D*padded floats (2+ MiB at D=768) would overflow the
    // 2 MiB enclave stack if embedded in the struct.
    std::vector<float> rotation;
    std::array<float, padded> offsets{};
  };
  struct alignas(8) full_block {
    vector values{};
  };
  struct alignas(8) quantized_entry {
    std::uint64_t id = invalid_id;
    std::int64_t leaf = 0;
    code quantized{};
  };
};
struct query_options {
  std::size_t nprobe = 1, k2 = 1, k = 1;
  // Public, fixed for an index instance. An empty vector selects exact top-k2.
  // Otherwise rank_partitions[r] representatives are selected from probed cluster r.
  std::vector<std::size_t> rank_partitions;
  query_options() = default;
  query_options(std::size_t probes, std::size_t candidates, std::size_t final_k,
                std::vector<std::size_t> partitions = {})
      : nprobe(probes), k2(candidates), k(final_k), rank_partitions(std::move(partitions)) {}
};
struct result {
  std::uint64_t id = invalid_id;
  double distance = infinity;
};
struct query_stats {
  std::size_t quantized_reads = 0, full_reads = 0, quantized_candidates = 0;
};
template <class T> auto bytes(const T &value) {
  std::array<std::uint8_t, sizeof(T)> out{};
  std::memcpy(out.data(), &value, sizeof(T));
  return out;
}
template <class T, class Buffer> T from_bytes(const Buffer &data) {
  static_assert(std::is_trivially_copyable_v<T>);
  T out{};
  std::memcpy(&out, data.data(), sizeof(T));
  return out;
}
} // namespace qcovert
