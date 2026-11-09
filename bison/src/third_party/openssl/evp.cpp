#include "evp.h"
#include <string.h>
#include <cassert>

// INSECURE but ok for just testing？

/* A 256 bit key */
unsigned char key[32] = { 0x30, 0x33, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                        0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35,
                        0x36, 0x37, 0x38, 0x39, 0x30, 0x31, 0x32, 0x33,
                        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30, 0x31
                        };

/* A 128 bit IV */
unsigned char iv[16] = { 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
                        0x38, 0x39, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35
                    };

void handleErrors(void) {
    ERR_print_errors_fp(stderr);
    abort();
}

void generate_random_iv(unsigned char *iv, size_t iv_len) {
    if(1 != RAND_bytes(iv, iv_len))
        handleErrors();
}

int encrypt(unsigned char *plaintext, int plaintext_len, unsigned char *key,
            unsigned char *iv, unsigned char *ciphertext)
{
    EVP_CIPHER_CTX *ctx;

    int len;

    int ciphertext_len;

    /* Create and initialise the context */
    if(!(ctx = EVP_CIPHER_CTX_new()))
        handleErrors();

    /*
     * Initialise the encryption operation. IMPORTANT - ensure you use a key
     * and IV size appropriate for your cipher
     * In this example we are using 256 bit AES (i.e. a 256 bit key). The
     * IV size for *most* modes is the same as the block size. For AES this
     * is 128 bits
     */
    if(1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
        handleErrors();

    /*
     * Provide the message to be encrypted, and obtain the encrypted output.
     * EVP_EncryptUpdate can be called multiple times if necessary
     */
    if(1 != EVP_EncryptUpdate(ctx, ciphertext, &len, plaintext, plaintext_len))
        handleErrors();
    ciphertext_len = len;

    /*
     * Finalise the encryption. Further ciphertext bytes may be written at
     * this stage.
     */
    if(1 != EVP_EncryptFinal_ex(ctx, ciphertext + len, &len))
        handleErrors();
    ciphertext_len += len;

    /* Clean up */
    EVP_CIPHER_CTX_free(ctx);

    return ciphertext_len;
}

int decrypt(unsigned char *ciphertext, int ciphertext_len, unsigned char *key,
            unsigned char *iv, unsigned char *plaintext)
{
    EVP_CIPHER_CTX *ctx;

    int len;

    int plaintext_len;

    /* Create and initialise the context */
    if(!(ctx = EVP_CIPHER_CTX_new()))
        handleErrors();

    /*
     * Initialise the decryption operation. IMPORTANT - ensure you use a key
     * and IV size appropriate for your cipher
     * In this example we are using 256 bit AES (i.e. a 256 bit key). The
     * IV size for *most* modes is the same as the block size. For AES this
     * is 128 bits
     */
    if(1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, key, iv))
        handleErrors();

    /*
     * Provide the message to be decrypted, and obtain the plaintext output.
     * EVP_DecryptUpdate can be called multiple times if necessary.
     */
    if(1 != EVP_DecryptUpdate(ctx, plaintext, &len, ciphertext, ciphertext_len))
        handleErrors();
    plaintext_len = len;

    /*
     * Finalise the decryption. Further plaintext bytes may be written at
     * this stage.
     */
    if(1 != EVP_DecryptFinal_ex(ctx, plaintext + len, &len))
        handleErrors();
    plaintext_len += len;

    /* Clean up */
    EVP_CIPHER_CTX_free(ctx);

    return plaintext_len;
}

int encrypt_wrapper(unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext) {
    unsigned char iv[16];  
    generate_random_iv(iv, sizeof(iv));

    // Copy IV to the beginning of ciphertext
    memcpy(ciphertext, iv, sizeof(iv));

    // Perform encryption, ciphertext after the IV
    int ciphertext_len = encrypt(plaintext, plaintext_len, key, iv, ciphertext + sizeof(iv));

    // Return total length (ciphertext + IV)
    return ciphertext_len + sizeof(iv);
}

int decrypt_wrapper(unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext) {
    if(ciphertext_len < 16)
        handleErrors();  // invalid ciphertext length

    unsigned char iv[16];
    memcpy(iv, ciphertext, sizeof(iv));

    // Actual ciphertext is after IV
    return decrypt(ciphertext + sizeof(iv), ciphertext_len - sizeof(iv), key, iv, plaintext);
}

int encrypt_wrapper_with_iv(unsigned char *iv, unsigned char *plaintext, int plaintext_len, unsigned char *ciphertext) {

    // Copy IV to the beginning of ciphertext
    // memcpy(ciphertext, iv, sizeof(iv));

    // Perform encryption, ciphertext after the IV
    int ciphertext_len = encrypt(plaintext, plaintext_len, key, iv, ciphertext);

    // Return total length (ciphertext + IV)
    return ciphertext_len;
}

int decrypt_wrapper_with_iv(unsigned char *iv, unsigned char *ciphertext, int ciphertext_len, unsigned char *plaintext) {

    // Actual ciphertext is after IV
    return decrypt(ciphertext, ciphertext_len, key, iv, plaintext);
}

// Get the length of aes encryption result based on input length
int compute_ctx_len(int l){
    return (l / 16 + 1) * 16 + 16; 
}

void sha256_wrapper(unsigned char* input, size_t length, uint8_t* output){

    const EVP_MD* md = EVP_sha256();

    int rc =
      EVP_Digest(input, length, output, nullptr, md, nullptr);
    if (rc != 1)
    {
      assert(0);
    }

}

void sha256_twin_input_wrapper(
  unsigned char* input1, size_t length1, 
  unsigned char* input2, size_t length2, 
  uint8_t* output
  ){

    EVP_MD_CTX *mdctx;
    const EVP_MD *md;

    OpenSSL_add_all_digests();
    md = EVP_get_digestbyname("sha256");
    if (md == NULL) {
        assert(0);
    }

    mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, md, NULL);
    EVP_DigestUpdate(mdctx, input1, length1);
    EVP_DigestUpdate(mdctx, input2, length2);
    EVP_DigestFinal_ex(mdctx, output, NULL);
    EVP_MD_CTX_free(mdctx);

}

void sha256_trib_input_wrapper(
  unsigned char* input1, size_t length1, 
  unsigned char* input2, size_t length2, 
  unsigned char* input3, size_t length3, 
  uint8_t* output
  ){

    EVP_MD_CTX *mdctx;
    const EVP_MD *md;

    OpenSSL_add_all_digests();
    md = EVP_get_digestbyname("sha256");
    if (md == NULL) {
        assert(0);
    }

    mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, md, NULL);
    EVP_DigestUpdate(mdctx, input1, length1);
    EVP_DigestUpdate(mdctx, input2, length2);
    EVP_DigestUpdate(mdctx, input3, length3);
    EVP_DigestFinal_ex(mdctx, output, NULL);
    EVP_MD_CTX_free(mdctx);

}


int encrypt_and_hash(unsigned char *plaintext, int plaintext_len, unsigned char *ctx_output, unsigned char *hash_output){
    int ctx_len = encrypt_wrapper(plaintext, plaintext_len, ctx_output);
    sha256_wrapper(ctx_output, ctx_len, hash_output);
    return ctx_len;
}