#include "ringdoram_sgx_u.h"
#include <cstdint>
#include <cstdlib>
extern "C" void qcovert_allocate(std::uint64_t *address, std::size_t bytes) {
  *address = reinterpret_cast<std::uint64_t>(std::malloc(bytes));
}
extern "C" void qcovert_release(void *address) { std::free(address); }
