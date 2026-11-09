#include "qcovert/crypto.hpp"
#include <cstring>
#include <limits>
#include <stdexcept>
#ifdef SN_SGX_ENCLAVE
#include "ringdoram_sgx_t.h"
#include <sgx_lfence.h>
#include <sgx_tcrypto.h>
#include <sgx_trts.h>
#else
#include <cstdlib>
#include <openssl/core_names.h>
#include <openssl/evp.h>
#endif
namespace qcovert {
namespace {
void require(bool ok) {
  if (!ok)
    throw std::runtime_error("Q-COVERT cryptographic/storage failure");
}
void check_size(std::size_t n) { require(n <= static_cast<std::size_t>(std::numeric_limits<int>::max())); }
} // namespace
void encrypt(const key128 &key, const nonce96 &iv, const void *src, std::size_t n, void *dst,
             std::uint8_t *tag) {
  check_size(n);
#ifdef SN_SGX_ENCLAVE
  require(sgx_rijndael128GCM_encrypt(reinterpret_cast<const sgx_aes_gcm_128bit_key_t *>(key.data()),
                                     static_cast<const std::uint8_t *>(src), n,
                                     static_cast<std::uint8_t *>(dst), iv.data(), iv.size(), nullptr, 0,
                                     reinterpret_cast<sgx_aes_gcm_128bit_tag_t *>(tag)) == SGX_SUCCESS);
#else
  auto *ctx = EVP_CIPHER_CTX_new();
  require(ctx != nullptr);
  int out = 0, tail = 0;
  bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), iv.data()) == 1;
  ok &= EVP_EncryptUpdate(ctx, static_cast<unsigned char *>(dst), &out,
                          static_cast<const unsigned char *>(src), n) == 1;
  ok &= EVP_EncryptFinal_ex(ctx, static_cast<unsigned char *>(dst) + out, &tail) == 1;
  ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
  EVP_CIPHER_CTX_free(ctx);
  require(ok);
#endif
}
void decrypt(const key128 &key, const nonce96 &iv, const void *src, std::size_t n, const std::uint8_t *tag,
             void *dst) {
  check_size(n);
#ifdef SN_SGX_ENCLAVE
  require(sgx_rijndael128GCM_decrypt(reinterpret_cast<const sgx_aes_gcm_128bit_key_t *>(key.data()),
                                     static_cast<const std::uint8_t *>(src), n,
                                     static_cast<std::uint8_t *>(dst), iv.data(), iv.size(), nullptr, 0,
                                     reinterpret_cast<const sgx_aes_gcm_128bit_tag_t *>(tag)) == SGX_SUCCESS);
#else
  auto *ctx = EVP_CIPHER_CTX_new();
  require(ctx != nullptr);
  int out = 0, tail = 0;
  bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_gcm(), nullptr, key.data(), iv.data()) == 1;
  ok &= EVP_DecryptUpdate(ctx, static_cast<unsigned char *>(dst), &out,
                          static_cast<const unsigned char *>(src), n) == 1;
  ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<std::uint8_t *>(tag)) == 1;
  ok &= EVP_DecryptFinal_ex(ctx, static_cast<unsigned char *>(dst) + out, &tail) == 1;
  EVP_CIPHER_CTX_free(ctx);
  require(ok);
#endif
}
std::uint64_t prf(const key128 &key, std::uint64_t cid, std::uint64_t counter, std::uint64_t offset) {
  std::array<std::uint64_t, 3> input{cid, counter, offset};
  std::array<std::uint8_t, 16> output{};
#ifdef SN_SGX_ENCLAVE
  require(sgx_rijndael128_cmac_msg(reinterpret_cast<const sgx_cmac_128bit_key_t *>(key.data()),
                                   reinterpret_cast<const std::uint8_t *>(input.data()), sizeof(input),
                                   reinterpret_cast<sgx_cmac_128bit_tag_t *>(output.data())) == SGX_SUCCESS);
#else
  EVP_MAC *mac = EVP_MAC_fetch(nullptr, "CMAC", nullptr);
  require(mac != nullptr);
  EVP_MAC_CTX *ctx = EVP_MAC_CTX_new(mac);
  EVP_MAC_free(mac);
  require(ctx != nullptr);
  char cipher[] = "AES-128-CBC";
  OSSL_PARAM params[] = {OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, cipher, 0),
                         OSSL_PARAM_construct_end()};
  std::size_t count = 0;
  bool ok = EVP_MAC_init(ctx, key.data(), key.size(), params) == 1;
  ok &= EVP_MAC_update(ctx, reinterpret_cast<const unsigned char *>(input.data()), sizeof(input)) == 1;
  ok &= EVP_MAC_final(ctx, output.data(), &count, output.size()) == 1;
  EVP_MAC_CTX_free(ctx);
  require(ok && count == 16);
#endif
  std::uint64_t result;
  std::memcpy(&result, output.data(), sizeof(result));
  return result;
}
void *external_allocate(std::size_t n) {
  void *ptr = nullptr;
#ifdef SN_SGX_ENCLAVE
  std::uint64_t address = 0;
  require(qcovert_allocate(&address, n) == SGX_SUCCESS);
  ptr = reinterpret_cast<void *>(address);
  require(ptr != nullptr && sgx_is_outside_enclave(ptr, n));
#else
  ptr = std::malloc(n);
  require(ptr != nullptr);
#endif
  return ptr;
}
void external_free(void *ptr) noexcept {
#ifdef SN_SGX_ENCLAVE
  if (ptr)
    (void)qcovert_release(ptr);
#else
  std::free(ptr);
#endif
}
void external_read(void *dst, const void *src, std::size_t n) {
#ifdef SN_SGX_ENCLAVE
  require(sgx_is_outside_enclave(src, n));
  sgx_lfence();
#endif
  std::memcpy(dst, src, n);
}
void external_write(void *dst, const void *src, std::size_t n) {
#ifdef SN_SGX_ENCLAVE
  require(sgx_is_outside_enclave(dst, n));
  sgx_lfence();
#endif
  std::memcpy(dst, src, n);
}
} // namespace qcovert
