#include "ProductQuantizer.h"
#include "FaissAssert.h"

#include <iostream>
#include <cassert>

using namespace std;


namespace faiss {

void ProductQuantizer::set_derived_values() {
    // quite a few derived values
    FAISS_THROW_IF_NOT_MSG(
            d % M == 0,
            "The dimension of the vector (d) should be a multiple of the number of subquantizers (M)");
    dsub = d / M;
    code_size = (nbits * M + 7) / 8;
    ksub = 1 << nbits;
    centroids.resize(d * ksub);
}

void ProductQuantizer::print_stats() {
    cout << "> PQ stats: " << endl;
    cout << " -> d: " << d << endl;
    cout << " -> code_size: " << code_size << endl;
    cout << " -> M: " << M << endl;
    cout << " -> nbits: " << nbits << endl;
    cout << " -> dsub: " << dsub << endl;
    cout << " -> ksub: " << ksub << endl;

    cout << " -> sizeof(centroids): " << centroids.size() << endl;
    cout << " -> sizeof(codes): " << codes.size() << endl;

}

float fvec_L2sqr(const float* x, const float* y, size_t d) {
    size_t i;
    float res = 0;
    for (i = 0; i < d; i++) {
        const float tmp = x[i] - y[i];
        res += tmp * tmp;
    }
    return res;
}

float fvec_neg_inner_product(const float* x, const float* y, size_t d) {
    float res = 0.F;
    for (size_t i = 0; i != d; ++i) {
        res += x[i] * y[i];
    }
    return -res;
}

void fvec_L2sqr_ny(
        float* dis,
        const float* x,
        const float* y,
        size_t d,
        size_t ny) {
    for (size_t i = 0; i < ny; i++) {
        dis[i] = fvec_L2sqr(x, y, d);
        y += d;
    }
}

void fvec_ip_ny(
        float* dis,
        const float* x,
        const float* y,
        size_t d,
        size_t ny) {
    for (size_t i = 0; i < ny; i++) {
        dis[i] = fvec_neg_inner_product(x, y, d);
        y += d;
        // if(dis[i] >= 1 || dis[i] <= -1){
        //     cout << dis[i] << " " << endl;
        //     assert(0);
        // }
    }
}


void ProductQuantizer::compute_distance_table(const float* x, float* dis_table)
        const { 
        // use regular version
        if(metric == METRIC_L2){
            for (size_t m = 0; m < M; m++) {
                fvec_L2sqr_ny(
                        dis_table + m * ksub,
                        x + m * dsub,
                        get_centroids(m, 0),
                        dsub,
                        ksub);
            }
        } else if (metric == METRIC_INNER_PRODUCT){
            for (size_t m = 0; m < M; m++) {
                fvec_ip_ny(
                        dis_table + m * ksub,
                        x + m * dsub,
                        get_centroids(m, 0),
                        dsub,
                        ksub);
            }
        } else{
            assert(0);
        }
        
}

}