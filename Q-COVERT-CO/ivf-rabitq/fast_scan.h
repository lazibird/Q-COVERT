#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <vector>

#include "types.h"

#ifndef lowbit
#define lowbit(x) ((x) & (-(x)))
#endif

template <uint32_t B>
inline void accumulate_one_block(const uint8_t* codes, const uint8_t* lut, uint16_t* result) {
    static_assert(B % 64 == 0, "B must be a multiple of 64");
    const __m256i low_mask = _mm256_set1_epi8(0xf);
    __m256i accu[4];
    for (int i = 0; i < 4; ++i) {
        accu[i] = _mm256_setzero_si256();
    }

    constexpr uint32_t M = B / 4;
    for (uint32_t m = 0; m < M; m += 2) {
        const __m256i c = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(codes));
        const __m256i lo = _mm256_and_si256(c, low_mask);
        const __m256i hi = _mm256_and_si256(_mm256_srli_epi16(c, 4), low_mask);
        const __m256i table = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(lut));

        const __m256i res_lo = _mm256_shuffle_epi8(table, lo);
        const __m256i res_hi = _mm256_shuffle_epi8(table, hi);

        accu[0] = _mm256_add_epi16(accu[0], res_lo);
        accu[1] = _mm256_add_epi16(accu[1], _mm256_srli_epi16(res_lo, 8));
        accu[2] = _mm256_add_epi16(accu[2], res_hi);
        accu[3] = _mm256_add_epi16(accu[3], _mm256_srli_epi16(res_hi, 8));

        codes += 32;
        lut += 32;
    }

    accu[0] = _mm256_sub_epi16(accu[0], _mm256_slli_epi16(accu[1], 8));
    const __m256i dis0 =
        _mm256_add_epi16(_mm256_permute2f128_si256(accu[0], accu[1], 0x21),
                         _mm256_blend_epi32(accu[0], accu[1], 0xF0));
    _mm256_store_si256(reinterpret_cast<__m256i*>(result), dis0);

    accu[2] = _mm256_sub_epi16(accu[2], _mm256_slli_epi16(accu[3], 8));
    const __m256i dis1 =
        _mm256_add_epi16(_mm256_permute2f128_si256(accu[2], accu[3], 0x21),
                         _mm256_blend_epi32(accu[2], accu[3], 0xF0));
    _mm256_store_si256(reinterpret_cast<__m256i*>(result + 16), dis1);
}

template <uint32_t B>
inline void accumulate(uint32_t nblk, const uint8_t* codes, const uint8_t* lut, uint16_t* result) {
    for (uint32_t i = 0; i < nblk; ++i) {
        accumulate_one_block<B>(codes, lut, result);
        codes += 32 * B / 8;
        result += 32;
    }
}

template <uint32_t B>
inline void pack_lut_4bit(const uint8_t* byte_query, uint8_t* lut) {
    constexpr uint32_t M = B / 4;
    constexpr uint32_t pos[16] = {
        3, 3, 2, 3,
        1, 3, 2, 3,
        0, 3, 2, 3,
        1, 3, 2, 3,
    };
    for (uint32_t i = 0; i < M; ++i) {
        lut[0] = 0;
        for (uint32_t j = 1; j < 16; ++j) {
            lut[j] = lut[j - lowbit(j)] + byte_query[pos[j]];
        }
        lut += 16;
        byte_query += 4;
    }
}

template <typename T, class ArrayLike>
inline void get_matrix_column(const T* src, size_t m, size_t n, int64_t i, int64_t j, ArrayLike& dest) {
    for (int64_t k = 0; k < static_cast<int64_t>(dest.size()); ++k) {
        if (k + i >= 0 && k + i < static_cast<int64_t>(m)) {
            dest[static_cast<size_t>(k)] = src[static_cast<size_t>(k + i) * n + static_cast<size_t>(j)];
        } else {
            dest[static_cast<size_t>(k)] = 0;
        }
    }
}

template <uint32_t B>
void pack_codes_bytes(const uint8_t* codes, uint32_t ncode, uint8_t* blocks) {
    constexpr uint32_t M = B / 4;
    constexpr uint8_t batch = 32;
    const uint32_t ncode_pad = (ncode + batch - 1) / batch * batch;
    std::memset(blocks, 0, static_cast<size_t>(ncode_pad) * M / 2);

    constexpr uint8_t perm0[16] = {
        0, 8, 1, 9, 2, 10, 3, 11,
        4, 12, 5, 13, 6, 14, 7, 15,
    };

    uint8_t* out = blocks;
    for (uint32_t blk = 0; blk < ncode_pad; blk += batch) {
        for (uint32_t m = 0; m < M; m += 2) {
            std::array<uint8_t, 32> c{};
            std::array<uint8_t, 32> c0{};
            std::array<uint8_t, 32> c1{};
            get_matrix_column(codes, ncode, M / 2, blk, m / 2, c);
            for (uint32_t j = 0; j < 32; ++j) {
                c0[j] = c[j] & 15;
                c1[j] = c[j] >> 4;
            }
            for (uint32_t j = 0; j < 16; ++j) {
                out[j] = c0[perm0[j]] | (c0[perm0[j] + 16] << 4);
                out[j + 16] = c1[perm0[j]] | (c1[perm0[j] + 16] << 4);
            }
            out += 32;
        }
    }
}

template <uint32_t B>
void pack_codes_binary(const uint64_t* binary_code, uint32_t ncode, uint8_t* blocks) {
    static_assert(B % 64 == 0, "B must be a multiple of 64");
    const uint32_t ncode_pad = (ncode + 31) / 32 * 32;
    std::vector<uint8_t> binary_code_8bit(static_cast<size_t>(ncode_pad) * B / 8, 0);
    std::memcpy(binary_code_8bit.data(), binary_code, static_cast<size_t>(ncode) * B / 64 * sizeof(uint64_t));

    for (uint32_t i = 0; i < ncode; ++i) {
        for (uint32_t j = 0; j < B / 64; ++j) {
            for (uint32_t k = 0; k < 4; ++k) {
                std::swap(binary_code_8bit[static_cast<size_t>(i) * B / 8 + 8 * j + k],
                          binary_code_8bit[static_cast<size_t>(i) * B / 8 + 8 * j + 7 - k]);
            }
        }
    }

    for (uint32_t i = 0; i < ncode * B / 8; ++i) {
        const uint8_t v = binary_code_8bit[i];
        binary_code_8bit[i] = static_cast<uint8_t>((v << 4) | (v >> 4));
    }

    pack_codes_bytes<B>(binary_code_8bit.data(), ncode, blocks);
}
