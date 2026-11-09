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

double elapsed() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec * 1e-6;
}

int main() {
    double t0 = elapsed();

    // this is typically the fastest one.
    const char* index_key = "HNSW32";

    // these ones have better memory usage
    // const char *index_key = "Flat";
    // const char *index_key = "PQ32";
    // const char *index_key = "PCA80,Flat";
    // const char *index_key = "IVF4096,PQ8+16";
    // const char *index_key = "IVF4096,PQ32";
    // const char *index_key = "IMI2x8,PQ32";
    // const char *index_key = "IMI2x8,PQ8+16";
    // const char *index_key = "OPQ16_64,IMI2x8,PQ8+16";

    faiss::Index* index;

    size_t d;

    faiss::Index* pq_index;

    // {
    //     const char* pq_index_key = "PQ32";
    //     printf("[%.3f s] Loading train set\n", elapsed() - t0);

    //     size_t nt;
    //     float* xt = fvecs_read("../../../downloads/sift/learn.fvecs", &d, &nt);

    //     printf("[%.3f s] Preparing index \"%s\" d=%ld\n",
    //            elapsed() - t0,
    //            index_key,
    //            d);
    //     pq_index = faiss::index_factory(d, pq_index_key);

    //     // dynamic_cast<faiss::IndexPQ*>(pq_index)->do_polysemous_training = false;

    //     printf("[%.3f s] Training on %ld vectors\n", elapsed() - t0, nt);

    //     pq_index->train(nt, xt);

    //     dynamic_cast<faiss::IndexPQ*>(pq_index)->pq.compute_sdc_table();

    //     size_t nb, d2;
    //     float* xb = fvecs_read("../../../downloads/sift/base.fvecs", &d2, &nb);

    //     pq_index->add(nb, xb);

    //     delete[] xt;
    //     delete[] xb;
    // }

    // {
    //     printf("[%.3f s] Loading train set\n", elapsed() - t0);

    //     size_t nt;
    //     float* xt = fvecs_read("../../..//dataset/TripClick/sift_learn.fvecs", &d, &nt);

    //     printf("[%.3f s] Preparing index \"%s\" d=%ld\n",
    //            elapsed() - t0,
    //            index_key,
    //            d);
    //     index = faiss::index_factory(d, index_key);

    //     printf("[%.3f s] Training on %ld vectors\n", elapsed() - t0, nt);

    //     index->train(nt, xt);
    //     delete[] xt;
    // }

    // {
    //     printf("[%.3f s] Loading database\n", elapsed() - t0);

    //     size_t nb, d2;
    //     float* xb = fvecs_read("../../../downloads/sift/base.fvecs", &d2, &nb);
    //     d = d2;
    //     assert(d == d2 || !"dataset does not have same dimension as train set");

    //     printf("[%.3f s] Indexing database, size %ld*%ld\n",
    //            elapsed() - t0,
    //            nb,
    //            d);

    //     index = faiss::index_factory(d, index_key);

    //     index->add(nb, xb);

    //     delete[] xb;
    // }

    index = faiss::read_index("../../../downloads/sift/index/hnsw_32.index", 0);
    pq_index = faiss::read_index("../../../downloads/sift/index/pq_8.index", 0);
    d = 128;

    ((faiss::IndexHNSW*)index)->set_quantize_storage(pq_index);

    // faiss::write_index(index, "../../../downloads/sift/index/hnsw_32.index");
    // faiss::write_index(pq_index, "../../../downloads/sift/index/pq_4.index");
    // return 0;

    // {
    //     // Create a delta compression dataset

    //     int level = 0;

    //     for(int v0 = 0; v0 < 10000; v0++){
    //         size_t begin, end;
    //         ((faiss::IndexHNSW*)index)->hnsw.neighbor_range(v0, level, &begin, &end);

    //         size_t nb, d2;
    //         float* xb = fvecs_read("../../../downloads/sift/base.fvecs", &d2, &nb);


    //     }
    // }

    size_t nq;
    float* xq;

    {
        printf("[%.3f s] Loading queries\n", elapsed() - t0);

        size_t d2;
        xq = fvecs_read("../../..//downloads/sift/query.fvecs", &d2, &nq);
        assert(d == d2 || !"query does not have same dimension as train set");
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
        int* gt_int = ivecs_read("../../..//downloads/sift/gt_10.ivecs", &k, &nq2);
        assert(nq2 == nq || !"incorrect nb of ground truth entries");

        gt = new faiss::idx_t[k * nq];
        for (int i = 0; i < k * nq; i++) {
            gt[i] = gt_int[i];
        }
        delete[] gt_int;
    }

    // {
    //     // We only need k = 10
    //     int k0 = 10;
    //     faiss::idx_t* gt0 = new faiss::idx_t[k0 * nq];
    //     for(int i = 0; i < nq; i++){
    //         for(int j = 0; j < k0; j++){
    //             gt0[i*k0 + j] = gt[i*k + j];
    //         }
    //     }

    //     k = k0;
    //     gt = gt0;
    // }

    // Result of the auto-tuning
    std::string selected_params;

    // if (TUNING) { // run auto-tuning

    //     printf("[%.3f s] Preparing auto-tune criterion 1-recall at 1 "
    //            "criterion, with k=%ld nq=%ld\n",
    //            elapsed() - t0,
    //            k,
    //            nq);

    //     faiss::OneRecallAtRCriterion crit(nq, 1);
    //     crit.set_groundtruth(k, nullptr, gt);
    //     crit.nnn = k; // by default, the criterion will request only 1 NN

    //     printf("[%.3f s] Preparing auto-tune parameters\n", elapsed() - t0);

    //     faiss::ParameterSpace params;
    //     params.initialize(index);

    //     printf("[%.3f s] Auto-tuning over %ld parameters (%ld combinations)\n",
    //            elapsed() - t0,
    //            params.parameter_ranges.size(),
    //            params.n_combinations());

    //     faiss::OperatingPoints ops;
    //     params.explore(index, nq, xq, crit, &ops);

    //     printf("[%.3f s] Found the following operating points: \n",
    //            elapsed() - t0);

    //     ops.display();

    //     // keep the first parameter that obtains > 0.5 1-recall@1
    //     for (int i = 0; i < ops.optimal_pts.size(); i++) {
    //         if (ops.optimal_pts[i].perf > 0.5) {
    //             selected_params = ops.optimal_pts[i].key;
    //             break;
    //         }
    //     }
    //     assert(selected_params.size() >= 0 ||
    //            !"could not find good enough op point");
    // }

    // ((faiss::IndexHNSW* )index)->hnsw.test();

    { // Use the found configuration to perform a search

        // faiss::ParameterSpace params;

        // printf("[%.3f s] Setting parameter configuration \"%s\" on index\n",
        //        elapsed() - t0,
        //        selected_params.c_str());

        // params.set_index_parameters(index, selected_params.c_str());

        // output buffers

        for(int efs = 32; efs <= 32; efs*=2){

            faiss::idx_t* I = new faiss::idx_t[nq * k];
            float* D = new float[nq * k];

            faiss::SearchParametersHNSW* params = new faiss::SearchParametersHNSW();

            printf("[%.3f s] Perform a search on %ld queries with efs %d k %ld\n",
               elapsed() - t0,
               nq,
               efs,
               k);

            params->efSearch = efs;

            index->search(nq, xq, k, D, I, params);

            printf("[%.3f s] Compute recalls\n", elapsed() - t0);

            compute_recall(
                gt,
                k,
                I,
                nq,
                k
            );

            delete[] I;
            delete[] D;
        }

        

        // evaluate result by hand.
        // int n_1 = 0, n_10 = 0, n_100 = 0;
        // for (int i = 0; i < nq; i++) {
        //     int gt_nn = gt[i * k];
        //     for (int j = 0; j < k; j++) {
        //         if (I[i * k + j] == gt_nn) {
        //             if (j < 1)
        //                 n_1++;
        //             if (j < 10)
        //                 n_10++;
        //             if (j < 100)
        //                 n_100++;
        //         }
        //     }
        // }
        // printf("R@1 = %.4f\n", n_1 / float(nq));
        // printf("R@10 = %.4f\n", n_10 / float(nq));
        // printf("R@100 = %.4f\n", n_100 / float(nq));
    }

    // write_index(index, "./msmarco_hnsw.index");

    delete[] xq;
    delete[] gt;
    delete index;
    return 0;
}
