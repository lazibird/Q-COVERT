#pragma once

#if defined(RINGDORAM_CRYPTO_BACKEND_SGX)
#include "sonic/crypto/impl/sgx/detail/utils.hpp"
#else
#include "sonic/crypto/impl/openssl/detail/utils.hpp"
#endif
