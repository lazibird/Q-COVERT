#pragma once

#if !defined(SN_LOG_HAS_CONSOLE)
#if defined(SN_SGX_ENCLAVE)
#define SN_LOG_HAS_CONSOLE 1
#elif defined(SONIC_NO_OS) && (SONIC_NO_OS)
#define SN_LOG_HAS_CONSOLE 0
#else
#define SN_LOG_HAS_CONSOLE 1
#endif
#endif

#if !defined(SN_LOG_HAS_COLOR)
#if defined(SN_SGX_ENCLAVE)
#define SN_LOG_HAS_COLOR 1
#elif defined(SONIC_NO_OS) && (SONIC_NO_OS)
#define SN_LOG_HAS_COLOR 0
#else
#define SN_LOG_HAS_COLOR 1
#endif
#endif

#if !defined(SN_LOG_HAS_PROGRESS)
#if defined(SONIC_NO_OS) && (SONIC_NO_OS)
#define SN_LOG_HAS_PROGRESS 0
#else
#define SN_LOG_HAS_PROGRESS 1
#endif
#endif
