#pragma once

#include <span>

namespace ccdeseq2 {

[[nodiscard]] double negative_binomial_nll(std::span<const double> counts,
                                           std::span<const double> mu,
                                           double alpha);

[[nodiscard]] double negative_binomial_nll_derivative_alpha(
    std::span<const double> counts, std::span<const double> mu, double alpha);

}  // namespace ccdeseq2
