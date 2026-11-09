#pragma once
#include "qcovert/types.hpp"
#include "ringdoram/obliv.hpp"
#include <cmath>
namespace qcovert {
template <class Layout> struct quantized_query {
  std::array<float, Layout::padded> codes{};
  float lo = 0, width = 0, norm2 = 0;
};
template <class Layout>
quantized_query<Layout> prepare_query(const std::array<float, Layout::padded> &rotated,
                                      const typename Layout::centroid &c,
                                      const std::array<float, Layout::padded> &offsets) {
  using ringdoram::obliv::OSelect;
  quantized_query<Layout> out;
  std::array<float, Layout::padded> residual{};
  float lo = std::numeric_limits<float>::infinity(), hi = -lo;
  for (std::size_t i = 0; i < Layout::padded; ++i) {
    const float x = rotated[i] - c.rotated[i];
    residual[i] = x;
    out.norm2 += x * x;
    lo = OSelect(x, lo, x < lo);
    hi = OSelect(x, hi, x > hi);
  }
  out.lo = lo;
  out.width = (hi - lo) / 15.0f; // 1-bit data, 4-bit query quantization
  const float denominator = OSelect(out.width, 1.0f, out.width > 0);
  for (std::size_t i = 0; i < Layout::padded; ++i) {
    float x = (residual[i] - lo) / denominator + offsets[i];
    x = OSelect(0.0f, x, x < 0);
    x = OSelect(15.0f, x, x > 15);
    // Nonnegative bounded conversion truncates without a secret-dependent floor implementation.
    out.codes[i] = OSelect(static_cast<float>(static_cast<std::uint32_t>(x)), 0.0f, out.width > 0);
  }
  return out;
}
template <class Layout> float estimate(const quantized_query<Layout> &q, const typename Layout::code &c) {
  using ringdoram::obliv::OSelect;
  float sign_sum = 0, code_sum = 0;
  for (std::size_t i = 0; i < Layout::padded; ++i) {
    const float sign = 2.0f * static_cast<float>((c.signs[i / 64] >> (i % 64)) & 1) - 1.0f;
    sign_sum += sign;
    code_sum += sign * q.codes[i];
  }
  const float dot = (q.lo * sign_sum + q.width * code_sum) / std::sqrt(float(Layout::padded));
  const float score = c.norm * c.norm + q.norm2 - 2 * c.norm * dot / OSelect(c.x0, 1.0f, c.x0 > 0);
  return OSelect(q.norm2, score, c.norm == 0);
}
} // namespace qcovert
