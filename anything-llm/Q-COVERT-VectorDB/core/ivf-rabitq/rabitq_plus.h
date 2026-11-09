#pragma once

#include <Eigen/Dense>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <queue>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

using FloatRowMat = Eigen::Matrix<float, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;

class RaBitQPlus {
public:
    struct QuantizeResult {
        std::vector<uint8_t> code;
        std::vector<uint8_t> bin_code;
        float sqr_x = 0.0f;
        float xipnorm = 0.0f;

        std::vector<uint8_t> serialize() const {
            const size_t code_size = code.size();
            const size_t bin_size = bin_code.size();
            std::vector<uint8_t> buf(sizeof(float) * 2 + sizeof(uint32_t) * 2 + code_size + bin_size);
            uint8_t* ptr = buf.data();

            std::memcpy(ptr, &sqr_x, sizeof(float));
            ptr += sizeof(float);
            std::memcpy(ptr, &xipnorm, sizeof(float));
            ptr += sizeof(float);

            const uint32_t cs = static_cast<uint32_t>(code_size);
            std::memcpy(ptr, &cs, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            std::memcpy(ptr, code.data(), code_size);
            ptr += code_size;

            const uint32_t bs = static_cast<uint32_t>(bin_size);
            std::memcpy(ptr, &bs, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            std::memcpy(ptr, bin_code.data(), bin_size);
            return buf;
        }

        static std::pair<QuantizeResult, size_t> deserialize(const uint8_t* data, size_t available) {
            if (available < sizeof(float) * 2 + sizeof(uint32_t) * 2) {
                return {QuantizeResult{}, 0};
            }

            QuantizeResult result;
            const uint8_t* ptr = data;
            size_t consumed = 0;

            std::memcpy(&result.sqr_x, ptr, sizeof(float));
            ptr += sizeof(float);
            consumed += sizeof(float);
            std::memcpy(&result.xipnorm, ptr, sizeof(float));
            ptr += sizeof(float);
            consumed += sizeof(float);

            uint32_t cs = 0;
            uint32_t bs = 0;
            std::memcpy(&cs, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            consumed += sizeof(uint32_t);
            if (consumed + cs + sizeof(uint32_t) > available) {
                return {QuantizeResult{}, 0};
            }
            result.code.resize(cs);
            std::memcpy(result.code.data(), ptr, cs);
            ptr += cs;
            consumed += cs;

            std::memcpy(&bs, ptr, sizeof(uint32_t));
            ptr += sizeof(uint32_t);
            consumed += sizeof(uint32_t);
            if (consumed + bs > available) {
                return {QuantizeResult{}, 0};
            }
            result.bin_code.resize(bs);
            std::memcpy(result.bin_code.data(), ptr, bs);
            consumed += bs;

            return {std::move(result), consumed};
        }

        static std::pair<QuantizeResult, size_t> deserialize(const uint8_t* data) {
            constexpr size_t very_large = static_cast<size_t>(-1) / 2;
            return deserialize(data, very_large);
        }

        size_t serialized_size() const {
            return sizeof(float) * 2 + sizeof(uint32_t) * 2 + code.size() + bin_code.size();
        }
    };

    RaBitQPlus(size_t dim, size_t bits = 8)
        : dim_(dim),
          padded_dim_(padded_dim_static(dim)),
          bits_(bits),
          fac_rescale_(1 << bits) {
        generate_rotation_matrix();
    }

    size_t dim() const { return dim_; }
    size_t padded_dim() const { return padded_dim_; }
    size_t bits() const { return bits_; }

    static constexpr size_t padded_dim_static(size_t dim) noexcept {
        return ((dim + 63u) / 64u) * 64u;
    }

    static constexpr size_t serialized_vector_bytes_static(size_t dim, size_t bits = 8) noexcept {
        (void)bits;
        const size_t pd = padded_dim_static(dim);
        return sizeof(float) * 2 + sizeof(uint32_t) * 2 + pd + pd / 8u;
    }

    std::vector<QuantizeResult> quantize(const float* centroid, const float* vectors, size_t num_vectors) {
        FloatRowMat rotated_c;
        FloatRowMat xp_norm;
        std::vector<int8_t> signs;

        data_transformation(centroid, vectors, num_vectors, rotated_c, xp_norm, signs);

        std::vector<QuantizeResult> results(num_vectors);
        for (size_t i = 0; i < num_vectors; ++i) {
            results[i].sqr_x = l2_sqr(vectors + i * dim_, centroid, dim_);
        }

        exrabitq_codes_and_factors(xp_norm, signs, results);
        pack_binary_codes(signs, num_vectors, results);
        return results;
    }

    float compute_distance(const float* query, const float* centroid, const QuantizeResult& qr) const {
        if (qr.code.size() < padded_dim_ || qr.bin_code.size() < padded_dim_ / 8) {
            throw std::runtime_error("RaBitQPlus::compute_distance: malformed quantized vector");
        }

        Eigen::RowVectorXf q_pad = Eigen::RowVectorXf::Zero(padded_dim_);
        std::memcpy(q_pad.data(), query, sizeof(float) * dim_);

        Eigen::RowVectorXf c_pad = Eigen::RowVectorXf::Zero(padded_dim_);
        std::memcpy(c_pad.data(), centroid, sizeof(float) * dim_);

        const Eigen::RowVectorXf q_rotated = q_pad * rotation_;
        const Eigen::RowVectorXf c_rotated = c_pad * rotation_;
        const Eigen::RowVectorXf residual_q = q_rotated - c_rotated;

        const float sqr_y = l2_sqr(query, centroid, dim_);
        const float half_sum_residual = 0.5f * static_cast<float>(residual_q.sum());

        float rabitq_ip = 0.0f;
        for (size_t j = 0; j < padded_dim_; ++j) {
            const size_t byte_idx = j / 8;
            const size_t bit_idx = 7 - (j % 8);
            const float bin_val = ((qr.bin_code[byte_idx] >> bit_idx) & 1) ? 1.0f : 0.0f;
            rabitq_ip += (bin_val - 0.5f) * residual_q(static_cast<Eigen::Index>(j));
        }

        float ip_8bit = 0.0f;
        for (size_t j = 0; j < padded_dim_; ++j) {
            ip_8bit += static_cast<float>(qr.code[j]) * residual_q(static_cast<Eigen::Index>(j));
        }

        return qr.sqr_x + sqr_y -
               qr.xipnorm * (fac_rescale_ * rabitq_ip + ip_8bit -
                              (fac_rescale_ - 1) * half_sum_residual);
    }

private:
    size_t dim_;
    size_t padded_dim_;
    size_t bits_;
    int fac_rescale_;
    FloatRowMat rotation_;

    void generate_rotation_matrix() {
        std::mt19937 rng(42);
        FloatRowMat random = FloatRowMat::NullaryExpr(
            padded_dim_, padded_dim_, [&]() { return std::normal_distribution<float>(0, 1)(rng); });
        Eigen::HouseholderQR<FloatRowMat> qr(random);
        FloatRowMat q = qr.householderQ();
        rotation_ = q.transpose();
    }

    void data_transformation(const float* centroid,
                             const float* vectors,
                             size_t num_vectors,
                             FloatRowMat& rotated_c,
                             FloatRowMat& xp_norm,
                             std::vector<int8_t>& signs) const {
        Eigen::RowVectorXf c_pad = Eigen::RowVectorXf::Zero(padded_dim_);
        std::memcpy(c_pad.data(), centroid, sizeof(float) * dim_);
        const Eigen::RowVectorXf cp = c_pad * rotation_;

        rotated_c = FloatRowMat(1, padded_dim_);
        rotated_c.row(0) = cp;

        xp_norm.resize(num_vectors, padded_dim_);
        signs.resize(num_vectors * padded_dim_);

        Eigen::RowVectorXf x_pad = Eigen::RowVectorXf::Zero(padded_dim_);
        for (size_t i = 0; i < num_vectors; ++i) {
            x_pad.setZero();
            std::memcpy(x_pad.data(), vectors + i * dim_, sizeof(float) * dim_);
            Eigen::RowVectorXf xp = x_pad * rotation_;
            xp -= cp;

            for (size_t j = 0; j < padded_dim_; ++j) {
                signs[i * padded_dim_ + j] = (xp(static_cast<Eigen::Index>(j)) > 0) ? 1 : -1;
            }

            const float norm = xp.norm();
            if (norm > 1e-10f) {
                xp_norm.row(static_cast<Eigen::Index>(i)) = xp / norm;
            } else {
                xp_norm.row(static_cast<Eigen::Index>(i)).setZero();
            }
        }
    }

    void fast_quantize(const float* o_prime, uint8_t* code, float& ip_norm) const {
        constexpr double eps = 1e-5;
        constexpr int n_enum = 10;
        double max_o = -1;
        for (size_t i = 0; i < padded_dim_; ++i) {
            max_o = std::max(max_o, static_cast<double>(o_prime[i]));
        }

        if (!(max_o > 0.0)) {
            std::fill(code, code + padded_dim_, 0);
            ip_norm = 1.0f;
            return;
        }

        const double t_start = static_cast<double>(((1 << bits_) - 1) / 3) / max_o;
        const double t_end = static_cast<double>(((1 << bits_) - 1) + n_enum) / max_o;

        std::vector<int> cur_o_bar(padded_dim_);
        double sqr_denominator = padded_dim_ * 0.25;
        double numerator = 0;

        for (size_t i = 0; i < padded_dim_; ++i) {
            cur_o_bar[i] = static_cast<int>(t_start * o_prime[i] + eps);
            sqr_denominator += static_cast<double>(cur_o_bar[i]) * cur_o_bar[i] + cur_o_bar[i];
            numerator += (cur_o_bar[i] + 0.5) * o_prime[i];
        }

        std::priority_queue<std::pair<double, size_t>,
                            std::vector<std::pair<double, size_t>>,
                            std::greater<std::pair<double, size_t>>>
            next_t;

        for (size_t i = 0; i < padded_dim_; ++i) {
            if (o_prime[i] > 0.0f) {
                next_t.emplace(static_cast<double>(cur_o_bar[i] + 1) / o_prime[i], i);
            }
        }

        double max_ip = 0;
        double t = 0;
        while (!next_t.empty()) {
            const double cur_t = next_t.top().first;
            const size_t update_id = next_t.top().second;
            next_t.pop();

            cur_o_bar[update_id]++;
            const int update_o_bar = cur_o_bar[update_id];
            sqr_denominator += 2 * update_o_bar;
            numerator += o_prime[update_id];

            const double cur_ip = numerator / std::sqrt(sqr_denominator);
            if (cur_ip > max_ip) {
                max_ip = cur_ip;
                t = cur_t;
            }

            if (update_o_bar < (1 << bits_) - 1) {
                const double t_next = static_cast<double>(update_o_bar + 1) / o_prime[update_id];
                if (t_next < t_end) {
                    next_t.emplace(t_next, update_id);
                }
            }
        }

        numerator = 0;
        const int mask = (1 << bits_) - 1;
        for (size_t i = 0; i < padded_dim_; ++i) {
            int o_bar = static_cast<int>(t * o_prime[i] + eps);
            o_bar = std::min(o_bar, mask);
            numerator += (o_bar + 0.5) * o_prime[i];
            code[i] = static_cast<uint8_t>(o_bar);
        }

        ip_norm = static_cast<float>(1.0 / numerator);
        if (!std::isfinite(ip_norm)) {
            ip_norm = 1.0f;
        }
    }

    void exrabitq_codes_and_factors(const FloatRowMat& xp_norm,
                                    const std::vector<int8_t>& signs,
                                    std::vector<QuantizeResult>& results) const {
        const int32_t mask = (1 << bits_) - 1;
        const FloatRowMat abs_mat = xp_norm.array().abs();
        std::vector<uint8_t> tmp_code(padded_dim_);

        for (size_t i = 0; i < results.size(); ++i) {
            float ipnorm = 1.0f;
            fast_quantize(&abs_mat(static_cast<Eigen::Index>(i), 0), tmp_code.data(), ipnorm);

            for (size_t j = 0; j < padded_dim_; ++j) {
                if (signs[i * padded_dim_ + j] < 0) {
                    tmp_code[j] = static_cast<uint8_t>((~tmp_code[j]) & mask);
                }
            }

            results[i].code.assign(tmp_code.begin(), tmp_code.end());
            const float dist2c = std::sqrt(static_cast<double>(results[i].sqr_x));
            results[i].xipnorm = ipnorm * 2 * dist2c;
        }
    }

    void pack_binary_codes(const std::vector<int8_t>& signs,
                           size_t num_vectors,
                           std::vector<QuantizeResult>& results) const {
        const size_t bin_bytes = padded_dim_ / 8;
        for (size_t i = 0; i < num_vectors; ++i) {
            results[i].bin_code.assign(bin_bytes, 0);
            for (size_t j = 0; j < padded_dim_; ++j) {
                if (signs[i * padded_dim_ + j] > 0) {
                    results[i].bin_code[j / 8] |= static_cast<uint8_t>(1u << (7 - (j % 8)));
                }
            }
        }
    }

    static float l2_sqr(const float* x, const float* y, size_t dim) {
        float result = 0.0f;
        for (size_t i = 0; i < dim; ++i) {
            const float diff = x[i] - y[i];
            result += diff * diff;
        }
        return result;
    }
};

