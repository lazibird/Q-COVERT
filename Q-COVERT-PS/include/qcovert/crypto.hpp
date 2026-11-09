#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace qcovert {
using key128 = std::array<std::uint8_t, 16>;
using nonce96 = std::array<std::uint8_t, 12>;
void encrypt(const key128 &, const nonce96 &, const void *, std::size_t, void *, std::uint8_t *tag);
void decrypt(const key128 &, const nonce96 &, const void *, std::size_t, const std::uint8_t *tag, void *);
std::uint64_t prf(const key128 &, std::uint64_t cid, std::uint64_t counter, std::uint64_t offset);
void *external_allocate(std::size_t);
void external_free(void *) noexcept;
void external_read(void *trusted, const void *external, std::size_t);
void external_write(void *external, const void *trusted, std::size_t);
} // namespace qcovert
