/*
// Source for the OS Cryptographically-Secure Pseudo-Random Number Generator
// Copyright 2017 Michael Thomas Greer
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file ../LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt )
*/

#include "./duthomhas/csprng.h"

/* ///////////////////////////////////////////////////////////////////////////////////////////// */
#ifdef _WIN32
/* ///////////////////////////////////////////////////////////////////////////////////////////// */

  #include <windows.h>
  #include <wincrypt.h>

  #ifdef _MSC_VER
  #pragma comment(lib, "advapi32.lib")
  #endif
  
  #ifdef __cplusplus
  extern "C" {
  #endif

  /* ------------------------------------------------------------------------------------------- */
  typedef union
  {
    DOUBLE_ORAM_CSPRNG     object;
    HCRYPTPROV hCryptProv;
  }
  CSPRNG_TYPE;

  /* ------------------------------------------------------------------------------------------- */
  DOUBLE_ORAM_CSPRNG double_oram_csprng_create()
  {
    CSPRNG_TYPE csprng;
    if (!CryptAcquireContextA( &csprng.hCryptProv, NULL, NULL, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT | CRYPT_SILENT ))
      csprng.hCryptProv = 0;
    return csprng.object;
  }

  /* ------------------------------------------------------------------------------------------- */
  int double_oram_csprng_get( DOUBLE_ORAM_CSPRNG object, void* dest, unsigned long long size )
  {
    // Alas, we have to be pedantic here. double_oram_csprng_get().size is a 64-bit entity.
    // However, CryptGenRandom().size is only a 32-bit DWORD. So we have to make sure failure 
    // isn't from providing less random data than requested, even if absurd.
    unsigned long long n;
    
    CSPRNG_TYPE csprng;
    csprng.object = object;
    if (!csprng.hCryptProv) return 0;
    
    n = size >> 30;
    while (n--)
      if (!CryptGenRandom( csprng.hCryptProv, 1UL << 30, (BYTE*)dest )) return 0;
      
    return !!CryptGenRandom( csprng.hCryptProv, size & ((1ULL << 30) - 1), (BYTE*)dest );
  }

  /* ------------------------------------------------------------------------------------------- */
  long double_oram_csprng_get_int( DOUBLE_ORAM_CSPRNG object )
  {
    long result;
    return double_oram_csprng_get( object, &result, sizeof(result) ) ? result : 0;
  }

  /* ------------------------------------------------------------------------------------------- */
  DOUBLE_ORAM_CSPRNG double_oram_csprng_destroy( DOUBLE_ORAM_CSPRNG object )
  {
    CSPRNG_TYPE csprng;
    csprng.object = object;
    if (csprng.hCryptProv) CryptReleaseContext( csprng.hCryptProv, 0 );
    return 0;
  }

  #ifdef __cplusplus
  }
  #endif

/* ///////////////////////////////////////////////////////////////////////////////////////////// */
#else  /* Using /dev/urandom                                                                     */
/* ///////////////////////////////////////////////////////////////////////////////////////////// */

  #include <stdio.h>

  #ifdef __cplusplus
  extern "C" {
  #endif

  /* ------------------------------------------------------------------------------------------- */
  typedef union
  {
    DOUBLE_ORAM_CSPRNG object;
    FILE*  urandom;
  }
  CSPRNG_TYPE;

  /* ------------------------------------------------------------------------------------------- */
  DOUBLE_ORAM_CSPRNG double_oram_csprng_create()
  {
    CSPRNG_TYPE csprng;
    csprng.urandom = fopen( "/dev/urandom", "rb" );
    return csprng.object;
  }

  /* ------------------------------------------------------------------------------------------- */
  int double_oram_csprng_get( DOUBLE_ORAM_CSPRNG object, void* dest, unsigned long long size )
  {
    CSPRNG_TYPE csprng;
    csprng.object = object;
    return (csprng.urandom) && (fread( (char*)dest, 1, size, csprng.urandom ) == size);
  }

  /* ------------------------------------------------------------------------------------------- */
  long double_oram_csprng_get_int( DOUBLE_ORAM_CSPRNG object )
  {
    long result;
    return double_oram_csprng_get( object, &result, sizeof(result) ) ? result : 0;
  }

  /* ------------------------------------------------------------------------------------------- */
  DOUBLE_ORAM_CSPRNG double_oram_csprng_destroy( DOUBLE_ORAM_CSPRNG object )
  {
    CSPRNG_TYPE csprng;
    csprng.object = object;
    if (csprng.urandom) fclose( csprng.urandom );
    return 0;
  }
  
  #ifdef __cplusplus
  }
  #endif

#endif