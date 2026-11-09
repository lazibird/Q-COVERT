#pragma once

#include "backend_cpp.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
#include "backend_x64.hpp"
#endif

namespace sn {
namespace obliv {
namespace detail {

#ifdef SN_OBLIV_BACKEND_OVERRIDE
#error "Do not use SN_OBLIV_BACKEND_OVERRIDE."
using default_backend = SN_OBLIV_BACKEND_OVERRIDE;
#else
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
using default_backend = backends::backend_x64;
#else
using default_backend = backends::backend_cpp;
#endif
#endif

}
}
}
