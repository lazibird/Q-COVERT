#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

#include "types.h"

#define PORTABLE_ALIGN32 __attribute__((aligned(32)))
#define PORTABLE_ALIGN64 __attribute__((aligned(64)))

template <uint32_t D, uint32_t B, uint32_t QueryBits = 4>
class Space {
public:
    static_assert(B % 64 == 0, "B must be a multiple of 64");
    static_assert(QueryBits > 0 && QueryBits <= 8, "QueryBits must be in [1, 8]");

    inline static uint32_t popcount(const uint64_t* d) {
        uint32_t ret = 0;
        for (uint32_t i = 0; i < B / 64; ++i) {
            ret += static_cast<uint32_t>(__builtin_popcountll(d[i]));
        }
        return ret;
    }

    inline static uint32_t ip_bin_bin(const uint64_t* q, const uint64_t* d) {
        uint32_t ret = 0;
        for (uint32_t i = 0; i < B / 64; ++i) {
            ret += static_cast<uint32_t>(__builtin_popcountll(q[i] & d[i]));
        }
        return ret;
    }

    inline static uint32_t ip_byte_bin(const uint64_t* q, const uint64_t* d) {
        uint32_t ret = 0;
        for (uint32_t i = 0; i < QueryBits; ++i) {
            ret += (ip_bin_bin(q, d) << i);
            q += B / 64;
        }
        return ret;
    }

    inline static void transpose_bin(const uint8_t* q, uint64_t* tq) {
        static_assert(QueryBits <= 8, "QueryBits must fit in uint8_t");
        std::memset(tq, 0, QueryBits * B / 64 * sizeof(uint64_t));
        for (uint32_t i = 0; i < B; i += 32) {
            __m256i v = _mm256_load_si256(reinterpret_cast<const __m256i*>(q));
            v = _mm256_slli_epi32(v, 8 - QueryBits);
            for (uint32_t j = 0; j < QueryBits; ++j) {
                uint32_t v1 = static_cast<uint32_t>(_mm256_movemask_epi8(v));
                v1 = reverse_bits(v1);
                tq[(QueryBits - j - 1) * (B / 64) + i / 64] |=
                    (static_cast<uint64_t>(v1) << (((i / 32) % 2 == 0) ? 32 : 0));
                v = _mm256_slli_epi32(v, 1);
            }
            q += 32;
        }
    }

    inline static void range(const float* q, const float* c, float& vl, float& vr) {
        vl = std::numeric_limits<float>::max();
        vr = -std::numeric_limits<float>::max();
        for (uint32_t i = 0; i < B; ++i) {
            const float tmp = q[i] - c[i];
            vl = std::min(vl, tmp);
            vr = std::max(vr, tmp);
        }
    }

    inline static void quantize(uint8_t* result,
                                const float* q,
                                const float* c,
                                const float* u,
                                float vl,
                                float width,
                                uint32_t& sum_q) {
        sum_q = 0;
        if (!(width > 0.0f) || !std::isfinite(width)) {
            std::memset(result, 0, B);
            return;
        }
        const float one_over_width = 1.0f / width;
        const uint32_t max_code = (1u << QueryBits) - 1u;
        for (uint32_t i = 0; i < B; ++i) {
            int code = static_cast<int>(((q[i] - c[i]) - vl) * one_over_width + u[i]);
            code = std::max(0, std::min<int>(static_cast<int>(max_code), code));
            result[i] = static_cast<uint8_t>(code);
            sum_q += static_cast<uint32_t>(code);
        }
    }
};

template <uint32_t L>
inline float sqr_dist(const float* d, const float* q) {
    float PORTABLE_ALIGN32 tmp_res[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    constexpr uint32_t num_blk16 = L >> 4;
    constexpr uint32_t tail = L & 0b1111u;

    __m256 sum = _mm256_set1_ps(0);
    for (uint32_t i = 0; i < num_blk16; ++i) {
        __m256 v1 = _mm256_loadu_ps(d);
        __m256 v2 = _mm256_loadu_ps(q);
        d += 8;
        q += 8;
        __m256 diff = _mm256_sub_ps(v1, v2);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(diff, diff));

        v1 = _mm256_loadu_ps(d);
        v2 = _mm256_loadu_ps(q);
        d += 8;
        q += 8;
        diff = _mm256_sub_ps(v1, v2);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(diff, diff));
    }
    for (uint32_t i = 0; i < tail / 8; ++i) {
        const __m256 v1 = _mm256_loadu_ps(d);
        const __m256 v2 = _mm256_loadu_ps(q);
        d += 8;
        q += 8;
        const __m256 diff = _mm256_sub_ps(v1, v2);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(diff, diff));
    }
    _mm256_store_ps(tmp_res, sum);

    float ret = tmp_res[0] + tmp_res[1] + tmp_res[2] + tmp_res[3] +
                tmp_res[4] + tmp_res[5] + tmp_res[6] + tmp_res[7];

    for (uint32_t i = 0; i < tail % 8; ++i) {
        const float diff = q[i] - d[i];
        ret += diff * diff;
    }
    return ret;
}

