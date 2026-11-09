/*
  * From: https://wiki.openssl.org/index.php/EVP_Symmetric_Encryption_and_Decryption
  */
# pragma once
#include <openssl/conf.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/rand.h>

int encrypt_wrapper_with_iv(unsigned char *iv, unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext);

int decrypt_wrapper_with_iv(unsigned char *iv, unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext);


int encrypt_wrapper(unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext);

int encrypt_and_hash(unsigned char *plaintext, int plaintext_len, unsigned char *ctx_output, unsigned char *hash_output);

int decrypt_wrapper(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext);

int compute_ctx_len(int l);

void sha256_wrapper(unsigned char* input, size_t length, uint8_t* output);

void sha256_twin_input_wrapper(
  unsigned char* input1, size_t length1, 
  unsigned char* input2, size_t length2, 
  uint8_t* output
  );

void sha256_trib_input_wrapper(
  unsigned char* input1, size_t length1, 
  unsigned char* input2, size_t length2, 
  unsigned char* input3, size_t length3, 
  uint8_t* output
  );