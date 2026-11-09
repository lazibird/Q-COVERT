#include "sonic/crypto/impl/openssl/detail/utils.hpp"

#include <array>
#include <sstream>

namespace sn::crypto::detail {

evp_cipher_ctx::evp_cipher_ctx() {
  ctx_ = EVP_CIPHER_CTX_new();
  if (ctx_ == nullptr) {
    throw_openssl_error("evp_cipher_ctx_new");
  }
}

evp_cipher_ctx::~evp_cipher_ctx() {
  if (ctx_ != nullptr) {
    EVP_CIPHER_CTX_free(ctx_);
    ctx_ = nullptr;
  }
}

evp_cipher_ctx::evp_cipher_ctx(evp_cipher_ctx&& other) noexcept : ctx_(other.ctx_) { other.ctx_ = nullptr; }

evp_cipher_ctx& evp_cipher_ctx::operator=(evp_cipher_ctx&& other) noexcept {
  if (this != &other) {
    if (ctx_ != nullptr) {
      EVP_CIPHER_CTX_free(ctx_);
    }
    ctx_ = other.ctx_;
    other.ctx_ = nullptr;
  }
  return *this;
}

void evp_cipher_ctx::reset() {
  if (ctx_ != nullptr) {
    EVP_CIPHER_CTX_free(ctx_);
  }
  ctx_ = EVP_CIPHER_CTX_new();
  if (ctx_ == nullptr) {
    throw_openssl_error("evp_cipher_ctx_reset");
  }
}

evp_md_ctx::evp_md_ctx() {
  ctx_ = EVP_MD_CTX_new();
  if (ctx_ == nullptr) {
    throw_openssl_error("evp_md_ctx_new");
  }
}

evp_md_ctx::~evp_md_ctx() {
  if (ctx_ != nullptr) {
    EVP_MD_CTX_free(ctx_);
    ctx_ = nullptr;
  }
}

evp_md_ctx::evp_md_ctx(evp_md_ctx&& other) noexcept : ctx_(other.ctx_) { other.ctx_ = nullptr; }

evp_md_ctx& evp_md_ctx::operator=(evp_md_ctx&& other) noexcept {
  if (this != &other) {
    if (ctx_ != nullptr) {
      EVP_MD_CTX_free(ctx_);
    }
    ctx_ = other.ctx_;
    other.ctx_ = nullptr;
  }
  return *this;
}

void evp_md_ctx::reset() {
  if (ctx_ != nullptr) {
    EVP_MD_CTX_free(ctx_);
  }
  ctx_ = EVP_MD_CTX_new();
  if (ctx_ == nullptr) {
    throw_openssl_error("evp_md_ctx_reset");
  }
}

[[noreturn]] void throw_openssl_error(const char* context) {
  unsigned long err = ERR_get_error();
  std::array<char, 256> buffer{};
  ERR_error_string_n(err, buffer.data(), buffer.size());

  std::ostringstream oss;
  oss << context << ": " << buffer.data();
  throw error(oss.str());
}

}
