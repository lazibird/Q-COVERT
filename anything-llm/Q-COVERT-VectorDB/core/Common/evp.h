#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <openssl/crypto.h>

/** AES-256 key held exclusively by the trusted query process. */
using AeadKey = std::array<unsigned char, 32>;

constexpr int kAeadNonceBytes = 16;
constexpr int kAeadTagBytes = 16;

/** Encrypt plaintext as ciphertext || GCM tag using a caller-provided nonce. */
int encrypt_wrapper_with_iv(const AeadKey& key,
                            const unsigned char* nonce,
                            const unsigned char* plaintext,
                            int plaintext_len,
                            unsigned char* ciphertext_and_tag);

/** Authenticate and decrypt ciphertext || GCM tag using a caller-provided nonce. */
int decrypt_wrapper_with_iv(const AeadKey& key,
                            const unsigned char* nonce,
                            const unsigned char* ciphertext_and_tag,
                            int ciphertext_and_tag_len,
                            unsigned char* plaintext);

/** Encrypt as nonce || ciphertext || GCM tag. */
int encrypt_wrapper(const AeadKey& key,
                    const unsigned char* plaintext,
                    int plaintext_len,
                    unsigned char* output);

/** Authenticate and decrypt nonce || ciphertext || GCM tag. */
int decrypt_wrapper(const AeadKey& key,
                    const unsigned char* input,
                    int input_len,
                    unsigned char* plaintext);

/** Encrypt and hash the complete authenticated ciphertext. */
int encrypt_and_hash(const AeadKey& key,
                     const unsigned char* plaintext,
                     int plaintext_len,
                     unsigned char* ciphertext,
                     unsigned char* hash_output);

/** Return nonce + plaintext-sized ciphertext + authentication-tag bytes. */
int compute_ctx_len(int plaintext_len);

void sha256_wrapper(const unsigned char* input, size_t length, uint8_t* output);

void sha256_twin_input_wrapper(
    const unsigned char* input1, size_t length1,
    const unsigned char* input2, size_t length2,
    uint8_t* output);

void sha256_trib_input_wrapper(
    const unsigned char* input1, size_t length1,
    const unsigned char* input2, size_t length2,
    const unsigned char* input3, size_t length3,
    uint8_t* output);

int double_oram_prf_leaf(uint64_t seed,
                         int cluster_id,
                         uint64_t counter,
                         int path_index,
                         uint64_t retry,
                         int num_leaves);
