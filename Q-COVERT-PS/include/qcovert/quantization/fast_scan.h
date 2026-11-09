#pragma once

// Batched 1-bit RaBitQ distance scan. AVX2 uses the original 32-vector
// nibble-packed layout and lookup-table accumulation; other targets fall back
// to the same scalar distance formula. Query coordinates must use <= 4 bits.
#include "rabitq.h"
#include <array>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#endif

namespace ivf_rabitq {
namespace detail {

#if defined(__AVX2__)
template <std::size_t B>
inline std::vector<std::uint8_t> pack_binary_codes(const std::vector<BinaryCode>& codes) {
    constexpr std::size_t M = B / 4;
    constexpr std::uint8_t perm[16] = {0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15};
    const std::size_t padded_count = (codes.size() + 31) / 32 * 32;
    std::vector<std::uint8_t> bytes(padded_count * B / 8, 0);
    for (std::size_t i = 0; i < codes.size(); ++i) {
        std::memcpy(bytes.data() + i * B / 8, codes[i].signs.data(), B / 8);
    }

    std::vector<std::uint8_t> packed(padded_count * B / 8, 0);
    auto* out = packed.data();
    for (std::size_t block = 0; block < padded_count; block += 32) {
        for (std::size_t m = 0; m < M; m += 2) {
            std::array<std::uint8_t, 32> c{};
            for (std::size_t j = 0; j < 32; ++j)
                c[j] = bytes[(block + j) * B / 8 + m / 2];
            for (std::size_t j = 0; j < 16; ++j) {
                out[j] = (c[perm[j]] & 15) | ((c[perm[j] + 16] & 15) << 4);
                out[j + 16] = (c[perm[j]] >> 4) | ((c[perm[j] + 16] >> 4) << 4);
            }
            out += 32;
        }
    }
    return packed;
}

template <std::size_t B>
inline void accumulate_block(const std::uint8_t* packed, const std::uint8_t* lut,
                             std::uint16_t* result) {
    const __m256i mask = _mm256_set1_epi8(15);
    __m256i accum[4] = {_mm256_setzero_si256(), _mm256_setzero_si256(),
                        _mm256_setzero_si256(), _mm256_setzero_si256()};
    for (std::size_t m = 0; m < B / 4; m += 2) {
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(packed));
        const __m256i low = _mm256_and_si256(c, mask);
        const __m256i high = _mm256_and_si256(_mm256_srli_epi16(c, 4), mask);
        const __m256i table = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lut));
        const __m256i lo_result = _mm256_shuffle_epi8(table, low);
        const __m256i hi_result = _mm256_shuffle_epi8(table, high);
        accum[0] = _mm256_add_epi16(accum[0], lo_result);
        accum[1] = _mm256_add_epi16(accum[1], _mm256_srli_epi16(lo_result, 8));
        accum[2] = _mm256_add_epi16(accum[2], hi_result);
        accum[3] = _mm256_add_epi16(accum[3], _mm256_srli_epi16(hi_result, 8));
        packed += 32;
        lut += 32;
    }
    accum[0] = _mm256_sub_epi16(accum[0], _mm256_slli_epi16(accum[1], 8));
    const __m256i lo = _mm256_add_epi16(
        _mm256_permute2f128_si256(accum[0], accum[1], 0x21),
        _mm256_blend_epi32(accum[0], accum[1], 0xF0));
    _mm256_store_si256(reinterpret_cast<__m256i*>(result), lo);
    accum[2] = _mm256_sub_epi16(accum[2], _mm256_slli_epi16(accum[3], 8));
    const __m256i hi = _mm256_add_epi16(
        _mm256_permute2f128_si256(accum[2], accum[3], 0x21),
        _mm256_blend_epi32(accum[2], accum[3], 0xF0));
    _mm256_store_si256(reinterpret_cast<__m256i*>(result + 16), hi);
}
#endif

}  // namespace detail

template <std::size_t B>
class FastScan {
    static_assert(B >= 64 && B % 64 == 0 && B <= 2048,
                  "FastScan requires padded dimension in [64, 2048], multiple of 64");

public:
    explicit FastScan(const std::vector<BinaryCode>& codes) : count_(codes.size()) {
        norms_.reserve(count_);
        inverse_cosines_.reserve(count_);
        sign_sums_.reserve(count_);
        for (const auto& code : codes) {
            if (code.signs.size() != B / 64 || (code.norm > 0 && !(code.x0 > 0)))
                throw std::invalid_argument("invalid binary code for FastScan");
            norms_.push_back(code.norm);
            inverse_cosines_.push_back(code.norm ? 1.0f / code.x0 : 0.0f);
            int sum = 0;
            for (std::size_t i = 0; i < B; ++i) sum += detail::bit(code.signs, i) ? 1 : -1;
            sign_sums_.push_back(sum);
        }
#if defined(__AVX2__)
        packed_ = detail::pack_binary_codes<B>(codes);
#else
        codes_ = codes;
#endif
    }

    std::size_t size() const { return count_; }

    void scan(const BinaryQuery& query, std::vector<float>& distances) const {
        if (query.codes.size() != B) throw std::invalid_argument("query dimension mismatch");
        if (std::any_of(query.codes.begin(), query.codes.end(), [](std::uint8_t c) { return c > 15; }))
            throw std::invalid_argument("FastScan needs query_bits <= 4");
        distances.resize(count_);
        const float scale = 2.0f / std::sqrt(float(B));
#if defined(__AVX2__)
        const int sum_q = std::accumulate(query.codes.begin(), query.codes.end(), 0);
        std::vector<std::uint8_t> lut(B / 4 * 16);
        for (std::size_t m = 0; m < B / 4; ++m) {
            lut[m * 16] = 0;
            for (unsigned v = 1; v < 16; ++v) {
                const unsigned bit = static_cast<unsigned>(__builtin_ctz(v));
                lut[m * 16 + v] = static_cast<std::uint8_t>(
                    lut[m * 16 + (v & (v - 1))] + 2 * query.codes[m * 4 + bit]);
            }
        }
        for (std::size_t block = 0; block < count_; block += 32) {
            alignas(32) std::uint16_t sums[32];
            detail::accumulate_block<B>(packed_.data() + block * B / 8, lut.data(), sums);
            for (std::size_t j = 0; j < std::min<std::size_t>(32, count_ - block); ++j) {
                const std::size_t i = block + j;
                distances[i] = norms_[i] ?
                    norms_[i] * norms_[i] + query.norm2 -
                        scale * norms_[i] * inverse_cosines_[i] *
                            (sign_sums_[i] * query.lo + (int(sums[j]) - sum_q) * query.width)
                    : query.norm2;
            }
        }
#else
        for (std::size_t i = 0; i < count_; ++i) {
            if (!norms_[i]) { distances[i] = query.norm2; continue; }
            int signed_codes = 0;
            for (std::size_t j = 0; j < B; ++j)
                signed_codes += (detail::bit(codes_[i].signs, j) ? 1 : -1) * int(query.codes[j]);
            distances[i] = norms_[i] * norms_[i] + query.norm2 -
                scale * norms_[i] * inverse_cosines_[i] *
                    (sign_sums_[i] * query.lo + signed_codes * query.width);
        }
#endif
    }

    std::vector<float> scan(const BinaryQuery& query) const {
        std::vector<float> distances;
        scan(query, distances);
        return distances;
    }

private:
    std::size_t count_;
    std::vector<float> norms_;
    std::vector<float> inverse_cosines_;
    std::vector<int> sign_sums_;
#if defined(__AVX2__)
    std::vector<std::uint8_t> packed_;
#else
    std::vector<BinaryCode> codes_;
#endif
};

}  // namespace ivf_rabitq
