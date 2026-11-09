/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/time.h>

#include <faiss/AutoTune.h>
#include <faiss/index_factory.h>
#include <faiss/index_io.h>
#include <faiss/IndexHNSW.h>




float* fvecs_read(const char* fname, size_t* d_out, size_t* n_out) {
    FILE* f = fopen(fname, "r");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }
    int d;
    fread(&d, 1, sizeof(int), f);
    assert((d > 0 && d < 1000000) || !"unreasonable dimension");
    fseek(f, 0, SEEK_SET);
    struct stat st;
    fstat(fileno(f), &st);
    size_t sz = st.st_size;
    assert(sz % ((d + 1) * 4) == 0 || !"weird file size");
    size_t n = sz / ((d + 1) * 4);

    *d_out = d;
    *n_out = n;
    float* x = new float[n * (d + 1)];
    size_t nr = fread(x, sizeof(float), n * (d + 1), f);
    assert(nr == n * (d + 1) || !"could not read whole file");

    // shift array to remove row headers
    for (size_t i = 0; i < n; i++)
        memmove(x + i * d, x + 1 + i * (d + 1), d * sizeof(*x));

    fclose(f);
    return x;
}

void fvecs_write(const char* fname, float* data, size_t d, size_t n) {
    FILE* f = fopen(fname, "w");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }
    for (size_t i = 0; i < n; i++){
        fwrite(&d, 1, sizeof(int), f);
        fwrite(data + i*d, d, sizeof(float), f);
    }
    fclose(f);
}

// not very clean, but works as long as sizeof(int) == sizeof(float)
// int* ivecs_read(const char* fname, size_t* d_out, size_t* n_out) {
//     return (int*)fvecs_read(fname, d_out, n_out);
// }

void ivecs_write(const char* fname, int* data, size_t d, size_t n) {
    fvecs_write(fname, (float*)data, d, n);
}

double elapsed() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main() {
    double t0 = elapsed();

    // this is typically the fastest one.
    const char* index_key = "Flat";

    faiss::Index* index;

    size_t d;

    {
        printf("[%.3f s] Loading database\n", elapsed() - t0);

        size_t nb, d2;
        float* xb = fvecs_read("/home/clive/see/data/dataset/laion1m/1k/laion_base.fvecs", &d2, &nb);
        d = d2;
        assert(d == d2 || !"dataset does not have same dimension as train set");

        printf("[%.3f s] Indexing database, size %ld*%ld\n",
               elapsed() - t0,
               nb,
               d);

        index = faiss::index_factory(d, index_key);
        index->metric_type = faiss::METRIC_INNER_PRODUCT;

        index->add(nb, xb);

        delete[] xb;
    }

    size_t nq;
    float* xq;

    {
        printf("[%.3f s] Loading queries\n", elapsed() - t0);

        size_t d2;
        xq = fvecs_read("/home/clive/see/data/dataset/laion1m/laion_query.fvecs", &d2, &nq);
        assert(d == d2 || !"query does not have same dimension as train set");
    }

    size_t k = 10;       // nb of results per query in the GT

    // Result of the auto-tuning
    std::string selected_params;

    { // Search for GT

        faiss::idx_t* I = new faiss::idx_t[nq * k];
        float* D = new float[nq * k];

        faiss::SearchParametersHNSW* params = new faiss::SearchParametersHNSW();

        printf("[%.3f s] Perform a search on %ld queries with k %ld\n",
            elapsed() - t0,
            nq,
            k);

        index->search(nq, xq, k, D, I, params);

        int* gt = new int[nq * k];
        for(int i = 0; i < nq*k; i++){
            gt[i] = I[i];
        }

        printf("[%.3f s] Save generate Ground Truth\n", elapsed() - t0);
        fvecs_write("/home/clive/see/data/dataset/laion1m/1k/gt.ivecs", (float*)gt, k, nq);

        delete[] gt;
        delete[] I;
        delete[] D;
    }

    // write_index(index, "./trip_norm_hnsw.index");

    delete[] xq;
    delete index;
    return 0;
}
