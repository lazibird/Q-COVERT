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
#include <vector>
#include <algorithm>
#include <map>
#include <iostream>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <sys/time.h>

#include <faiss/Clustering.h>
#include <faiss/IndexFlat.h>
#include <faiss/IndexHNSW.h>
#include <faiss/index_factory.h>
#include <faiss/utils/distances.h>
#include <faiss/utils/random.h>
#include <faiss/index_io.h>

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

// float tiptoe_clustering(
//         size_t d,
//         size_t n,
//         size_t k,
//         const float* x,
//         float* centroids) {
//     faiss::Clustering clus(d, k);
//     clus.nredo = 3;
//     clus.niter = 5;
//     clus.verbose = true;
//     // display logs if > 1Gflop per iteration
//     faiss::IndexFlatL2 index(d);
//     index.metric_type = faiss::METRIC_INNER_PRODUCT;
//     clus.train(n, x, index); 
//     memcpy(centroids, clus.centroids.data(), sizeof(*centroids) * d * k);
//     return clus.iteration_stats.back().obj;
// }

std::vector<std::vector<float>> tiptoe_clustering_recursive(
        size_t dim,
        size_t nb,
        size_t n_centroids,
        const float* xb,
        size_t max_n_nodes) {

    std::cout << "-- tiptoe_clustering_recursive on: " << nb << " nodes to: " << n_centroids << " centroids" << std::endl;

    std::vector<std::vector<float>> centroids;

    // Compute centroids
    faiss::Clustering clus(dim, n_centroids);
    clus.nredo = 1;
    clus.spherical = true;
    clus.niter = 16;
    clus.verbose = true;
    // display logs if > 1Gflop per iteration
    faiss::IndexFlatL2 index(dim);
    clus.train(nb, xb, index); 
    // memcpy(centroids, clus.centroids.data(), sizeof(*centroids) * dim * n_centroids);

    // Get assignment
    std::map<size_t, std::vector<size_t>> assignment;
    faiss::IndexFlatL2 index_centroids(dim);
    index_centroids.add(n_centroids, clus.centroids.data());

    faiss::idx_t* i_b = new faiss::idx_t[nb];
    float* d_b = new float[nb];

    index_centroids.search(nb, xb, 1, d_b, i_b);

    for(size_t i = 0; i < nb; i++){
        assignment[i_b[i]].push_back(i);
    }

    // check size
    for(size_t nc = 0; nc < n_centroids; nc++){
        if(assignment[nc].size() > max_n_nodes){
            int new_n_centrods = ceil(assignment[nc].size() * 1.0 / max_n_nodes); 

            if(new_n_centrods < n_centroids){
                float* new_xb = new float[dim*assignment[nc].size()];

                for(size_t i = 0; i < assignment[nc].size(); i++){
                    size_t ni = assignment[nc][i];
                    memcpy(new_xb + i * dim, xb + ni * dim, dim*sizeof(float));
                }
                std::cout << " ## RECURSIVE." << std::endl;

                std::vector<std::vector<float>> new_centroids = tiptoe_clustering_recursive(
                    dim, 
                    assignment[nc].size(),
                    new_n_centrods,
                    new_xb,
                    max_n_nodes
                );

                for(auto c : new_centroids){
                    centroids.push_back(c);
                }

                std::cout << " ## RECURSIVE DONE." << std::endl;
            } else{
                std::vector<float> c(
                    clus.centroids.data() + nc*dim,
                    clus.centroids.data() + (nc + 1)*dim
                );
                centroids.push_back(c);
            }
        } else{
            std::vector<float> c(
                clus.centroids.data() + nc*dim,
                clus.centroids.data() + (nc + 1)*dim
            );
            centroids.push_back(c);
        }
    }
    
    return centroids;
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

int main() {
    
    size_t nb;
    size_t d;
    
    size_t n_centroids = 3500;
    size_t max_nodes_per_centroids = 4000;

    
    float* xb = fvecs_read("/home/clive/see/data/dataset/msmarco_bert/passages.fvecs", &d, &nb);

    // Loading Queries
    std::cout << "Loading queries..." << std::endl;
    size_t nq;
    float* xq;
    size_t d2;
    int n_result = 10;

    xq = fvecs_read("/home/clive/see/data/dataset/msmarco_bert/queries.fvecs", &d2, &nq);

    std::cout << "Clustering data: " << std::endl;
    std::cout << "- d: " << d << std::endl;
    std::cout << "- n: " << nb << std::endl;
    std::cout << "- nc: " << n_centroids << std::endl;

    // tiptoe_clustering(
    //     d,
    //     nb,
    //     n_centroids,
    //     xb,
    //     centroids
    // );

    std::vector<std::vector<float>> cs = tiptoe_clustering_recursive(
        d,
        nb,
        n_centroids,
        xb,
        max_nodes_per_centroids
    );

    n_centroids = cs.size();
    float* centroids = new float[d*n_centroids];

    for(int i = 0; i < n_centroids; i++){
        memcpy(centroids + i*d, cs[i].data(), d*sizeof(float));
    }
    std::cout << "New n_centrods: " << n_centroids << std::endl;

    std::cout << "Building flat index..." << std::endl;

    const char* index_key = "Flat";
    faiss::Index* index = faiss::index_factory(d, index_key);
    index->add(n_centroids, centroids);

    faiss::write_index(index, "/home/clive/see/data/dataset/msmarco_bert/tiptoe_centroids.index");

    int n_dup_cluster = 2;
    faiss::idx_t* i_b = new faiss::idx_t[nb * n_dup_cluster];
    float* d_b = new float[nb * n_dup_cluster];


    std::cout << "Searching on flat index..." << std::endl;
    index->search(nb, xb, n_dup_cluster, d_b, i_b);

    std::cout << "Sorting diff..." << std::endl;
    std::vector<float> D_diff;

    for(int i = 0; i < nb; i++){
        D_diff.push_back(d_b[2*i + 1] - d_b[2*i]);
    }

    for(int i = 0; i < 10; i++){
        std::cout << D_diff[i] << " "; 
    }
    std::cout << std::endl;

    std::sort(D_diff.begin(), D_diff.end(), [](float a, float b) {
        return a < b;
    });

    for(int i = 0; i < 10; i++){
        std::cout << D_diff[i] << " "; 
    }
    std::cout << std::endl;

    std::cout << "Sorting diff..." << std::endl;
    int cutoff = 0.2*nb;
    float cutoff_d = D_diff[cutoff];
    std::cout << "Cut off diff: " << cutoff_d << std::endl;

    std::map<int, std::vector<int>> assignment;

    int cnt = 0;
    for(int i = 0; i < nb; i++){
        int centroid_1 = i_b[2*i];
        int centroid_2 = i_b[2*i + 1];
        assignment[centroid_1].push_back(i);
        if(D_diff[i] < cutoff_d){
            assignment[centroid_2].push_back(i);
            cnt ++;
        }
    }

    // Constructing Index for each clusters
    std::map<std::pair<int, int>, int> reverse_map;
    std::cout << "Building index for each cluster..." << std::endl;
    std::vector<faiss::Index*> clusters;
    const char* flat_key = "Flat";
    int max_cnt = 0;
    for(int cid = 0; cid < n_centroids; cid++){
        faiss::Index* index = faiss::index_factory(d, flat_key);
        if(assignment.find(cid) == assignment.end()){
            assert(0);
        }
        int cnt = 0;
        for(auto nid : assignment[cid]){
            index->add(1, xb + nid*d);
            reverse_map[std::make_pair(cid, cnt)] = nid;
            cnt++;
        }
        std::cout << cnt << " nodes added to cluster "<< cid << std::endl;
        max_cnt = cnt > max_cnt ? cnt : max_cnt;
        clusters.push_back(index);
    }

    std::cout << "Largest cluster contains "<< max_cnt << " nodes." << std::endl;

    faiss::idx_t* I = new faiss::idx_t[nq*n_result];
    float* D = new float[nq*n_result];
    
    faiss::idx_t* i_q = new faiss::idx_t[nq];
    float* d_q = new float[nq];
    index->search(nq, xq, 1, d_q, i_q);

    std::cout << "Searching..." << std::endl;
    for(int qid = 0; qid < nq; qid++){
        int cid = i_q[qid];
        // std::cout << qid << " query to cluster " << cid << std::endl;
        clusters[cid]->search(
            1, 
            xq + qid*d, 
            n_result, 
            D + qid*n_result, 
            I + qid*n_result
        );
    }

    for(int qid = 0; qid < nq; qid++){
        int cid = i_q[qid];
        for(int rid = 0; rid< n_result; rid++){
            I[qid*n_result + rid] = reverse_map[std::make_pair(cid, I[qid*n_result + rid])];
        }
    }

    size_t k;         // nb of results per query in the GT
    faiss::idx_t* gt; // nq * k matrix of ground-truth nearest-neighbors

    std::cout << "Loading GT..." << std::endl;
    {

        // load ground-truth and convert int to long
        size_t nq2;
        int* gt_int = ivecs_read("/home/clive/see/data/dataset/msmarco_bert/gt_10.ivecs", &k, &nq2);
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

    std::cout << "Computing Recall..." << std::endl;
    compute_recall(
        gt,
        k,
        I,
        nq,
        k
    );

    int* result = new int[nq * k];
    for(int i = 0; i < nq*k; i++){
        result[i] = I[i];
    }

    ivecs_write("/home/clive/see/data/dataset/msmarco_bert/tiptoe_msmarco.ivecs", result, k, nq);

    return 0;
}
