#include "sonic/crypto/random.hpp"

#include <limits>

#include <sgx_trts.h>

namespace sn::crypto {

void random_device::fill(sn::util::span<std::uint8_t> out) const { fill(out.data(), out.size()); }

void random_device::fill(std::uint8_t* dst, std::size_t len) const {
  if (len == 0) {
    return;
  }
  if (dst == nullptr) {
    throw error("sgx_read_rand: null destination");
  }

  constexpr auto max_request = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
  while (len != 0) {
    const auto step = len < max_request ? len : max_request;
    if (sgx_read_rand(dst, step) != SGX_SUCCESS) {
      throw error("sgx_read_rand failed");
    }
    dst += step;
    len -= step;
  }
}

} // namespace sn::crypto
