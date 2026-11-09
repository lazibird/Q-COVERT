#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include "sonic/util/log.hpp"

#include "sonic/util/ext/lambertw.hpp"

namespace sn::oram::zingoram::analysis {

inline std::uint64_t zingoram_tree_height(std::uint64_t block_count, std::uint32_t eviction_rate) {
  sn::util::log::ensure(block_count > 0, "zingoram::analysis: block_count must be positive");
  sn::util::log::ensure(eviction_rate > 0, "zingoram::analysis: eviction_rate must be positive");

  const double rate = static_cast<double>(eviction_rate);
  double two_n_over_a = std::ceil((2.0 * static_cast<double>(block_count)) / rate);
  if (two_n_over_a < 1.0) {
    two_n_over_a = 1.0;
  }

  const double raw_height = std::ceil(std::log2(two_n_over_a));
  std::uint64_t height = raw_height > 0.0 ? static_cast<std::uint64_t>(raw_height) : 0ULL;
  height = std::max<std::uint64_t>(0, height - 1);

  return height;
}

inline std::uint64_t max_eviction_rate(std::uint64_t bucket_real_size) {
  sn::util::log::ensure(bucket_real_size > 0, "zingoram::analysis: bucket_real_size must be positive");

  const auto constraint = [bucket_real_size](double candidate) {
    const double x = static_cast<double>(bucket_real_size);
    const double y = candidate;
    return x * std::log((2.0 * x) / y) + (y / 2.0) - x - std::log(4.0) >= 0.0 && y <= 2.0 * x;
  };

  double low = 1.0;
  double high = 2.0 * static_cast<double>(bucket_real_size);
  double result = low;
  while (high - low > 1e-6) {
    const double mid = (low + high) * 0.5;
    if (constraint(mid)) {
      result = mid;
      low = mid;
    } else {
      high = mid;
    }
  }
  auto integral = static_cast<std::uint64_t>(std::floor(result));
  while (integral > 0 && !constraint(static_cast<double>(integral))) {
    --integral;
  }
  sn::util::log::ensure(integral > 0, "zingoram::analysis: failed to find a valid eviction rate");
  return integral;
}

inline std::uint64_t stash_bound(
    std::uint64_t bucket_size, std::uint64_t eviction_rate, std::uint64_t security_parameter
) {
  sn::util::log::ensure(bucket_size > 0, "zingoram::analysis: bucket_size must be positive");
  sn::util::log::ensure(eviction_rate > 0, "zingoram::analysis: eviction_rate must be positive");
  sn::util::log::ensure(security_parameter > 0, "zingoram::analysis: security_parameter must be positive");

  const double Z = static_cast<double>(bucket_size);
  const double A = static_cast<double>(eviction_rate);
  const double lambda_val = static_cast<double>(security_parameter);
  const double a = A / 2.0;
  const double q = Z * std::log(Z / a) + a - Z - std::log(4.0);

  sn::util::log::ensuref(
      q > 0.0, "zingoram::analysis: invalid eviction parameters: Z=%d A=%d q=%f", bucket_size, eviction_rate, q
  );

  auto probability = [a, Z, q](std::uint64_t stash_size) {
    const double R = static_cast<double>(stash_size);
    const double numerator = std::pow(a / Z, R) * std::exp(-q);
    const double denominator = 1.0 - std::exp(-q);
    return numerator / denominator;
  };

  const double threshold = std::pow(2.0, -lambda_val);
  std::uint64_t stash_size = 1;
  double failure = probability(stash_size);
  sn::util::log::ensure(std::isfinite(failure) && failure >= 0.0, "zingoram::analysis: invalid failure probability");

  while (failure >= threshold) {
    ++stash_size;
    failure = probability(stash_size);
    sn::util::log::ensure(std::isfinite(failure), "zingoram::analysis: failed to find stash bound");
  }

  return stash_size;
}

/**
 from Snoopy paper: https://eprint.iacr.org/2021/1280
 Theorem 3. For any set of 𝑅 requests that are distinct and randomly distributed, number of subORAMs 𝑆, and security
 parameter 𝜆,let𝜇=𝑅/𝑆,𝛾=−log(1/(𝑆·2𝜆)),and𝑊0(·)bebranch0oftheLambert𝑊 function[23].Thenforthefollowingfunction𝑓(𝑅,𝑆)
 that outputs a batch size, the probability that a request is dropped is negligible in 𝜆: f(R,S) = min(R, μ * exp(W₀(e⁻¹
 * (γ/μ - 1)) + 1)) effectively, a balls-into-bins model with negligible failure probability where R is the number of
 balls, S is the number of bins, and lambda is the security parameter
*/
// f(R,S) = min(R, μ * exp(W₀(e⁻¹ * (γ/μ - 1)) + 1))
namespace detail {

inline double balls_and_bins_impl(double R, double S, double lambda_param) {
  sn::util::log::ensure(R >= 0.0, "balls_and_bins: R must be non-negative");
  sn::util::log::ensure(S > 0.0, "balls_and_bins: S must be positive");
  sn::util::log::ensure(lambda_param > 0.0, "balls_and_bins: lambda must be positive");

  // calculate μ (mu) - average number of requests per subORAM
  double mu = R / S;

  // calculate γ (gamma)
  // γ = -log(1 / (S * 2^λ))
  double gamma = -std::log(1.0 / (S * std::pow(2.0, lambda_param)));

  // calculate (γ/μ - 1)
  // inner term of the Lambert W function input
  double inner_term = gamma / mu - 1.0;

  // multiply by e^(-1)
  double e_neg_1 = std::exp(-1.0);
  double lambert_input = e_neg_1 * inner_term;

  // apply Lambert W function (branch 0)
  double lambert_result = lambertw::LambertW0(lambert_input);

  // add 1 to Lambert W result
  // compute the input to the exponentiation
  double exponent = lambert_result + 1.0;

  // take e to the power of the result from step 6
  // this is the exp[...] part of the formula
  double exp_term = std::exp(exponent);

  // multiply by μ (mu)
  // this completes the μ * exp[...] part of the formula
  double result = mu * exp_term;

  // take the minimum of R and the result
  // this implements the min(R, ...) part of the formula
  return std::min(R, result);
}

} // namespace detail

inline double balls_and_bins(std::uint64_t R, std::uint64_t S, double lambda_param) {
  const double real_R = static_cast<double>(R);
  const double real_S = static_cast<double>(S);
  return detail::balls_and_bins_impl(real_R, real_S, lambda_param);
}

inline double balls_and_bins(int R, int S, double lambda_param) {
  return detail::balls_and_bins_impl(static_cast<double>(R), static_cast<double>(S), lambda_param);
}

inline std::uint64_t balls_and_bins_integer(std::uint64_t R, std::uint64_t S, double lambda_param) {
  return static_cast<std::uint64_t>(std::ceil(balls_and_bins(R, S, lambda_param)));
}

} // namespace sn::oram::zingoram::analysis
