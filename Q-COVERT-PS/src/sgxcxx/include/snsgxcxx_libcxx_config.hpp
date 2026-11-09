#pragma once

#include <stddef.h>

#ifdef _LIBCPP_HAS_NO_ATOMIC_HEADER
#undef _LIBCPP_HAS_NO_ATOMIC_HEADER
#endif

#ifndef _LIBCPP_SGX_HAS_CXX_ATOMIC
#define _LIBCPP_SGX_HAS_CXX_ATOMIC
#endif

#ifndef _LIBCPP_ATOMIC_FLAG_TYPE
#define _LIBCPP_ATOMIC_FLAG_TYPE bool
#endif
