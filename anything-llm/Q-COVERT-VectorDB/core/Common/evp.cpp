#include "evp.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;

[[noreturn]] void crypto_error(const char* operation) {
    throw std::runtime_error(std::string("OpenSSL failure during ") + operation);
}

void validate_length(int length, int minimum, const char* field) {
    if (length < minimum) {
        throw std::invalid_argument(std::string(field) + " has an invalid length");
    }
}

void put_u64_be(unsigned char* out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = static_cast<unsigned char>((value >> (i * 8)) & 0xffU);
    }
}

uint64_t get_u64_be(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8U) | static_cast<uint64_t>(in[i]);
    }
    return value;
}

} // namespace

int encrypt_wrapper_with_iv(const AeadKey& key,
                            const unsigned char* nonce,
                            const unsigned char* plaintext,
                            int plaintext_len,
                            unsigned char* ciphertext_and_tag) {
    validate_length(plaintext_len, 0, "plaintext");
    if (nonce == nullptr || plaintext == nullptr || ciphertext_and_tag == nullptr) {
        throw std::invalid_argument("AES-GCM buffers must not be null");
    }

    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, kAeadNonceBytes, nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce) != 1) {
        crypto_error("AES-256-GCM initialization");
    }

    int written = 0;
    int total = 0;
    if (EVP_EncryptUpdate(context.get(), ciphertext_and_tag, &written, plaintext, plaintext_len) != 1) {
        crypto_error("AES-256-GCM encryption");
    }
    total += written;
    if (EVP_EncryptFinal_ex(context.get(), ciphertext_and_tag + total, &written) != 1) {
        crypto_error("AES-256-GCM finalization");
    }
    total += written;
    if (total != plaintext_len ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, kAeadTagBytes,
                            ciphertext_and_tag + total) != 1) {
        crypto_error("AES-256-GCM tag generation");
    }
    return total + kAeadTagBytes;
}

int decrypt_wrapper_with_iv(const AeadKey& key,
                            const unsigned char* nonce,
                            const unsigned char* ciphertext_and_tag,
                            int ciphertext_and_tag_len,
                            unsigned char* plaintext) {
    validate_length(ciphertext_and_tag_len, kAeadTagBytes, "authenticated ciphertext");
    if (nonce == nullptr || ciphertext_and_tag == nullptr || plaintext == nullptr) {
        throw std::invalid_argument("AES-GCM buffers must not be null");
    }
    const int ciphertext_len = ciphertext_and_tag_len - kAeadTagBytes;

    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context ||
        EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, kAeadNonceBytes, nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce) != 1) {
        crypto_error("AES-256-GCM decryption initialization");
    }

    int written = 0;
    int total = 0;
    if (EVP_DecryptUpdate(context.get(), plaintext, &written,
                          ciphertext_and_tag, ciphertext_len) != 1) {
        crypto_error("AES-256-GCM decryption");
    }
    total += written;
    unsigned char tag[kAeadTagBytes];
    std::memcpy(tag, ciphertext_and_tag + ciphertext_len, sizeof(tag));
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, kAeadTagBytes, tag) != 1 ||
        EVP_DecryptFinal_ex(context.get(), plaintext + total, &written) != 1) {
        throw std::runtime_error("AES-256-GCM authentication failed");
    }
    total += written;
    return total;
}

int encrypt_wrapper(const AeadKey& key,
                    const unsigned char* plaintext,
                    int plaintext_len,
                    unsigned char* output) {
    if (output == nullptr) {
        throw std::invalid_argument("ciphertext output must not be null");
    }
    if (RAND_bytes(output, kAeadNonceBytes) != 1) {
        crypto_error("nonce generation");
    }
    return kAeadNonceBytes + encrypt_wrapper_with_iv(
        key, output, plaintext, plaintext_len, output + kAeadNonceBytes);
}

int decrypt_wrapper(const AeadKey& key,
                    const unsigned char* input,
                    int input_len,
                    unsigned char* plaintext) {
    validate_length(input_len, kAeadNonceBytes + kAeadTagBytes, "authenticated ciphertext");
    return decrypt_wrapper_with_iv(key, input, input + kAeadNonceBytes,
                                   input_len - kAeadNonceBytes, plaintext);
}

int compute_ctx_len(int plaintext_len) {
    validate_length(plaintext_len, 0, "plaintext");
    if (plaintext_len > std::numeric_limits<int>::max() - kAeadNonceBytes - kAeadTagBytes) {
        throw std::overflow_error("authenticated ciphertext length overflow");
    }
    return plaintext_len + kAeadNonceBytes + kAeadTagBytes;
}

void sha256_wrapper(const unsigned char* input, size_t length, uint8_t* output) {
    if (input == nullptr || output == nullptr ||
        EVP_Digest(input, length, output, nullptr, EVP_sha256(), nullptr) != 1) {
        crypto_error("SHA-256");
    }
}

void sha256_twin_input_wrapper(const unsigned char* input1, size_t length1,
                               const unsigned char* input2, size_t length2,
                               uint8_t* output) {
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (raw == nullptr) crypto_error("SHA-256 context allocation");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), input1, length1) != 1 ||
        EVP_DigestUpdate(context.get(), input2, length2) != 1 ||
        EVP_DigestFinal_ex(context.get(), output, nullptr) != 1) {
        crypto_error("SHA-256");
    }
}

void sha256_trib_input_wrapper(const unsigned char* input1, size_t length1,
                               const unsigned char* input2, size_t length2,
                               const unsigned char* input3, size_t length3,
                               uint8_t* output) {
    EVP_MD_CTX* raw = EVP_MD_CTX_new();
    if (raw == nullptr) crypto_error("SHA-256 context allocation");
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(raw, EVP_MD_CTX_free);
    if (EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), input1, length1) != 1 ||
        EVP_DigestUpdate(context.get(), input2, length2) != 1 ||
        EVP_DigestUpdate(context.get(), input3, length3) != 1 ||
        EVP_DigestFinal_ex(context.get(), output, nullptr) != 1) {
        crypto_error("SHA-256");
    }
}

int encrypt_and_hash(const AeadKey& key,
                     const unsigned char* plaintext,
                     int plaintext_len,
                     unsigned char* ciphertext,
                     unsigned char* hash_output) {
    const int ciphertext_len = encrypt_wrapper(key, plaintext, plaintext_len, ciphertext);
    sha256_wrapper(ciphertext, static_cast<size_t>(ciphertext_len), hash_output);
    return ciphertext_len;
}

int double_oram_prf_leaf(uint64_t seed, int cluster_id, uint64_t counter,
                         int path_index, uint64_t retry, int num_leaves) {
    if (cluster_id < 0 || path_index < 0 || num_leaves <= 0) {
        throw std::invalid_argument("invalid ORAM PRF leaf parameters");
    }
    unsigned char input[40];
    put_u64_be(input, seed);
    put_u64_be(input + 8, static_cast<uint64_t>(cluster_id));
    put_u64_be(input + 16, counter);
    put_u64_be(input + 24, static_cast<uint64_t>(path_index));
    put_u64_be(input + 32, retry);
    uint8_t digest[SHA256_DIGEST_LENGTH];
    sha256_wrapper(input, sizeof(input), digest);
    return static_cast<int>(get_u64_be(digest) % static_cast<uint64_t>(num_leaves));
}
