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
#include <faiss/IndexLSH.h>
#include <faiss/IndexFlat.h>

#define TUNING false

/**
 * To run this demo, please download the ANN_SIFT1M dataset from
 *
 *   http://corpus-texmex.irisa.fr/
 *
 * and unzip it to the sudirectory sift1M.
 **/

/*****************************************************
 * I/O functions for fvecs and ivecs
 *****************************************************/

// ground truth labels @gt, results to evaluate @I with @nq queries, returns @gt_size-Recall@k where gt had max gt_size NN's per query
float compute_recall(faiss::idx_t* gt, int gt_size, faiss::idx_t* I, int nq, int k, int gamma=1) {
    // printf("compute_recall params: gt.size(): %ld, gt_size: %d, I.size(): %ld, nq: %d, k: %d, gamma: %d\n", gt.size(), gt_size, I.size(), nq, k, gamma);
    int n_1 = 0, n_10 = 0, n_100 = 0;
    for (int i = 0; i < nq; i++) { // loop over all queries
        // int gt_nn = gt[i * k];
        faiss::idx_t* first = gt + i*gt_size;
        faiss::idx_t* last = gt + i*gt_size + (k / gamma);
        std::vector<faiss::idx_t> gt_nns_tmp(first, last);
        // if (gt_nns_tmp.size() > 10) {
        //     printf("gt_nns size: %ld\n", gt_nns_tmp.size());
        // }
        // gt_nns_tmp.resize(k); // truncate if gt_size > k
        // std::set<faiss::idx_t> gt_nns_100(gt_nns_tmp.begin(), gt_nns_tmp.end());
        // gt_nns_tmp.resize(10);
        std::set<faiss::idx_t> gt_nns_10(gt_nns_tmp.begin(), gt_nns_tmp.end());
        gt_nns_tmp.resize(1);
        std::set<faiss::idx_t> gt_nns_1(gt_nns_tmp.begin(), gt_nns_tmp.end());
        // if (gt_nns.size() > 10) {
        //     printf("gt_nns size: %ld\n", gt_nns.size());
        // }
        for (int j = 0; j < k; j++) { // iterate over returned nn results
            // if (gt_nns_100.count(I[i * k + j])!=0) {
            //     if (j < 100 * gamma)
            //         n_100++;
            // }
            if (gt_nns_10.count(I[i * k + j])!=0) {
                if (j < 10 * gamma)
                    n_10++;
            }
            if (gt_nns_1.count(I[i * k + j])!=0) {
                if (j < 1 * gamma)
                    n_1++;
            }
        }
    }
    // BASE ACCURACY
    printf("* Base HNSW accuracy relative to exact search:\n");
    printf("\tR@1 = %.4f\n", n_1 / float(nq) );
    printf("\tR@10 = %.4f\n", n_10 / float(nq));
    // printf("\tR@100 = %.4f\n", n_100 / float(nq)); // not sure why this is always same as R@10
    // printf("\t---Results for %ld queries, k=%d, N=%ld, gt_size=%d\n", nq, k, N, gt_size);
    return (n_10 / float(nq));
}

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

// not very clean, but works as long as sizeof(int) == sizeof(float)
int* ivecs_read(const char* fname, size_t* d_out, size_t* n_out) {
    return (int*)fvecs_read(fname, d_out, n_out);
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

    int num_tables = 20;
    int num_probes = 100;
    int nbits = 64;

    std::vector<faiss::Index*> indices;
    std::vector<faiss::Index*> flat_indices;
    std::vector<std::set<faiss::idx_t>> result_set;
    

    size_t d;

    // {
        // const char* index_key = "LSH32";
        
        printf("[%.3f s] Loading database\n", elapsed() - t0);

        size_t nb, d2;
        float* xb = fvecs_read("/home/clive/see/data/dataset/trip_distilbert/passages.fvecs", &d2, &nb);
        d = d2;
        assert(d == d2 || !"dataset does not have same dimension as train set");

        printf("[%.3f s] Indexing database, size %ld*%ld\n",
               elapsed() - t0,
               nb,
               d);

        for(int i = 0; i < num_tables; i++){
            faiss::Index* index = new faiss::IndexLSH(d, nbits);
            // index->metric_type = faiss::METRIC_INNER_PRODUCT;
            index->add(nb, xb);
            indices.push_back(index);
        }

        // faiss::write_index(index, "/home/clive/see/data/dataset/trip_distilbert/hnsw_48_60_2_ip.index");
        // delete[] xb;
        // return 0;
    // }

    // index = faiss::read_index("/home/clive/see/data/dataset/trip_distilbert/hnsw_64_80_2_ip.index", 0);
    // d = 768;

    size_t nq;
    float* xq;

    {
        printf("[%.3f s] Loading queries\n", elapsed() - t0);

        size_t d2;
        xq = fvecs_read("/home/clive/see/data/dataset/trip_distilbert/queries.fvecs", &d2, &nq);
        assert(d == d2 || !"query does not have same dimension as train set");
        printf("[%.3f s] Loaded %ld queries\n", elapsed() - t0, nq);

        for(int i = 0; i < nq; i++){
            flat_indices.push_back(new faiss::IndexFlatIP(d));
            result_set.push_back(std::set<faiss::idx_t>());
        } 
    }

    size_t k;         // nb of results per query in the GT
    faiss::idx_t* gt; // nq * k matrix of ground-truth nearest-neighbors

    // k = 10;

    {
        printf("[%.3f s] Loading ground truth for %ld queries\n",
               elapsed() - t0,
               nq);

        // load ground-truth and convert int to long
        size_t nq2;
        int* gt_int = ivecs_read("/home/clive/see/data/dataset/trip_distilbert/gt_10.ivecs", &k, &nq2);
        assert(nq2 == nq || !"incorrect nb of ground truth entries");

        gt = new faiss::idx_t[k * nq];
        for (int i = 0; i < k * nq; i++) {
            gt[i] = gt_int[i];
        }
        delete[] gt_int;
    }

    {
        // We only need k = 10
        int k0 = 10;
        faiss::idx_t* gt0 = new faiss::idx_t[k0 * nq];
        for(int i = 0; i < nq; i++){
            for(int j = 0; j < k0; j++){
                gt0[i*k0 + j] = gt[i*k + j];
            }
        }

        k = k0;
        gt = gt0;
    }


    { // Use the found configuration to perform a search

        // for(int efs = 16; efs <= 48; efs+=16){

        printf("[%.3f s] Perform a search on %ld queries with k %ld\n",
               elapsed() - t0,
               nq,
               k);


        for(auto index : indices){
            faiss::idx_t* I = new faiss::idx_t[nq * num_probes];
            float* D = new float[nq * num_probes];

            index->search(nq, xq, num_probes, D, I, nullptr);

            for(int iq = 0; iq < nq; iq++){
                for(int j = 0; j < num_probes; j++){
                    result_set[iq].insert(I[iq*num_probes + j]);
                }
            }

            delete[] I;
            delete[] D;
        }

        faiss::idx_t* I = new faiss::idx_t[nq * k];

        for(int iq = 0; iq < nq; iq++){
            // Get result vector
            std::vector<faiss::idx_t> res(result_set[iq].begin(), result_set[iq].end());

            for(auto id : res){
                flat_indices[iq]->add(1, xb + id*d);
            }

            faiss::idx_t* tmp_I = new faiss::idx_t[nq * k];
            float* tmp_D = new float[nq * k];

            flat_indices[iq]->search(1, xq + iq*d, k, tmp_D, tmp_I, nullptr);

            for(int j = 0; j < k; j++){
                I[iq*k + j] = res[tmp_I[j]];
            }
        }

        compute_recall(
            gt,
            k,
            I,
            nq,
            k
        );

    }

    delete[] xq;
    delete[] gt;
    delete[] xb;
    return 0;
}
