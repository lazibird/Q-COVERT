#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

float* pca_read(const char* fname, size_t* d_out, size_t* n_out, std::vector<int> &id_list);

float* fvecs_read(const char* fname, size_t* d_out, size_t* n_out);
float* fvecs_read_prefix(const char* fname, size_t max_n, size_t* d_out, size_t* n_out);
void fvecs_write(const char* fname, float* data, size_t d, size_t n);

int* ivecs_read(const char* fname, size_t* d_out, size_t* n_out);
void ivecs_write(const char* fname, int* data, size_t d, size_t n);
