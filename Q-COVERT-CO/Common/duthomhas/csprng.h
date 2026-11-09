/*
// Interface to the OS Cryptographically-Secure Pseudo-Random Number Generator
// Copyright 2017 Michael Thomas Greer
// Distributed under the Boost Software License, Version 1.0.
// (See accompanying file ../../LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt )
*/

#pragma once
#ifndef DOUBLE_ORAM_DUTHOMHAS_CSPRNG_H
#define DOUBLE_ORAM_DUTHOMHAS_CSPRNG_H

/* ------------------------------------------------------------------------------------------------
 * DOUBLE_ORAM_CSPRNG
 */
typedef void* DOUBLE_ORAM_CSPRNG;

#ifdef __cplusplus
extern "C" {
#endif

DOUBLE_ORAM_CSPRNG double_oram_csprng_create();
/* Returns a new DOUBLE_ORAM_CSPRNG object, 0 on error.
 */

int double_oram_csprng_get( DOUBLE_ORAM_CSPRNG, void* dest, unsigned long long size );
/* Fills $dest with $size bytes worth of random data.
 * Returns 1 on succes, 0 on failure.
 */

long double_oram_csprng_get_int( DOUBLE_ORAM_CSPRNG );
/* Return a random value
 * There is no way to know if it failed.
 * (If it did fail, the result will be 0, which is fairly unlikely
 *  to occur multiple times in sequence.
 *  It is also unlikely to fail at all if double_oram_csprng_create() succeeded.)
 */

DOUBLE_ORAM_CSPRNG double_oram_csprng_destroy( DOUBLE_ORAM_CSPRNG );
/* Destroy an existing DOUBLE_ORAM_CSPRNG object. Returns 0.
 * Use it as:
 *   v = double_oram_csprng_destroy( v );
 */

#ifdef __cplusplus
}
#endif

#endif