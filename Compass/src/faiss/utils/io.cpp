#include "io.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace {

bool has_suffix(const char* fname, const char* suffix) {
    const std::string name(fname);
    const std::string suff(suffix);
    return name.size() >= suff.size() &&
           name.compare(name.size() - suff.size(), suff.size(), suff) == 0;
}

void checked_fread(void* ptr, size_t size, size_t count, FILE* f, const char* fname) {
    size_t nr = fread(ptr, size, count, f);
    if (nr != count) {
        fprintf(stderr, "could not read whole file %s\n", fname);
        abort();
    }
}

size_t file_size(FILE* f) {
    struct stat st;
    fstat(fileno(f), &st);
    return st.st_size;
}

size_t checked_matrix_count(uint32_t n, uint32_t d, const char* fname) {
    if (n == 0 || d == 0 || d >= 1000000) {
        fprintf(stderr, "%s has invalid BigANN matrix header: n=%u d=%u\n", fname, n, d);
        abort();
    }
    const uint64_t count = static_cast<uint64_t>(n) * static_cast<uint64_t>(d);
    if (count > std::numeric_limits<size_t>::max()) {
        fprintf(stderr, "%s BigANN matrix is too large for this platform\n", fname);
        abort();
    }
    return static_cast<size_t>(count);
}

float* allocate_float_matrix(size_t count, const char* fname) {
    float* x = new (std::nothrow) float[count];
    if (!x) {
        fprintf(
                stderr,
                "could not allocate %.2f GiB while reading %s\n",
                static_cast<double>(count) * sizeof(float) / (1024.0 * 1024.0 * 1024.0),
                fname);
        abort();
    }
    return x;
}

struct BigannFloatLayout {
    uint32_t header_n;
    uint32_t d;
    size_t available_n;
    size_t element_size;
};

BigannFloatLayout read_bigann_float_layout(FILE* f, const char* fname) {
    uint32_t n32 = 0;
    uint32_t d32 = 0;
    checked_fread(&n32, sizeof(uint32_t), 1, f, fname);
    checked_fread(&d32, sizeof(uint32_t), 1, f, fname);
    checked_matrix_count(n32, d32, fname);

    const size_t sz = file_size(f);
    const size_t payload_size = sz >= 2 * sizeof(uint32_t) ? sz - 2 * sizeof(uint32_t) : 0;
    const uint64_t header_count = static_cast<uint64_t>(n32) * static_cast<uint64_t>(d32);
    const uint64_t header_u8_payload = header_count * sizeof(uint8_t);
    const uint64_t header_float_payload = header_count * sizeof(float);

    if (payload_size == header_u8_payload) {
        return {n32, d32, n32, sizeof(uint8_t)};
    }
    if (payload_size == header_float_payload) {
        return {n32, d32, n32, sizeof(float)};
    }
    if (payload_size % d32 == 0) {
        return {n32, d32, payload_size / d32, sizeof(uint8_t)};
    }
    if (payload_size % (static_cast<size_t>(d32) * sizeof(float)) == 0) {
        return {
                n32,
                d32,
                payload_size / (static_cast<size_t>(d32) * sizeof(float)),
                sizeof(float)};
    }

    fprintf(
            stderr,
            "%s has unsupported BigANN payload size: got %zu bytes for header n=%u d=%u\n",
            fname,
            payload_size,
            n32,
            d32);
    abort();
}

float* bigann_fbin_read_prefix(const char* fname, size_t max_n, size_t* d_out, size_t* n_out) {
    FILE* f = fopen(fname, "rb");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }

    BigannFloatLayout layout = read_bigann_float_layout(f, fname);
    size_t rows = layout.available_n;
    if (max_n > 0) {
        if (max_n > layout.available_n) {
            fprintf(
                    stderr,
                    "%s has only %zu available rows, requested prefix %zu rows\n",
                    fname,
                    layout.available_n,
                    max_n);
            abort();
        }
        rows = max_n;
    }

    const size_t count = rows * static_cast<size_t>(layout.d);
    *d_out = layout.d;
    *n_out = rows;
    float* x = allocate_float_matrix(count, fname);

    if (layout.element_size == sizeof(uint8_t)) {
        const size_t chunk_items = 1 << 20;
        uint8_t* tmp = new uint8_t[chunk_items];
        size_t offset = 0;
        while (offset < count) {
            const size_t todo = std::min(chunk_items, count - offset);
            checked_fread(tmp, sizeof(uint8_t), todo, f, fname);
            for (size_t i = 0; i < todo; i++) {
                x[offset + i] = static_cast<float>(tmp[i]);
            }
            offset += todo;
        }
        delete[] tmp;
    } else {
        checked_fread(x, sizeof(float), count, f, fname);
    }

    fclose(f);
    return x;
}

int* bigann_ibin_read(const char* fname, size_t* d_out, size_t* n_out) {
    static_assert(sizeof(int) == sizeof(uint32_t), "ivecs_read assumes 32-bit int");

    FILE* f = fopen(fname, "rb");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }

    uint32_t n32 = 0;
    uint32_t d32 = 0;
    checked_fread(&n32, sizeof(uint32_t), 1, f, fname);
    checked_fread(&d32, sizeof(uint32_t), 1, f, fname);

    const size_t count = checked_matrix_count(n32, d32, fname);
    const size_t sz = file_size(f);
    const size_t payload_size = sz >= 2 * sizeof(uint32_t) ? sz - 2 * sizeof(uint32_t) : 0;
    const size_t expected_payload_size = count * sizeof(uint32_t);
    if (payload_size != expected_payload_size) {
        fprintf(
                stderr,
                "%s has unsupported BigANN int payload size: got %zu bytes, expected %zu bytes\n",
                fname,
                payload_size,
                expected_payload_size);
        abort();
    }

    *d_out = d32;
    *n_out = n32;
    int* x = new int[count];
    checked_fread(x, sizeof(uint32_t), count, f, fname);

    fclose(f);
    return x;
}

} // namespace

float* pca_read(const char* fname, size_t* d_out, size_t* n_out, std::vector<int> &id_list) {
    FILE* f = fopen(fname, "rb");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }
    int d;
    fread(&d, 1, sizeof(int), f);
    fread(&d, 1, sizeof(int), f);

    assert((d > 0 && d < 1000000) || !"unreasonable dimension");
    fseek(f, 0, SEEK_SET);
    size_t sz = file_size(f);

    // printf("file size: %ld\n", sz);
    assert(sz % ((d + 2) * 4) == 0 || !"weird file size");
    size_t n = sz / ((d + 2) * 4);

    *d_out = d;
    *n_out = n;
    float* x = new float[n * (d + 2)];
    size_t nr = fread(x, sizeof(float), n * (d + 2), f);
    assert(nr == n * (d + 2) || !"could not read whole file");

    // shift array to remove row headers
    for (size_t i = 0; i < n; i++){
        id_list.push_back(((int*)x)[i * (d + 2)]);
        memmove(x + i * d, x + 2 + i * (d + 2), d * sizeof(*x));
    }

    fclose(f);
    return x;
}

float* fvecs_read(const char* fname, size_t* d_out, size_t* n_out) {
    return fvecs_read_prefix(fname, 0, d_out, n_out);
}

float* fvecs_read_prefix(const char* fname, size_t max_n, size_t* d_out, size_t* n_out) {
    if (has_suffix(fname, ".bin")) {
        return bigann_fbin_read_prefix(fname, max_n, d_out, n_out);
    }

    FILE* f = fopen(fname, "rb");
    if (!f) {
        fprintf(stderr, "could not open %s\n", fname);
        perror("");
        abort();
    }
    int d;
    fread(&d, 1, sizeof(int), f);
    assert((d > 0 && d < 1000000) || !"unreasonable dimension");
    fseek(f, 0, SEEK_SET);
    size_t sz = file_size(f);
    assert(sz % ((d + 1) * 4) == 0 || !"weird file size");
    size_t n = sz / ((d + 1) * 4);
    size_t rows = max_n > 0 ? std::min(max_n, n) : n;
    if (max_n > n) {
        fprintf(
                stderr,
                "%s has only %zu rows, requested prefix %zu rows\n",
                fname,
                n,
                max_n);
        abort();
    }

    *d_out = d;
    *n_out = rows;
    float* x = allocate_float_matrix(rows * static_cast<size_t>(d), fname);
    for (size_t i = 0; i < rows; i++) {
        int row_d = 0;
        checked_fread(&row_d, sizeof(int), 1, f, fname);
        if (row_d != d) {
            fprintf(stderr, "%s row %zu has dim %d, expected %d\n", fname, i, row_d, d);
            abort();
        }
        checked_fread(x + i * d, sizeof(float), d, f, fname);
    }

    fclose(f);
    return x;
}

// not very clean, but works as long as sizeof(int) == sizeof(float)
int* ivecs_read(const char* fname, size_t* d_out, size_t* n_out) {
    if (has_suffix(fname, ".bin")) {
        return bigann_ibin_read(fname, d_out, n_out);
    }
    return (int*)fvecs_read(fname, d_out, n_out);
}

void fvecs_write(const char* fname, float* data, size_t d, size_t n) {
    FILE* f = fopen(fname, "wb");
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
