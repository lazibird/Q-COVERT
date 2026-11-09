#include "sonic/crypto/random.hpp"

#include <openssl/rand.h>

namespace sn::crypto {

void random_device::fill(sn::util::span<std::uint8_t> out) const { fill(out.data(), out.size()); }

void random_device::fill(std::uint8_t* dst, std::size_t len) const {
  if (len == 0) {
    return;
  }
  const int rc = RAND_bytes(dst, static_cast<int>(len));
  if (rc != 1) {
    detail::throw_openssl_error("RAND_bytes");
  }
}

}
