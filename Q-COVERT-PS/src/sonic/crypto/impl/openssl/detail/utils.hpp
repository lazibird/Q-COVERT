#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include <openssl/err.h>
#include <openssl/evp.h>

#include "sonic/crypto/error.hpp"

namespace sn::crypto::detail {

class evp_cipher_ctx {
public:
  evp_cipher_ctx();
  ~evp_cipher_ctx();

  evp_cipher_ctx(const evp_cipher_ctx&) = delete;
  evp_cipher_ctx& operator=(const evp_cipher_ctx&) = delete;

  evp_cipher_ctx(evp_cipher_ctx&& other) noexcept;
  evp_cipher_ctx& operator=(evp_cipher_ctx&& other) noexcept;

  EVP_CIPHER_CTX* get() noexcept { return ctx_; }
  const EVP_CIPHER_CTX* get() const noexcept { return ctx_; }

  void reset();

private:
  EVP_CIPHER_CTX* ctx_ = nullptr;
};

class evp_md_ctx {
public:
  evp_md_ctx();
  ~evp_md_ctx();

  evp_md_ctx(const evp_md_ctx&) = delete;
  evp_md_ctx& operator=(const evp_md_ctx&) = delete;

  evp_md_ctx(evp_md_ctx&& other) noexcept;
  evp_md_ctx& operator=(evp_md_ctx&& other) noexcept;

  EVP_MD_CTX* get() noexcept { return ctx_; }
  const EVP_MD_CTX* get() const noexcept { return ctx_; }

  void reset();

private:
  EVP_MD_CTX* ctx_ = nullptr;
};

[[noreturn]] void throw_openssl_error(const char* context);

inline void check_evp(int rc, const char* context) {
  if (rc != 1) {
    throw_openssl_error(context);
  }
}

inline void check_md(int rc, const char* context) {
  if (rc != 1) {
    throw_openssl_error(context);
  }
}

inline void secure_zero(void* ptr, std::size_t len) {
#if defined(SONIC_CRYPTO_TEE)
  (void) ptr;
  (void) len;
#else
  if (len == 0) {
    return;
  }
  OPENSSL_cleanse(ptr, len);
#endif
}

template <typename T> inline T load_trivial(const std::uint8_t* src) {
  static_assert(std::is_trivially_copyable<T>::value, "load_trivial requires trivially copyable type");
  T value;
  std::memcpy(&value, src, sizeof(T));
  return value;
}

template <typename T> inline void store_trivial(std::uint8_t* dst, const T& value) {
  static_assert(std::is_trivially_copyable<T>::value, "store_trivial requires trivially copyable type");
  std::memcpy(dst, &value, sizeof(T));
}

constexpr std::size_t max_chunk_size = 1u << 20;

}
