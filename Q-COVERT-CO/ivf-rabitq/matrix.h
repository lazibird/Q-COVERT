#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

enum class HeaderBinElementType {
    Uint8,
    Float32,
};

template <typename T>
struct Matrix {
    size_t n = 0;
    size_t d = 0;
    std::vector<T> values;

    Matrix() = default;

    Matrix(size_t rows, size_t dim) : n(rows), d(dim), values(rows * dim) {}

    explicit Matrix(const std::string& path) {
        read_vecs(path);
    }

    T* row(size_t i) {
        return values.data() + i * d;
    }

    const T* row(size_t i) const {
        return values.data() + i * d;
    }

    T* data() {
        return values.data();
    }

    const T* data() const {
        return values.data();
    }

    bool empty() const {
        return n == 0;
    }

    void read_vecs(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("cannot open vector file: " + path);
        }

        uint32_t dim = 0;
        in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
        if (!in || dim == 0) {
            throw std::runtime_error("invalid vector file header: " + path);
        }

        in.seekg(0, std::ios::end);
        const std::streamoff bytes = in.tellg();
        const size_t record_bytes = sizeof(uint32_t) + static_cast<size_t>(dim) * sizeof(T);
        if (bytes < 0 || static_cast<size_t>(bytes) % record_bytes != 0) {
            throw std::runtime_error("non-uniform vector file: " + path);
        }

        n = static_cast<size_t>(bytes) / record_bytes;
        d = dim;
        values.assign(n * d, T{});

        in.seekg(0, std::ios::beg);
        for (size_t i = 0; i < n; ++i) {
            uint32_t cur_dim = 0;
            in.read(reinterpret_cast<char*>(&cur_dim), sizeof(cur_dim));
            if (!in || cur_dim != dim) {
                throw std::runtime_error("non-uniform vector dimension in: " + path);
            }
            in.read(reinterpret_cast<char*>(row(i)), static_cast<std::streamsize>(d * sizeof(T)));
            if (!in) {
                throw std::runtime_error("short read in vector file: " + path);
            }
        }
    }

    void write_vecs(const std::string& path) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot write vector file: " + path);
        }
        const uint32_t dim = static_cast<uint32_t>(d);
        for (size_t i = 0; i < n; ++i) {
            out.write(reinterpret_cast<const char*>(&dim), sizeof(dim));
            out.write(reinterpret_cast<const char*>(row(i)), static_cast<std::streamsize>(d * sizeof(T)));
        }
        if (!out) {
            throw std::runtime_error("failed writing vector file: " + path);
        }
    }
};

inline bool matrix_file_exists(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return in.good();
}

inline uint64_t matrix_file_size(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        throw std::runtime_error("cannot open file: " + path);
    }
    const std::streamoff bytes = in.tellg();
    if (bytes < 0) {
        throw std::runtime_error("cannot determine file size: " + path);
    }
    return static_cast<uint64_t>(bytes);
}

inline HeaderBinElementType detect_header_bin_element_type(
        const std::string& path,
        uint32_t& rows,
        uint32_t& dim) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open header bin file: " + path);
    }

    rows = 0;
    dim = 0;
    in.read(reinterpret_cast<char*>(&rows), sizeof(rows));
    in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    if (!in || rows == 0 || dim == 0) {
        throw std::runtime_error("invalid header bin file: " + path);
    }

    const uint64_t actual = matrix_file_size(path);
    const uint64_t u8_expected =
        8ULL + static_cast<uint64_t>(rows) * static_cast<uint64_t>(dim);
    const uint64_t f32_expected =
        8ULL + static_cast<uint64_t>(rows) * static_cast<uint64_t>(dim) * sizeof(float);
    if (actual == u8_expected) {
        return HeaderBinElementType::Uint8;
    }
    if (actual == f32_expected) {
        return HeaderBinElementType::Float32;
    }
    throw std::runtime_error("header bin size mismatch: " + path);
}

inline Matrix<float> read_bigann_u8bin_as_float(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open BigANN uint8 bin file: " + path);
    }

    uint32_t rows = 0;
    uint32_t dim = 0;
    in.read(reinterpret_cast<char*>(&rows), sizeof(rows));
    in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    if (!in || rows == 0 || dim == 0) {
        throw std::runtime_error("invalid BigANN uint8 bin header: " + path);
    }

    const uint64_t expected =
        8ULL + static_cast<uint64_t>(rows) * static_cast<uint64_t>(dim);
    if (matrix_file_size(path) != expected) {
        throw std::runtime_error("BigANN uint8 bin size mismatch: " + path);
    }

    Matrix<float> out(rows, dim);
    constexpr size_t kChunkRows = 65536;
    std::vector<uint8_t> buffer;
    for (uint32_t start = 0; start < rows; start += kChunkRows) {
        const uint32_t count = std::min<uint32_t>(kChunkRows, rows - start);
        buffer.resize(static_cast<size_t>(count) * dim);
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        if (!in) {
            throw std::runtime_error("short read in BigANN uint8 bin file: " + path);
        }
        float* dst = out.row(start);
        for (size_t i = 0; i < buffer.size(); ++i) {
            dst[i] = static_cast<float>(buffer[i]);
        }
    }
    return out;
}

inline Matrix<float> read_header_bin_as_float(const std::string& path) {
    uint32_t rows = 0;
    uint32_t dim = 0;
    const HeaderBinElementType type = detect_header_bin_element_type(path, rows, dim);
    if (type == HeaderBinElementType::Uint8) {
        return read_bigann_u8bin_as_float(path);
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open float32 header bin file: " + path);
    }
    in.seekg(8, std::ios::beg);
    Matrix<float> out(rows, dim);
    in.read(
        reinterpret_cast<char*>(out.values.data()),
        static_cast<std::streamsize>(out.values.size() * sizeof(float)));
    if (!in) {
        throw std::runtime_error("short read in float32 header bin file: " + path);
    }
    return out;
}

inline Matrix<uint8_t> read_bigann_u8bin_as_uint8(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open BigANN uint8 bin file: " + path);
    }

    uint32_t rows = 0;
    uint32_t dim = 0;
    in.read(reinterpret_cast<char*>(&rows), sizeof(rows));
    in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    if (!in || rows == 0 || dim == 0) {
        throw std::runtime_error("invalid BigANN uint8 bin header: " + path);
    }

    const uint64_t expected =
        8ULL + static_cast<uint64_t>(rows) * static_cast<uint64_t>(dim);
    if (matrix_file_size(path) != expected) {
        throw std::runtime_error("BigANN uint8 bin size mismatch: " + path);
    }

    Matrix<uint8_t> out(rows, dim);
    in.read(
        reinterpret_cast<char*>(out.values.data()),
        static_cast<std::streamsize>(out.values.size() * sizeof(uint8_t)));
    if (!in) {
        throw std::runtime_error("short read in BigANN uint8 bin file: " + path);
    }
    return out;
}

inline bool header_bin_is_uint8(const std::string& path) {
    uint32_t rows = 0;
    uint32_t dim = 0;
    return detect_header_bin_element_type(path, rows, dim) == HeaderBinElementType::Uint8;
}

inline Matrix<uint32_t> read_bigann_u32bin_matrix(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open BigANN uint32 bin file: " + path);
    }

    uint32_t rows = 0;
    uint32_t dim = 0;
    in.read(reinterpret_cast<char*>(&rows), sizeof(rows));
    in.read(reinterpret_cast<char*>(&dim), sizeof(dim));
    if (!in || rows == 0 || dim == 0) {
        throw std::runtime_error("invalid BigANN uint32 bin header: " + path);
    }

    const uint64_t expected =
        8ULL + static_cast<uint64_t>(rows) * static_cast<uint64_t>(dim) * sizeof(uint32_t);
    if (matrix_file_size(path) != expected) {
        throw std::runtime_error("BigANN uint32 bin size mismatch: " + path);
    }

    Matrix<uint32_t> out(rows, dim);
    in.read(
        reinterpret_cast<char*>(out.values.data()),
        static_cast<std::streamsize>(out.values.size() * sizeof(uint32_t)));
    if (!in) {
        throw std::runtime_error("short read in BigANN uint32 bin file: " + path);
    }
    return out;
}

// Read the selected file explicitly; never silently prefer another dataset copy.
inline Matrix<float> read_float_vectors_file(const std::string& path) {
    return path.size() >= 4 && path.substr(path.size() - 4) == ".bin"
        ? read_header_bin_as_float(path) : Matrix<float>(path);
}
inline Matrix<uint32_t> read_uint32_vectors_file(const std::string& path) {
    return path.size() >= 4 && path.substr(path.size() - 4) == ".bin"
        ? read_bigann_u32bin_matrix(path) : Matrix<uint32_t>(path);
}

inline Matrix<float> read_float_vectors_auto(const std::string& vecs_path,
                                             const std::string& u8bin_path) {
    if (matrix_file_exists(vecs_path)) {
        return Matrix<float>(vecs_path);
    }
    if (matrix_file_exists(u8bin_path)) {
        return read_header_bin_as_float(u8bin_path);
    }
    throw std::runtime_error("cannot find float vector file: " + vecs_path + " or " + u8bin_path);
}

inline Matrix<uint32_t> read_uint32_vectors_auto(const std::string& vecs_path,
                                                 const std::string& u32bin_path) {
    if (matrix_file_exists(vecs_path)) {
        return Matrix<uint32_t>(vecs_path);
    }
    if (matrix_file_exists(u32bin_path)) {
        return read_bigann_u32bin_matrix(u32bin_path);
    }
    throw std::runtime_error("cannot find uint32 vector file: " + vecs_path + " or " + u32bin_path);
}

inline Matrix<float> multiply_padded_left(const Matrix<float>& left,
                                          size_t padded_left_dim,
                                          const Matrix<float>& right) {
    if (right.n != padded_left_dim) {
        throw std::runtime_error("multiply_padded_left: incompatible dimensions");
    }
    Matrix<float> out(left.n, right.d);
    for (size_t i = 0; i < left.n; ++i) {
        for (size_t j = 0; j < right.d; ++j) {
            float acc = 0.0f;
            for (size_t k = 0; k < left.d; ++k) {
                acc += left.row(i)[k] * right.row(k)[j];
            }
            out.row(i)[j] = acc;
        }
    }
    return out;
}
