/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

// -*- c++ -*-

#include <cstdio>
#include <cstdlib>
#include <iostream>

#include <sys/stat.h>
#include <sys/types.h>

#include "./impl/FaissAssert.h"
#include "./impl/io.h"
#include "./impl/io_macros.h"

#include "IndexHNSW.h"
#include "index_io.h"


namespace faiss {

/*************************************************************
 * Read
 **************************************************************/

static void read_index_header(IndexHNSW* idx, IOReader* f) {
    READ1(idx->d);
    // std::cout << "idx->d: " << idx->d << std::endl;
    READ1(idx->ntotal);
    // std::cout << "idx->ntotal: " << idx->ntotal << std::endl;
    idx_t dummy;
    READ1(dummy);
    READ1(dummy);
    READ1(idx->is_trained);
    READ1(idx->metric_type);
    if (idx->metric_type > 1) {
        READ1(idx->metric_arg);
    }
    idx->verbose = false;
}



static void read_HNSW(HNSW* hnsw, IOReader* f) {
    READVECTOR(hnsw->assign_probas);
    READVECTOR(hnsw->cum_nneighbor_per_level);
    READVECTOR(hnsw->levels);
    READVECTOR(hnsw->offsets);
    READVECTOR(hnsw->neighbors);

    READ1(hnsw->entry_point);
    READ1(hnsw->max_level);
    READ1(hnsw->efConstruction);
    READ1(hnsw->efSearch);
    READ1(hnsw->upper_beam);
}


IndexHNSW* read_index(IOReader* f, int io_flags) {
    uint32_t h;
    READ1(h);

    IndexHNSW* idxhnsw = new IndexHNSW();
    // std::cout << "Read index header..." << std::endl;
    read_index_header(idxhnsw, f);
    // std::cout << "Read HNSW..." << std::endl;
    read_HNSW(&idxhnsw->hnsw, f);
    // idxhnsw->storage = read_index(f, io_flags);
    idxhnsw->own_fields = true;

    return idxhnsw;
}

IndexHNSW* read_index(FILE* f, int io_flags) {
    FileIOReader reader(f);
    return read_index(&reader, io_flags);
}

IndexHNSW* read_index(const char* fname, int io_flags) {
    FileIOReader reader(fname);
    IndexHNSW* idx = read_index(&reader, io_flags);
    return idx;
}

static void read_ProductQuantizer(ProductQuantizer* pq, IOReader* f) {
    READ1(pq->d);
    READ1(pq->M);
    READ1(pq->nbits);
    pq->set_derived_values();
    READVECTOR(pq->centroids);
}

ProductQuantizer* read_pq(IOReader* f, int io_flags){
    // Skip index type
    uint32_t h;
    READ1(h);

    // Skip index header
    int d;
    idx_t ntotal;
    idx_t dummy;
    bool is_trained;
    faiss::MetricType metric_type;
    float metric_arg;

    READ1(d);
    READ1(ntotal);
    READ1(dummy);
    READ1(dummy);
    READ1(is_trained);
    READ1(metric_type);
    if (metric_type > 1) {
        READ1(metric_arg);
    }

    // Read PQ
    ProductQuantizer* pq = new ProductQuantizer();
    pq->metric = metric_type;
    read_ProductQuantizer(pq, f);

    READVECTOR(pq->codes);

    return pq;
}

ProductQuantizer* read_pq(const char* fname, int io_flags){
    FileIOReader reader(fname);
    return read_pq(&reader, io_flags);
}

} // namespace faiss
