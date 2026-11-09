#pragma once

#include <cstddef>
#include <cstdint>

#include "sonic/obliv/ops/word_ops.hpp"

namespace sn::obliv {
namespace detail {

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)

inline __attribute__((always_inline)) void ct_rw_word_u64_mask(
    std::uint8_t* item_ptr, std::uint8_t* block_ptr, bool read_cond, bool write_cond
) noexcept {
  std::uint64_t item_val;
  std::uint64_t block_val;
  std::uint64_t item_orig;

  asm volatile("mov %[item_val], qword ptr [%[item_ptr]]\n\t"
               "mov %[block_val], qword ptr [%[block_ptr]]\n\t"
               "mov %[item_orig], %[item_val]\n\t"
               "test %[write_cond], %[write_cond]\n\t"
               "cmovnz %[item_val], %[block_val]\n\t"
               "test %[read_cond], %[read_cond]\n\t"
               "cmovnz %[block_val], %[item_orig]\n\t"
               "mov qword ptr [%[item_ptr]], %[item_val]\n\t"
               "mov qword ptr [%[block_ptr]], %[block_val]\n\t"
               : [item_val] "=&r"(item_val), [block_val] "=&r"(block_val), [item_orig] "=&r"(item_orig)
               : [item_ptr] "r"(item_ptr), [block_ptr] "r"(block_ptr),
                 [write_cond] "r"(static_cast<unsigned>(write_cond)), [read_cond] "r"(static_cast<unsigned>(read_cond))
               : "cc", "memory");
}

inline __attribute__((always_inline)) void ct_rw_word_u128_mask(
    std::uint8_t* item_ptr, std::uint8_t* block_ptr, bool read_cond, bool write_cond
) noexcept {
  asm volatile("mov rax, qword ptr [%[item_ptr]]\n\t"
               "mov rbx, qword ptr [%[item_ptr] + 8]\n\t"
               "mov rcx, qword ptr [%[block_ptr]]\n\t"
               "mov rdx, qword ptr [%[block_ptr] + 8]\n\t"
               "mov r8, rax\n\t"
               "mov r9, rbx\n\t"
               "test %[write_cond], %[write_cond]\n\t"
               "cmovnz rax, rcx\n\t"
               "cmovnz rbx, rdx\n\t"
               "test %[read_cond], %[read_cond]\n\t"
               "cmovnz rcx, r8\n\t"
               "cmovnz rdx, r9\n\t"
               "mov qword ptr [%[item_ptr]], rax\n\t"
               "mov qword ptr [%[item_ptr] + 8], rbx\n\t"
               "mov qword ptr [%[block_ptr]], rcx\n\t"
               "mov qword ptr [%[block_ptr] + 8], rdx\n\t"
               :
               : [item_ptr] "r"(item_ptr), [block_ptr] "r"(block_ptr),
                 [write_cond] "r"(static_cast<unsigned>(write_cond)), [read_cond] "r"(static_cast<unsigned>(read_cond))
               : "cc", "memory", "rax", "rbx", "rcx", "rdx", "r8", "r9");
}

#endif

}

template <std::size_t ByteCount>
inline __attribute__((always_inline)) void ct_rw_words_fallback(
    std::uint8_t* item_aligned, std::uint8_t* block_aligned, bool read_cond, bool write_cond
) {
  ct_set_words<ByteCount>(item_aligned, block_aligned, write_cond);
  ct_set_words<ByteCount>(block_aligned, item_aligned, read_cond);
}

template <std::size_t ByteCount>
inline void ct_rw_words_aligned(std::uint8_t* item_bytes, std::uint8_t* block_bytes, bool read_cond, bool write_cond) {
  (void) sizeof(detail::word_buffer_traits<ByteCount>);
  auto* item_aligned = detail::word_alignment_guard::assume(item_bytes);
  auto* block_aligned = detail::word_alignment_guard::assume(block_bytes);
  if constexpr (ByteCount == 8) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    detail::ct_rw_word_u64_mask(
        reinterpret_cast<std::uint8_t*>(item_aligned), reinterpret_cast<std::uint8_t*>(block_aligned), read_cond,
        write_cond
    );
#else
    ct_rw_words_fallback<ByteCount>(
        reinterpret_cast<std::uint8_t*>(item_aligned), reinterpret_cast<std::uint8_t*>(block_aligned), read_cond,
        write_cond
    );
#endif
  } else if constexpr (ByteCount == 16) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    detail::ct_rw_word_u128_mask(
        reinterpret_cast<std::uint8_t*>(item_aligned), reinterpret_cast<std::uint8_t*>(block_aligned), read_cond,
        write_cond
    );
#else
    ct_rw_words_fallback<ByteCount>(
        reinterpret_cast<std::uint8_t*>(item_aligned), reinterpret_cast<std::uint8_t*>(block_aligned), read_cond,
        write_cond
    );
#endif
  } else {
    ct_rw_words_fallback<ByteCount>(
        reinterpret_cast<std::uint8_t*>(item_aligned), reinterpret_cast<std::uint8_t*>(block_aligned), read_cond,
        write_cond
    );
  }
}

}
