/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include <cstdio>
#include <cstdlib>

#include <sys/stat.h>
#include <sys/types.h>

#include "./impl/FaissAssert.h"
#include "./impl/io.h"
#include "./impl/io_macros.h"
#include "./impl/ProductQuantizer.h"

#include "IndexHNSW.h"



namespace faiss {

/*************************************************************
 * Read
 **************************************************************/

static void read_index_header(IndexHNSW* idx, IOReader* f);
static void read_HNSW(HNSW* hnsw, IOReader* f);
IndexHNSW* read_index(IOReader* f, int io_flags);
IndexHNSW* read_index(FILE* f, int io_flags);
IndexHNSW* read_index(const char* fname, int io_flags);

ProductQuantizer* read_pq(IOReader* f, int io_flags);
ProductQuantizer* read_pq(const char* fname, int io_flags);




} // namespace faiss
