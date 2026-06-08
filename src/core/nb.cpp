#include "ccdeseq2/nb.hpp"

#include <cmath>
#include <limits>

#include "ccdeseq2/special.hpp"

namespace ccdeseq2 {

double negative_binomial_nll(std::span<const double> counts,
                             std::span<const double> mu, double alpha) {
  if (alpha <= 0.0 || counts.size() != mu.size()) {
    return std::numeric_limits<double>::infinity();
  }
  const double alpha_inv = 1.0 / alpha;
  double result = static_cast<double>(counts.size()) * alpha_inv * std::log(alpha);
  const double lgamma_alpha_inv = gammaln(alpha_inv);
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (mu[i] <= 0.0 || !std::isfinite(mu[i])) {
      return std::numeric_limits<double>::infinity();
    }
    const double count = counts[i];
    const double logbinom =
        gammaln(count + alpha_inv) - gammaln(count + 1.0) -
        lgamma_alpha_inv;
    result += -logbinom + (count + alpha_inv) * std::log(alpha_inv + mu[i]) -
              count * std::log(mu[i]);
  }
  return result;
}

double negative_binomial_nll_derivative_alpha(std::span<const double> counts,
                                              std::span<const double> mu,
                                              double alpha) {
  if (alpha <= 0.0 || counts.size() != mu.size()) {
    return std::numeric_limits<double>::infinity();
  }
  const double alpha_inv = 1.0 / alpha;
  double sum = 0.0;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (mu[i] <= 0.0 || !std::isfinite(mu[i])) {
      return std::numeric_limits<double>::infinity();
    }
    const double count = counts[i];
    sum += digamma(alpha_inv) - digamma(count + alpha_inv) +
           std::log1p(mu[i] * alpha) +
           (count - mu[i]) / (mu[i] + alpha_inv);
  }
  return -(alpha_inv * alpha_inv * sum);
}

}  // namespace ccdeseq2
