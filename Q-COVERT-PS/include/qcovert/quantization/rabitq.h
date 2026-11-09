#pragma once

// Standalone RaBitQ and RaBitQ+ for squared Euclidean distance. C++17 + bundled Eigen.
#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ivf_rabitq {

struct PreparedVector {
    std::vector<float> values;
    std::size_t dim = 0;
    std::uint32_t seed = 0;
};

namespace detail {

inline float dot(const std::vector<float>& a, const std::vector<float>& b) {
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
}

inline float safe_lower(float value, float x2, float y2) {
    return std::max(0.0f, value - 2e-4f * (1.0f + x2 + y2));
}

class Rotation {
public:
    using Matrix = Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

    explicit Rotation(std::size_t dim, std::uint32_t seed)
        : dim_(dim), padded_((dim + 63) / 64 * 64), seed_(seed), matrix_(padded_, padded_) {
        if (dim == 0 || padded_ < dim) throw std::invalid_argument("dimension must be positive");
        std::mt19937 rng(seed);
        std::normal_distribution<float> normal(0.0f, 1.0f);
        Matrix random = Matrix::NullaryExpr(padded_, padded_, [&]() { return normal(rng); });
        Eigen::HouseholderQR<Matrix> qr(random);
        matrix_ = qr.householderQ().transpose();
    }

    std::size_t dim() const { return dim_; }
    std::size_t padded_dim() const { return padded_; }
    const Matrix& matrix() const { return matrix_; }

    PreparedVector prepare(const float* vector) const {
        if (!vector) throw std::invalid_argument("null vector");
        Eigen::RowVectorXf padded = Eigen::RowVectorXf::Zero(padded_);
        for (std::size_t i = 0; i < dim_; ++i) {
            if (!std::isfinite(vector[i])) throw std::invalid_argument("vectors must be finite");
            padded[static_cast<Eigen::Index>(i)] = vector[i];
        }
        const Eigen::RowVectorXf rotated = padded * matrix_;
        return {{rotated.data(), rotated.data() + padded_}, dim_, seed_};
    }

    std::vector<float> residual(const PreparedVector& vector, const PreparedVector& centroid) const {
        if (vector.dim != dim_ || centroid.dim != dim_ || vector.seed != seed_ || centroid.seed != seed_ ||
            vector.values.size() != padded_ || centroid.values.size() != padded_)
            throw std::invalid_argument("prepared vector uses a different rotation");
        std::vector<float> out(padded_);
        for (std::size_t i = 0; i < padded_; ++i) out[i] = vector.values[i] - centroid.values[i];
        return out;
    }

    std::vector<float> residual(const float* vector, const float* centroid) const {
        if (!vector || !centroid) throw std::invalid_argument("null vector or centroid");
        Eigen::RowVectorXf delta = Eigen::RowVectorXf::Zero(padded_);
        for (std::size_t i = 0; i < dim_; ++i) {
            if (!std::isfinite(vector[i]) || !std::isfinite(centroid[i]))
                throw std::invalid_argument("vectors must be finite");
            delta[static_cast<Eigen::Index>(i)] = static_cast<float>(vector[i]) - centroid[i];
        }
        const Eigen::RowVectorXf rotated = delta * matrix_;
        return {rotated.data(), rotated.data() + padded_};
    }

private:
    std::size_t dim_;
    std::size_t padded_;
    std::uint32_t seed_;
    Matrix matrix_;
};

inline bool bit(const std::vector<std::uint64_t>& words, std::size_t i) {
    return (words[i / 64] >> (i % 64)) & 1u;
}

inline void set_bit(std::vector<std::uint64_t>& words, std::size_t i) {
    words[i / 64] |= std::uint64_t{1} << (i % 64);
}

}  // namespace detail

struct BinaryCode {
    std::vector<std::uint64_t> signs;
    float norm = 0.0f;
    float x0 = 0.0f;  // cosine between residual and sign vector / sqrt(B)
};

struct BinaryQuery {
    std::vector<std::uint8_t> codes;
    std::vector<float> residual;  // needed for the geometric lower-bound formula
    float lo = 0.0f;
    float width = 0.0f;
    float norm2 = 0.0f;
};

class RabitQ {
public:
    explicit RabitQ(std::size_t dim, unsigned query_bits = 4, std::uint32_t seed = 42)
        : rotation_(dim, seed), query_bits_(query_bits) {
        if (query_bits < 1 || query_bits > 8) throw std::invalid_argument("query_bits must be in [1, 8]");
        std::mt19937 rng(1234567);
        std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
        offsets_.resize(rotation_.padded_dim());
        for (float& offset : offsets_) offset = uniform(rng);
    }

    std::size_t dim() const { return rotation_.dim(); }
    std::size_t padded_dim() const { return rotation_.padded_dim(); }
    const detail::Rotation::Matrix& rotation_matrix() const { return rotation_.matrix(); }
    const std::vector<float>& query_offsets() const { return offsets_; }
    PreparedVector prepare(const float* vector) const { return rotation_.prepare(vector); }

    BinaryCode quantize_vector(const float* vector, const float* centroid) const {
        const auto r = rotation_.residual(vector, centroid);
        BinaryCode out;
        out.signs.assign(padded_dim() / 64, 0);
        float signed_sum = 0.0f;
        for (std::size_t i = 0; i < padded_dim(); ++i) {
            if (r[i] > 0) detail::set_bit(out.signs, i);
            signed_sum += std::abs(r[i]);
        }
        out.norm = std::sqrt(detail::dot(r, r));
        out.x0 = out.norm ? signed_sum / (out.norm * std::sqrt(float(padded_dim()))) : 0.0f;
        return out;
    }

    std::vector<BinaryCode> quantize_cluster(const float* vectors, std::size_t count, const float* centroid) const {
        if (count && !vectors) throw std::invalid_argument("null vectors");
        std::vector<BinaryCode> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) result.push_back(quantize_vector(vectors + i * dim(), centroid));
        return result;
    }

    BinaryQuery quantize_query(const float* query, const float* centroid) const {
        return quantize_residual(rotation_.residual(query, centroid));
    }

    BinaryQuery quantize_query(const PreparedVector& query, const PreparedVector& centroid) const {
        return quantize_residual(rotation_.residual(query, centroid));
    }

private:
    BinaryQuery quantize_residual(std::vector<float> residual) const {
        BinaryQuery out;
        out.residual = std::move(residual);
        out.norm2 = detail::dot(out.residual, out.residual);
        const auto range = std::minmax_element(out.residual.begin(), out.residual.end());
        out.lo = *range.first;
        out.width = (*range.second - out.lo) / float((1u << query_bits_) - 1u);
        out.codes.resize(padded_dim(), 0);
        if (out.width > 0) {
            const float max_code = float((1u << query_bits_) - 1u);
            for (std::size_t i = 0; i < padded_dim(); ++i) {
                float code = std::floor((out.residual[i] - out.lo) / out.width + offsets_[i]);
                out.codes[i] = static_cast<std::uint8_t>(std::max(0.0f, std::min(max_code, code)));
            }
        }
        return out;
    }

public:
    float approximate_distance(const BinaryQuery& query, const BinaryCode& vector) const {
        check(query, vector);
        if (vector.norm == 0) return query.norm2;
        float sign_sum = 0.0f, code_sum = 0.0f;
        for (std::size_t i = 0; i < padded_dim(); ++i) {
            const float sign = detail::bit(vector.signs, i) ? 1.0f : -1.0f;
            sign_sum += sign;
            code_sum += sign * query.codes[i];
        }
        const float dot_estimate = (query.lo * sign_sum + query.width * code_sum) / std::sqrt(float(padded_dim()));
        return vector.norm * vector.norm + query.norm2 - 2 * vector.norm * dot_estimate / vector.x0;
    }

    // Geometric lower-bound formula with a float-rounding margin; uses the residual.
    float lower_bound(const BinaryQuery& query, const BinaryCode& vector) const {
        check(query, vector);
        if (vector.norm == 0) return query.norm2;
        float qb = 0.0f;
        for (std::size_t i = 0; i < padded_dim(); ++i)
            qb += (detail::bit(vector.signs, i) ? 1.0f : -1.0f) * query.residual[i];
        qb /= std::sqrt(float(padded_dim()));
        const float alpha = vector.norm * vector.x0;
        const float error = vector.norm * std::sqrt(std::max(0.0f, 1 - vector.x0 * vector.x0));
        const float q_orth = std::sqrt(std::max(0.0f, query.norm2 - qb * qb));
        const float x2 = vector.norm * vector.norm;
        return detail::safe_lower(query.norm2 + x2 - 2 * (alpha * qb + error * q_orth), x2, query.norm2);
    }

    // Original project's statistical pruning score. It is not a guaranteed lower bound.
    float statistical_lower_bound(const BinaryQuery& query, const BinaryCode& vector) const {
        check(query, vector);
        if (vector.norm == 0) return query.norm2;
        const float scaled = vector.norm / vector.x0;
        const float error = 2 * (1.9f / std::sqrt(float(padded_dim() - 1))) *
                             std::sqrt(std::max(0.0f, scaled * scaled - vector.norm * vector.norm));
        return approximate_distance(query, vector) - std::sqrt(query.norm2) * error;
    }

private:
    void check(const BinaryQuery& query, const BinaryCode& vector) const {
        if (query.codes.size() != padded_dim() || query.residual.size() != padded_dim() ||
            vector.signs.size() != padded_dim() / 64)
            throw std::invalid_argument("code dimension mismatch");
    }

    detail::Rotation rotation_;
    unsigned query_bits_;
    std::vector<float> offsets_;
};

struct PlusCode {
    std::vector<std::uint8_t> code;  // one byte per padded coordinate, even when bits < 8
    std::vector<std::uint64_t> signs;
    float norm2 = 0.0f;
    float xipnorm = 0.0f;
};

struct PlusQuery {
    std::vector<float> residual;
    float norm2 = 0.0f;
};

class RabitQPlus {
public:
    explicit RabitQPlus(std::size_t dim, unsigned bits = 8, std::uint32_t seed = 42)
        : rotation_(dim, seed), bits_(bits) {
        if (bits < 1 || bits > 8) throw std::invalid_argument("bits must be in [1, 8]");
    }

    std::size_t dim() const { return rotation_.dim(); }
    std::size_t padded_dim() const { return rotation_.padded_dim(); }
    PreparedVector prepare(const float* vector) const { return rotation_.prepare(vector); }

    PlusCode quantize_vector(const float* vector, const float* centroid) const {
        auto r = rotation_.residual(vector, centroid);
        PlusCode out;
        out.norm2 = detail::dot(r, r);
        const float norm = std::sqrt(out.norm2);
        std::vector<float> abs_unit(padded_dim(), 0.0f);
        out.signs.assign(padded_dim() / 64, 0);
        for (std::size_t i = 0; i < padded_dim(); ++i) {
            if (r[i] > 0) detail::set_bit(out.signs, i);
            if (norm) abs_unit[i] = std::abs(r[i]) / norm;
        }
        float ipnorm = 1.0f;
        out.code = magnitudes(abs_unit, ipnorm);
        const std::uint8_t mask = static_cast<std::uint8_t>((1u << bits_) - 1u);
        for (std::size_t i = 0; i < padded_dim(); ++i)
            if (!detail::bit(out.signs, i)) out.code[i] = mask - out.code[i];
        out.xipnorm = 2 * norm * ipnorm;
        return out;
    }

    std::vector<PlusCode> quantize_cluster(const float* vectors, std::size_t count, const float* centroid) const {
        if (count && !vectors) throw std::invalid_argument("null vectors");
        std::vector<PlusCode> result;
        result.reserve(count);
        for (std::size_t i = 0; i < count; ++i) result.push_back(quantize_vector(vectors + i * dim(), centroid));
        return result;
    }

    PlusQuery quantize_query(const float* query, const float* centroid) const {
        return quantize_residual(rotation_.residual(query, centroid));
    }

    PlusQuery quantize_query(const PreparedVector& query, const PreparedVector& centroid) const {
        return quantize_residual(rotation_.residual(query, centroid));
    }

private:
    PlusQuery quantize_residual(std::vector<float> residual) const {
        PlusQuery out;
        out.residual = std::move(residual);
        out.norm2 = detail::dot(out.residual, out.residual);
        return out;
    }

public:
    float approximate_distance(const PlusQuery& query, const PlusCode& vector) const {
        check(query, vector);
        float dot_axis = 0.0f;
        for (std::size_t i = 0; i < padded_dim(); ++i) dot_axis += axis(vector, i) * query.residual[i];
        return vector.norm2 + query.norm2 - vector.xipnorm * dot_axis;
    }

    float lower_bound(const PlusQuery& query, const PlusCode& vector) const {
        check(query, vector);
        if (vector.norm2 == 0) return query.norm2;
        float a2 = 0.0f, qa = 0.0f;
        for (std::size_t i = 0; i < padded_dim(); ++i) {
            const float a = axis(vector, i);
            a2 += a * a;
            qa += a * query.residual[i];
        }
        if (a2 == 0 || vector.xipnorm == 0) {
            const float gap = std::sqrt(query.norm2) - std::sqrt(vector.norm2);
            return detail::safe_lower(gap * gap, vector.norm2, query.norm2);
        }
        const float xa = 2 * vector.norm2 / vector.xipnorm;
        const float x_orth = std::sqrt(std::max(0.0f, vector.norm2 - xa * xa / a2));
        const float q_orth = std::sqrt(std::max(0.0f, query.norm2 - qa * qa / a2));
        const float bound = query.norm2 + vector.norm2 - 2 * (xa * qa / a2 + x_orth * q_orth);
        return detail::safe_lower(bound, vector.norm2, query.norm2);
    }

private:
    float axis(const PlusCode& vector, std::size_t i) const {
        const bool positive = detail::bit(vector.signs, i);
        const float magnitude = positive ? vector.code[i] : ((1u << bits_) - 1u - vector.code[i]);
        return (positive ? 1.0f : -1.0f) * (magnitude + 0.5f);
    }

    void check(const PlusQuery& query, const PlusCode& vector) const {
        if (query.residual.size() != padded_dim() || vector.code.size() != padded_dim() ||
            vector.signs.size() != padded_dim() / 64)
            throw std::invalid_argument("code dimension mismatch");
    }

    std::vector<std::uint8_t> magnitudes(const std::vector<float>& values, float& ipnorm) const {
        const float max_o = *std::max_element(values.begin(), values.end());
        std::vector<std::uint8_t> code(padded_dim(), 0);
        if (!(max_o > 0)) { ipnorm = 1; return code; }
        const int mask = (1 << bits_) - 1;
        const float t_start = (mask / 3) / max_o;
        const float t_end = (mask + 10) / max_o;
        std::vector<int> current(padded_dim());
        float denominator = padded_dim() * 0.25f, numerator = 0.0f;
        using Event = std::pair<float, std::size_t>;
        std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events;
        for (std::size_t i = 0; i < padded_dim(); ++i) {
            current[i] = int(t_start * values[i] + 1e-5f);
            denominator += float(current[i]) * current[i] + current[i];
            numerator += (current[i] + 0.5f) * values[i];
            if (values[i] > 0) events.emplace((current[i] + 1) / values[i], i);
        }
        float best_ip = 0.0f, best_t = 0.0f;
        while (!events.empty()) {
            const auto [t, i] = events.top();
            events.pop();
            ++current[i];
            denominator += 2 * current[i];
            numerator += values[i];
            const float ip = numerator / std::sqrt(denominator);
            if (ip > best_ip) { best_ip = ip; best_t = t; }
            if (current[i] < mask) {
                const float next = (current[i] + 1) / values[i];
                if (next < t_end) events.emplace(next, i);
            }
        }
        numerator = 0.0f;
        for (std::size_t i = 0; i < padded_dim(); ++i) {
            code[i] = static_cast<std::uint8_t>(std::min(mask, int(best_t * values[i] + 1e-5f)));
            numerator += (code[i] + 0.5f) * values[i];
        }
        ipnorm = numerator > 0 ? 1.0f / numerator : 1.0f;
        return code;
    }

    detail::Rotation rotation_;
    unsigned bits_;
};

}  // namespace ivf_rabitq
