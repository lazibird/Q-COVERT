#include "graph.h"
#include "faiss/impl/HNSW.h"
#include "faiss/utils/io.h"
#include "faiss/index_io.h"


// Naive cluster
// void naive_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb);

// Fully naive cluster
void fully_naive_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb, vector<uint8_t> codes);

// void fully_duplicate_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb);

// Random cluster
// void random_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb);

// KaHIP cluster
// void kahip_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb, char* fname);

// void kahip_cluster_with_duplication(
//     BlockFetcher* bf, 
//     faiss::IndexHNSW* index, 
//     float* xb, 
//     const char* fname);

// Last level cluster
// void LL_cluster(BlockFetcher* bf, faiss::IndexHNSW* index, float* xb);

void to_metis_graph_weightless(faiss::IndexHNSW* index);

