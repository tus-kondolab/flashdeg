#include "ccdeseq2/pydeseq2_utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <utility>
#include <vector>

#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/linalg.hpp"
#include "ccdeseq2/numpy_compat.hpp"
#include "ccdeseq2/optimize.hpp"
#include "ccdeseq2/pydeseq2_dds.hpp"
#include "ccdeseq2/pydeseq2_grid_search.hpp"
#include "ccdeseq2/special.hpp"

namespace ccdeseq2::pydeseq2::utils {
namespace {

constexpr double kCookMinDisp = 0.04;

[[nodiscard]] double logaddexp(double a, double b) {
  const double hi = std::max(a, b);
  const double lo = std::min(a, b);
  if (!std::isfinite(hi)) {
    return hi;
  }
  return hi + std::log1p(std::exp(lo - hi));
}

[[nodiscard]] double linear_predictor(const DesignMatrix& design_matrix,
                                      std::size_t sample,
                                      std::span<const double> beta) {
  double value = 0.0;
  for (std::size_t col = 0; col < design_matrix.column_count(); ++col) {
    value += design_matrix(sample, col) * beta[col];
  }
  return value;
}

[[nodiscard]] std::vector<double> invert_matrix_row_major(
    const std::vector<double>& matrix, std::size_t n) {
  if (matrix.size() != n * n) {
    throw Error(ExitCode::numeric_error,
                "Internal error: inverse input has invalid dimensions.");
  }
  std::vector<double> augmented(n * 2 * n, 0.0);
  auto at = [&](std::size_t row, std::size_t col) -> double& {
    return augmented[row * (2 * n) + col];
  };
  for (std::size_t row = 0; row < n; ++row) {
    for (std::size_t col = 0; col < n; ++col) {
      at(row, col) = matrix[row * n + col];
    }
    at(row, n + row) = 1.0;
  }

  for (std::size_t col = 0; col < n; ++col) {
    std::size_t pivot = col;
    double pivot_abs = std::abs(at(col, col));
    for (std::size_t row = col + 1; row < n; ++row) {
      const double candidate = std::abs(at(row, col));
      if (candidate > pivot_abs) {
        pivot = row;
        pivot_abs = candidate;
      }
    }
    if (pivot_abs <= 1e-30 || !std::isfinite(pivot_abs)) {
      throw Error(ExitCode::numeric_error, "Matrix inversion failed.");
    }
    if (pivot != col) {
      for (std::size_t j = 0; j < 2 * n; ++j) {
        std::swap(at(col, j), at(pivot, j));
      }
    }
    const double scale = at(col, col);
    for (std::size_t j = 0; j < 2 * n; ++j) {
      at(col, j) /= scale;
    }
    for (std::size_t row = 0; row < n; ++row) {
      if (row == col) {
        continue;
      }
      const double factor = at(row, col);
      if (factor == 0.0) {
        continue;
      }
      for (std::size_t j = 0; j < 2 * n; ++j) {
        at(row, j) -= factor * at(col, j);
      }
    }
  }

  std::vector<double> inverse(n * n, 0.0);
  for (std::size_t row = 0; row < n; ++row) {
    for (std::size_t col = 0; col < n; ++col) {
      inverse[row * n + col] = at(row, n + col);
    }
  }
  return inverse;
}

void shrinkage_loss_gradient(const DesignMatrix& design_matrix,
                             std::span<const double> counts, double size,
                             std::span<const double> offset,
                             double prior_no_shrink_scale, double prior_scale,
                             std::size_t shrink_index,
                             std::span<const double> beta, double scale_cnst,
                             std::vector<double>& gradient) {
  const std::size_t p = design_matrix.column_count();
  gradient.assign(p, 0.0);
  if (counts.size() != design_matrix.sample_count() ||
      offset.size() != design_matrix.sample_count() || shrink_index >= p) {
    throw Error(ExitCode::input_error,
                "Shrinkage gradient inputs have inconsistent dimensions.");
  }

  for (std::size_t col = 0; col < p; ++col) {
    if (col == shrink_index) {
      gradient[col] += 2.0 * beta[col] /
                       (prior_scale * prior_scale +
                        beta[shrink_index] * beta[shrink_index]);
    } else {
      gradient[col] += beta[col] /
                       (prior_no_shrink_scale * prior_no_shrink_scale);
    }
  }

  for (std::size_t sample = 0; sample < design_matrix.sample_count();
       ++sample) {
    const double xbeta = linear_predictor(design_matrix, sample, beta);
    const double d_nll =
        counts[sample] -
        (counts[sample] + size) /
            (1.0 + size * std::exp(-xbeta - offset[sample]));
    for (std::size_t col = 0; col < p; ++col) {
      gradient[col] -= d_nll * design_matrix(sample, col);
    }
  }

  for (double& value : gradient) {
    value /= scale_cnst;
  }
}

[[nodiscard]] std::vector<double> analytic_shrinkage_hessian_for_compat_mode(
    const DesignMatrix& design_matrix, std::span<const double> counts,
    double size, std::span<const double> offset,
    double prior_no_shrink_scale, double prior_scale,
    std::size_t shrink_index, std::span<const double> beta,
    double scale_cnst, CompatMode compat_mode) {
  const std::size_t p = design_matrix.column_count();
  std::vector<double> hessian(p * p, 0.0);
  if (counts.size() != design_matrix.sample_count() ||
      offset.size() != design_matrix.sample_count() || shrink_index >= p) {
    throw Error(ExitCode::input_error,
                "Shrinkage Hessian inputs have inconsistent dimensions.");
  }

  for (std::size_t sample = 0; sample < design_matrix.sample_count();
       ++sample) {
    const double xbeta = linear_predictor(design_matrix, sample, beta);
    const double exp_xbeta_off = std::exp(xbeta + offset[sample]);
    const double denom = size + exp_xbeta_off;
    const double frac =
        (counts[sample] + size) * size * exp_xbeta_off / (denom * denom);
    for (std::size_t i = 0; i < p; ++i) {
      for (std::size_t j = 0; j <= i; ++j) {
        hessian[i * p + j] +=
            design_matrix(sample, i) * frac * design_matrix(sample, j);
      }
    }
  }

  for (std::size_t i = 0; i < p; ++i) {
    for (std::size_t j = i + 1; j < p; ++j) {
      hessian[i * p + j] = hessian[j * p + i];
    }
  }
  std::vector<double> prior_terms(p, 0.0);
  for (std::size_t col = 0; col < p; ++col) {
    prior_terms[col] =
        col == shrink_index
            ? 2.0 * (prior_scale * prior_scale -
                     beta[shrink_index] * beta[shrink_index]) /
                  ((prior_scale * prior_scale +
                    beta[shrink_index] * beta[shrink_index]) *
                   (prior_scale * prior_scale +
                    beta[shrink_index] * beta[shrink_index]))
            : 1.0 / (prior_no_shrink_scale * prior_no_shrink_scale);
  }
  if (compat_mode == CompatMode::deseq2_r) {
    // The model prior contributes only diagonal curvature: independent
    // Cauchy(target LFC) and Normal(non-target) priors have no cross-coefficient
    // second derivatives.
    for (std::size_t i = 0; i < p; ++i) {
      hessian[i * p + i] += prior_terms[i];
    }
  } else {
    // PyDESeq2 builds h = np.diag(prior_terms) and then adds np.diag(h).
    // For a 2D diagonal matrix, np.diag(h) is a 1D vector, so NumPy
    // broadcasts it across rows. This makes the Hessian non-symmetric and
    // introduces spurious off-diagonal prior contributions. Mirrored
    // intentionally in pydeseq2 mode for byte-exact lfcSE compatibility.
    for (std::size_t row = 0; row < p; ++row) {
      for (std::size_t col = 0; col < p; ++col) {
        hessian[row * p + col] += prior_terms[col];
      }
    }
  }
  for (double& value : hessian) {
    value /= scale_cnst;
  }
  return hessian;
}

[[nodiscard]] std::vector<double> central_difference_shrinkage_hessian(
    const DesignMatrix& design_matrix, std::span<const double> counts,
    double size, std::span<const double> offset,
    double prior_no_shrink_scale, double prior_scale,
    std::size_t shrink_index, std::span<const double> beta) {
  const std::size_t p = design_matrix.column_count();
  if (beta.size() != p || counts.size() != design_matrix.sample_count() ||
      offset.size() != design_matrix.sample_count() || shrink_index >= p) {
    throw Error(ExitCode::input_error,
                "Finite-difference shrinkage Hessian inputs have inconsistent dimensions.");
  }

  // Central finite differences of the unscaled gradient with ndeps = 1e-3 and
  // unit parameter scaling. The resulting Hessian is used for the Laplace SE.
  constexpr double kNdeps = 1e-3;
  std::vector<double> beta_plus(beta.begin(), beta.end());
  std::vector<double> beta_minus(beta.begin(), beta.end());
  std::vector<double> grad_plus;
  std::vector<double> grad_minus;
  std::vector<double> hessian(p * p, 0.0);

  for (std::size_t col = 0; col < p; ++col) {
    beta_plus[col] += kNdeps;
    beta_minus[col] -= kNdeps;
    shrinkage_loss_gradient(design_matrix, counts, size, offset,
                            prior_no_shrink_scale, prior_scale, shrink_index,
                            beta_plus, 1.0, grad_plus);
    shrinkage_loss_gradient(design_matrix, counts, size, offset,
                            prior_no_shrink_scale, prior_scale, shrink_index,
                            beta_minus, 1.0, grad_minus);
    for (std::size_t row = 0; row < p; ++row) {
      hessian[row * p + col] =
          (grad_plus[row] - grad_minus[row]) / (2.0 * kNdeps);
    }
    beta_plus[col] = beta[col];
    beta_minus[col] = beta[col];
  }

  for (std::size_t row = 0; row < p; ++row) {
    for (std::size_t col = 0; col < row; ++col) {
      const double sym =
          0.5 * (hessian[row * p + col] + hessian[col * p + row]);
      hessian[row * p + col] = sym;
      hessian[col * p + row] = sym;
    }
  }
  return hessian;
}

[[nodiscard]] int trim_bucket(std::size_t group_size) {
  if (group_size >= 24) {
    return 2;
  }
  if (group_size >= 4) {
    return 1;
  }
  return 0;
}

[[nodiscard]] std::map<std::vector<double>, std::vector<std::size_t>>
design_groups(const DesignMatrix& design_matrix) {
  std::map<std::vector<double>, std::vector<std::size_t>> groups;
  for (std::size_t sample = 0; sample < design_matrix.sample_count();
       ++sample) {
    std::vector<double> row(design_matrix.column_count(), 0.0);
    for (std::size_t col = 0; col < design_matrix.column_count(); ++col) {
      row[col] = design_matrix(sample, col);
    }
    groups[std::move(row)].push_back(sample);
  }
  return groups;
}

[[nodiscard]] double trimmed_variance_for_gene(
    const CountMatrix& normed_counts, std::size_t gene,
    const std::vector<std::size_t>& samples, double trim, double scale) {
  std::vector<double> values;
  values.reserve(samples.size());
  for (const std::size_t sample : samples) {
    values.push_back(normed_counts(sample, gene));
  }
  const double mean = trim_mean(values, trim, EmptyInputPolicy::return_nan);
  std::vector<double> squared_errors;
  squared_errors.reserve(samples.size());
  for (const std::size_t sample : samples) {
    const double diff = normed_counts(sample, gene) - mean;
    squared_errors.push_back(diff * diff);
  }
  return scale * trim_mean(std::move(squared_errors), trim,
                           EmptyInputPolicy::return_nan);
}

[[nodiscard]] double robust_variance_for_gene(
    const CountMatrix& normed_counts,
    const std::map<std::vector<double>, std::vector<std::size_t>>& groups,
    const ByteMask& replaceable3, std::size_t gene) {
  static constexpr double kTrimRatio[3] = {1.0 / 3.0, 1.0 / 4.0,
                                           1.0 / 8.0};
  static constexpr double kScale[3] = {2.04, 1.86, 1.51};

  if (std::any_of(replaceable3.begin(), replaceable3.end(),
                  [](std::uint8_t value) { return value != 0; })) {
    bool seen = false;
    double best = -std::numeric_limits<double>::infinity();
    for (const auto& [row, samples] : groups) {
      (void)row;
      if (samples.size() < 3) {
        continue;
      }
      const int bucket = trim_bucket(samples.size());
      const double value = trimmed_variance_for_gene(
          normed_counts, gene, samples, kTrimRatio[bucket], kScale[bucket]);
      if (std::isnan(value)) {
        return std::numeric_limits<double>::quiet_NaN();
      }
      best = std::max(best, value);
      seen = true;
    }
    return seen ? best : std::numeric_limits<double>::quiet_NaN();
  }

  std::vector<std::size_t> all_samples(normed_counts.sample_count(), 0);
  std::iota(all_samples.begin(), all_samples.end(), 0);
  return trimmed_variance_for_gene(normed_counts, gene, all_samples, 0.125,
                                   1.51);
}

}  // namespace

double dispersion_trend(double base_mean, double a0, double a1) {
  return a0 + a1 / base_mean;
}

bool is_non_zero_gene(const CountMatrix& counts, std::size_t gene) {
  for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
    if (counts(sample, gene) != 0.0) {
      return true;
    }
  }
  return false;
}

double mean_absolute_deviation(std::vector<double> values) {
  const double center = median(values);
  for (double& value : values) {
    value = std::abs(value - center);
  }
  return median(std::move(values)) / normal_ppf_75();
}

double nb_nll(std::span<const double> counts, std::span<const double> mu,
              double alpha) {
  return ccdeseq2::negative_binomial_nll(counts, mu, alpha);
}

double dnb_nll(std::span<const double> counts, std::span<const double> mu,
               double alpha) {
  return ccdeseq2::negative_binomial_nll_derivative_alpha(counts, mu, alpha);
}

[[nodiscard]] double negative_binomial_log_posterior_loss(
    std::span<const double> beta, const DesignMatrix& design_matrix,
    std::span<const double> counts, double size, std::span<const double> offset,
    double prior_no_shrink_scale, double prior_scale,
    std::size_t shrink_index) {
  const std::size_t n = design_matrix.sample_count();
  const std::size_t p = design_matrix.column_count();
  if (beta.size() != p || counts.size() != n || offset.size() != n ||
      shrink_index >= p || size <= 0.0 || prior_no_shrink_scale <= 0.0 ||
      prior_scale <= 0.0) {
    throw Error(ExitCode::input_error,
                "Shrinkage loss inputs have inconsistent dimensions.");
  }

  double prior = 0.0;
  for (std::size_t col = 0; col < p; ++col) {
    if (col == shrink_index) {
      continue;
    }
    prior += (beta[col] * beta[col]) /
             (2.0 * prior_no_shrink_scale * prior_no_shrink_scale);
  }
  prior += std::log1p((beta[shrink_index] / prior_scale) *
                      (beta[shrink_index] / prior_scale));

  double nll = 0.0;
  const double log_size = std::log(size);
  for (std::size_t sample = 0; sample < n; ++sample) {
    const double xbeta = linear_predictor(design_matrix, sample, beta);
    nll += counts[sample] * xbeta -
           (counts[sample] + size) *
               logaddexp(xbeta + offset[sample], log_size);
  }
  return prior - nll;
}

[[nodiscard]] NbinomGlmResult fit_shrunken_lfc_map(
    const DesignMatrix& design_matrix, std::span<const double> counts,
    double size, std::span<const double> offset,
    double prior_no_shrink_scale, double prior_scale,
    std::size_t shrink_index, CompatMode compat_mode) {
  const std::size_t p = design_matrix.column_count();
  if (counts.size() != design_matrix.sample_count() ||
      offset.size() != design_matrix.sample_count() || shrink_index >= p) {
    throw Error(ExitCode::input_error,
                "Shrinkage MAP inputs have inconsistent dimensions.");
  }

  std::vector<double> beta_init(p, 0.0);
  for (std::size_t col = 0; col < p; ++col) {
    beta_init[col] = (col % 2 == 0) ? 0.1 : -0.1;
  }
  std::vector<double> zeros(p, 0.0);
  double scale_cnst = negative_binomial_log_posterior_loss(
      zeros, design_matrix, counts, size, offset, prior_no_shrink_scale,
      prior_scale, shrink_index);
  scale_cnst = std::max(scale_cnst, 1.0);

  const auto objective = [&](std::span<const double> beta) {
    const double value = negative_binomial_log_posterior_loss(
                             beta, design_matrix, counts, size, offset,
                             prior_no_shrink_scale, prior_scale,
                             shrink_index) /
                         scale_cnst;
    // The compatibility target uses a scaled objective shifted by 10.0. This
    // does not change the mathematical optimum, but it does change the
    // numerical L-BFGS stopping point through relative function-decrease tests.
    return compat_mode == CompatMode::deseq2_r ? value + 10.0 : value;
  };
  const auto gradient = [&](std::span<const double> beta,
                            std::vector<double>& grad) {
    shrinkage_loss_gradient(design_matrix, counts, size, offset,
                            prior_no_shrink_scale, prior_scale, shrink_index,
                            beta, scale_cnst, grad);
  };

  LbfgsbOptions options;
  options.ftol = 1e-8;
  options.gtol = 1e-8;
  options.max_iterations = 15000;
  const auto fit_from_initial = [&](std::vector<double> initial) {
    return minimize_l_bfgs_b_scipy(objective, std::move(initial),
                                   std::vector<Bound>(p), options, gradient);
  };
  const LbfgsbResult opt = fit_from_initial(beta_init);

  std::vector<double> beta = opt.x;
  const bool optimizer_failed = !opt.converged;
  bool retry_after_unstable_fit = optimizer_failed;
  bool converged = opt.converged;
  if (compat_mode == CompatMode::deseq2_r && converged && beta.size() == p) {
    // Run a second small-opposite initialization as a stability diagnostic.
    // If the two MAPs differ by more than 0.01 natural-log units, mark the row
    // unstable and retry from the first local optimum below.
    std::vector<double> beta_init_2(p, 0.0);
    for (std::size_t col = 0; col < p; ++col) {
      beta_init_2[col] = (col % 2 == 0) ? -0.1 : 0.1;
    }
    const LbfgsbResult opt2 = fit_from_initial(std::move(beta_init_2));
    if (opt2.x.size() == p) {
      double max_diff = 0.0;
      for (std::size_t col = 0; col < p; ++col) {
        max_diff = std::max(max_diff, std::abs(beta[col] - opt2.x[col]));
      }
      if (max_diff > 0.01) {
        retry_after_unstable_fit = true;
        converged = false;
      }
    }
  }
  if ((optimizer_failed || beta.size() != p ||
       std::any_of(beta.begin(), beta.end(),
                   [](double value) { return !std::isfinite(value); })) &&
      p == 2) {
    beta = pydeseq2::grid_search::grid_fit_shrink_beta(
        counts, offset, design_matrix, size, prior_no_shrink_scale,
        prior_scale, scale_cnst, 60, -30.0, 30.0);
  }
  if (beta.size() != p) {
    beta.assign(p, std::numeric_limits<double>::quiet_NaN());
  }

  if (compat_mode == CompatMode::deseq2_r && retry_after_unstable_fit &&
      beta.size() == p &&
      std::all_of(beta.begin(), beta.end(),
                  [](double value) { return std::isfinite(value); })) {
    // Retry rows with an unstable first fit using an objective shifted so that
    // f(retry_initial) = -1. The constant shift leaves the minimizer unchanged
    // while keeping relative function-decrease stopping criteria on a stable
    // scale across genes. The remaining compatibility residual is the
    // optimizer engine: SciPy L-BFGS-B here versus R's BFGS implementation.
    const std::vector<double> retry_initial = beta;
    const double retry_cnst =
        -negative_binomial_log_posterior_loss(
            retry_initial, design_matrix, counts, size, offset,
            prior_no_shrink_scale, prior_scale, shrink_index) -
        1.0;
    const auto retry_objective = [&](std::span<const double> trial) {
      return negative_binomial_log_posterior_loss(
                 trial, design_matrix, counts, size, offset,
                 prior_no_shrink_scale, prior_scale, shrink_index) +
             retry_cnst;
    };
    const auto retry_gradient = [&](std::span<const double> trial,
                                    std::vector<double>& grad) {
      shrinkage_loss_gradient(design_matrix, counts, size, offset,
                              prior_no_shrink_scale, prior_scale, shrink_index,
                              trial, 1.0, grad);
    };
    LbfgsbOptions retry_options;
    retry_options.ftol = 1e-8;
    retry_options.gtol = 1e-8;
    retry_options.max_iterations = 15000;
    const LbfgsbResult retry = minimize_l_bfgs_b_scipy(
        retry_objective, retry_initial, std::vector<Bound>(p), retry_options,
        retry_gradient);
    if (retry.x.size() == p &&
        std::all_of(retry.x.begin(), retry.x.end(),
                    [](double value) { return std::isfinite(value); })) {
      beta = retry.x;
      converged = retry.converged;
    }
  }

  std::vector<double> inv_hessian(
      p * p, std::numeric_limits<double>::quiet_NaN());
  try {
    const std::vector<double> hessian =
        compat_mode == CompatMode::deseq2_r
            ? central_difference_shrinkage_hessian(
                  design_matrix, counts, size, offset, prior_no_shrink_scale,
                  prior_scale, shrink_index, beta)
            : analytic_shrinkage_hessian_for_compat_mode(
                  design_matrix, counts, size, offset, prior_no_shrink_scale,
                  prior_scale, shrink_index, beta, 1.0, compat_mode);
    inv_hessian = invert_matrix_row_major(hessian, p);
  } catch (const Error&) {
    converged = false;
  }

  return {std::move(beta), std::move(inv_hessian), converged};
}

double nbinomFn(std::span<const double> beta,
                const DesignMatrix& design_matrix,
                std::span<const double> counts, double size,
                std::span<const double> offset,
                double prior_no_shrink_scale, double prior_scale,
                std::size_t shrink_index) {
  return negative_binomial_log_posterior_loss(
      beta, design_matrix, counts, size, offset, prior_no_shrink_scale,
      prior_scale, shrink_index);
}

NbinomGlmResult nbinomGLM(
    const DesignMatrix& design_matrix, std::span<const double> counts,
    double size, std::span<const double> offset,
    double prior_no_shrink_scale, double prior_scale,
    std::size_t shrink_index, CompatMode compat_mode) {
  return fit_shrunken_lfc_map(design_matrix, counts, size, offset,
                              prior_no_shrink_scale, prior_scale, shrink_index,
                              compat_mode);
}

LFCFit irls(const CountMatrix& counts, const NormalizedCounts& normalized,
            const DesignMatrix& design_matrix,
            const std::vector<double>& dispersions, const ByteMask& non_zero,
            double min_mu, double beta_tol, int requested_threads,
            bool deterministic) {
  return ccdeseq2::pydeseq2::dds::fit_LFC(
      counts, normalized, design_matrix, dispersions, non_zero, min_mu,
      beta_tol, requested_threads, deterministic);
}

std::vector<double> fit_rough_dispersions(
    const CountMatrix& normed_counts, const DesignMatrix& design_matrix) {
  const std::size_t n = normed_counts.sample_count();
  const std::size_t p = design_matrix.column_count();
  if (design_matrix.sample_count() != n) {
    throw Error(ExitCode::input_error,
                "Design matrix and normalized counts have different sample counts.");
  }
  if (n == p) {
    throw Error(ExitCode::input_error,
                "The number of samples and design variables are equal; there are no "
                "replicates to estimate dispersion.");
  }
  if (n < p) {
    throw Error(ExitCode::input_error,
                "The design matrix has more columns than samples.");
  }

  std::vector<double> y_col_major(n * normed_counts.gene_count(), 0.0);
  for (std::size_t gene = 0; gene < normed_counts.gene_count(); ++gene) {
    for (std::size_t sample = 0; sample < n; ++sample) {
      y_col_major[gene * n + sample] = normed_counts(sample, gene);
    }
  }
  const std::vector<double> beta_by_gene = least_squares_multi_rhs(
      design_matrix.values_row_major(), y_col_major, n, p,
      normed_counts.gene_count());
  std::vector<double> rough(normed_counts.gene_count(),
                            std::numeric_limits<double>::quiet_NaN());

  for (std::size_t gene = 0; gene < normed_counts.gene_count(); ++gene) {
    if (!is_non_zero_gene(normed_counts, gene)) {
      continue;
    }

    const double* beta = beta_by_gene.data() + gene * p;

    double alpha = 0.0;
    for (std::size_t sample = 0; sample < n; ++sample) {
      double y_hat = 0.0;
      for (std::size_t col = 0; col < p; ++col) {
        y_hat += design_matrix(sample, col) * beta[col];
      }
      y_hat = std::max(y_hat, 1.0);
      const double residual = normed_counts(sample, gene) - y_hat;
      alpha += ((residual * residual) - y_hat) /
               (static_cast<double>(n - p) * y_hat * y_hat);
    }
    rough[gene] = std::max(alpha, 0.0);
  }
  return rough;
}

std::vector<double> fit_moments_dispersions(
    const CountMatrix& normed_counts, const std::vector<double>& size_factors) {
  const std::size_t n = normed_counts.sample_count();
  if (size_factors.size() != n) {
    throw Error(ExitCode::input_error,
                "Size factors and normalized counts have different sample counts.");
  }
  if (n < 2) {
    throw Error(ExitCode::input_error,
                "At least two samples are required for moments dispersion.");
  }

  double mean_inv_size = 0.0;
  for (double sf : size_factors) {
    mean_inv_size += 1.0 / sf;
  }
  mean_inv_size /= static_cast<double>(n);

  std::vector<double> moments(normed_counts.gene_count(),
                              std::numeric_limits<double>::quiet_NaN());
  for (std::size_t gene = 0; gene < normed_counts.gene_count(); ++gene) {
    if (!is_non_zero_gene(normed_counts, gene)) {
      continue;
    }

    double mean = 0.0;
    for (std::size_t sample = 0; sample < n; ++sample) {
      mean += normed_counts(sample, gene);
    }
    mean /= static_cast<double>(n);

    double sq = 0.0;
    for (std::size_t sample = 0; sample < n; ++sample) {
      const double delta = normed_counts(sample, gene) - mean;
      sq += delta * delta;
    }
    const double variance = sq / static_cast<double>(n - 1);
    const double alpha = (variance - mean_inv_size * mean) / (mean * mean);
    moments[gene] = nan_to_num(alpha);
  }
  return moments;
}

CountMatrix fit_lin_mu(const CountMatrix& counts,
                       const NormalizedCounts& normalized,
                       const DesignMatrix& design_matrix, double min_mu) {
  const std::size_t n = counts.sample_count();
  const std::size_t p = design_matrix.column_count();
  if (design_matrix.sample_count() != n) {
    throw Error(ExitCode::input_error,
                "Design matrix and counts have different sample counts.");
  }
  CountMatrix mu_hat(counts.sample_names(), counts.gene_names());
  const std::vector<double>& size_factors =
      normalized.sample_wise_size_factors();
  std::vector<double> y_col_major(n * counts.gene_count(), 0.0);
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    for (std::size_t sample = 0; sample < n; ++sample) {
      y_col_major[gene * n + sample] =
          counts(sample, gene) / size_factors[sample];
    }
  }
  const std::vector<double> beta_by_gene = least_squares_multi_rhs(
      design_matrix.values_row_major(), y_col_major, n, p,
      counts.gene_count());

  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    if (!is_non_zero_gene(counts, gene)) {
      for (std::size_t sample = 0; sample < n; ++sample) {
        mu_hat(sample, gene) = std::numeric_limits<double>::quiet_NaN();
      }
      continue;
    }
    const double* beta = beta_by_gene.data() + gene * p;
    for (std::size_t sample = 0; sample < n; ++sample) {
      double fitted = 0.0;
      for (std::size_t col = 0; col < p; ++col) {
        fitted += design_matrix(sample, col) * beta[col];
      }
      mu_hat(sample, gene) = std::max(size_factors[sample] * fitted, min_mu);
    }
  }
  return mu_hat;
}

std::vector<double> robust_method_of_moments_disp(
    const CountMatrix& normed_counts, const DesignMatrix& design_matrix,
    const ByteMask& non_zero) {
  if (design_matrix.sample_count() != normed_counts.sample_count() ||
      non_zero.size() != normed_counts.gene_count()) {
    throw Error(ExitCode::input_error,
                "Cook dispersion inputs have inconsistent dimensions.");
  }
  const ByteMask replaceable3 =
      ccdeseq2::pydeseq2::utils::n_or_more_replicates(design_matrix, 3);
  const auto groups = design_groups(design_matrix);
  std::vector<double> alpha(normed_counts.gene_count(),
                            std::numeric_limits<double>::quiet_NaN());
  for (std::size_t gene = 0; gene < normed_counts.gene_count(); ++gene) {
    if (!non_zero[gene]) {
      continue;
    }
    double mean = 0.0;
    for (std::size_t sample = 0; sample < normed_counts.sample_count();
         ++sample) {
      mean += normed_counts(sample, gene);
    }
    mean /= static_cast<double>(normed_counts.sample_count());
    const double variance =
        robust_variance_for_gene(normed_counts, groups, replaceable3, gene);
    const double estimate = (variance - mean) / (mean * mean);
    alpha[gene] = std::max(estimate, kCookMinDisp);
  }
  return alpha;
}

ByteMask n_or_more_replicates(const DesignMatrix& design_matrix,
                              int min_replicates) {
  const auto groups = design_groups(design_matrix);
  ByteMask result(design_matrix.sample_count(), 0);
  for (const auto& [row, samples] : groups) {
    (void)row;
    if (samples.size() >= static_cast<std::size_t>(min_replicates)) {
      for (const std::size_t sample : samples) {
        result[sample] = 1;
      }
    }
  }
  return result;
}

std::vector<double> lowess(const std::vector<double>& features,
                           const std::vector<double>& targets, double frac,
                           int iterations, bool r_compat_bandwidth) {
  const std::size_t n = features.size();
  std::vector<double> yest(n, 0.0);
  if (n == 0 || targets.size() != n) {
    return yest;
  }

  std::size_t r;
  if (r_compat_bandwidth) {
    int ns_int = static_cast<int>(frac * static_cast<double>(n) + 1e-7);
    if (ns_int < 2) ns_int = 2;
    std::size_t ns = static_cast<std::size_t>(ns_int);
    if (ns > n) ns = n;
    r = ns > 0 ? ns - 1 : 0;
  } else {
    r = static_cast<std::size_t>(std::ceil(frac * static_cast<double>(n)));
    if (r >= n) {
      r = n - 1;
    }
  }
  std::vector<double> h(n, 1e-12);
  std::vector<double> distances(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < n; ++j) {
      distances[j] = std::abs(features[j] - features[i]);
    }
    std::sort(distances.begin(), distances.end());
    h[i] = std::max(distances[r], 1e-12);
  }

  std::vector<double> weights_matrix(n * n, 0.0);
  for (std::size_t row = 0; row < n; ++row) {
    for (std::size_t col = 0; col < n; ++col) {
      double value = (features[row] - features[col]) / h[col];
      if (!std::isfinite(value)) {
        value = 0.0;
      }
      value = std::clamp(std::abs(value), 0.0, 1.0);
      const double one_minus_cube = 1.0 - value * value * value;
      weights_matrix[row * n + col] =
          one_minus_cube * one_minus_cube * one_minus_cube;
    }
  }

  std::vector<double> delta(n, 1.0);
  std::vector<double> residuals(n, 0.0);
  for (int iter = 0; iter < iterations; ++iter) {
    for (std::size_t i = 0; i < n; ++i) {
      double sw = 0.0;
      double swx = 0.0;
      double swxx = 0.0;
      double swy = 0.0;
      double swxy = 0.0;
      for (std::size_t row = 0; row < n; ++row) {
        const double weight = delta[row] * weights_matrix[row * n + i];
        const double x = features[row];
        const double y = targets[row];
        sw += weight;
        swx += weight * x;
        swxx += weight * x * x;
        swy += weight * y;
        swxy += weight * x * y;
      }
      const double det = sw * swxx - swx * swx;
      if (std::abs(det) <= 1e-30 || !std::isfinite(det)) {
        yest[i] = sw > 0.0 ? swy / sw : 0.0;
      } else {
        const double beta0 = (swy * swxx - swx * swxy) / det;
        const double beta1 = (sw * swxy - swx * swy) / det;
        yest[i] = beta0 + beta1 * features[i];
      }
    }

    for (std::size_t i = 0; i < n; ++i) {
      residuals[i] = targets[i] - yest[i];
    }
    std::vector<double> abs_residuals(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
      abs_residuals[i] = std::abs(residuals[i]);
    }
    const double s = quantile_linear(abs_residuals, 0.5);
    for (std::size_t i = 0; i < n; ++i) {
      const double scaled =
          s == 0.0 ? (std::abs(residuals[i]) > 0.0 ? 1.0 : 0.0)
                   : std::clamp(residuals[i] / (6.0 * s), -1.0, 1.0);
      const double one_minus_sq = 1.0 - scaled * scaled;
      delta[i] = one_minus_sq * one_minus_sq;
    }
  }
  return yest;
}

namespace {

// Single local fit at xs over window [nleft, nright] (inclusive), faithful
// to R's stats `lowest` (Cleveland 1979). Returns std::nullopt when the
// weighted neighborhood is empty. The x-spread guard (sqrt(c) > 0.001*range)
// drops the linear term and falls back to the weighted mean, which is what
// prevents the singular-system blow-up the PyDESeq2-derived lowess hits on
// steep numRej curves.
[[nodiscard]] std::optional<double> r_lowest(
    const std::vector<double>& x, const std::vector<double>& y, double xs,
    std::size_t nleft, std::size_t nright, const std::vector<double>& rw,
    bool use_rw) {
  const std::size_t n = x.size();
  const double range = x[n - 1] - x[0];
  const double h = std::max(xs - x[nleft], x[nright] - xs);
  const double h9 = 0.999 * h;
  const double h1 = 0.001 * h;

  std::vector<double> w(n, 0.0);
  double a = 0.0;
  std::size_t nrt = nleft;
  for (std::size_t j = nleft; j < n; ++j) {
    const double r = std::abs(x[j] - xs);
    if (r <= h9) {
      if (r > h1) {
        const double u = r / h;
        const double t = 1.0 - u * u * u;
        w[j] = t * t * t;
      } else {
        w[j] = 1.0;
      }
      if (use_rw) {
        w[j] *= rw[j];
      }
      a += w[j];
      nrt = j;
    } else if (x[j] > xs) {
      break;
    } else {
      nrt = j;
    }
  }
  if (a <= 0.0) {
    return std::nullopt;
  }
  for (std::size_t j = nleft; j <= nrt; ++j) {
    w[j] /= a;
  }
  if (h > 0.0) {
    double xbar = 0.0;
    for (std::size_t j = nleft; j <= nrt; ++j) {
      xbar += w[j] * x[j];
    }
    double b = xs - xbar;
    double c = 0.0;
    for (std::size_t j = nleft; j <= nrt; ++j) {
      const double d = x[j] - xbar;
      c += w[j] * d * d;
    }
    if (std::sqrt(c) > 0.001 * range) {
      b /= c;
      for (std::size_t j = nleft; j <= nrt; ++j) {
        w[j] *= (1.0 + b * (x[j] - xbar));
      }
    }
  }
  double ys = 0.0;
  for (std::size_t j = nleft; j <= nrt; ++j) {
    ys += w[j] * y[j];
  }
  return ys;
}

}  // namespace

std::vector<double> lowess_r_compat(const std::vector<double>& x,
                                    const std::vector<double>& y, double frac,
                                    int nsteps) {
  const std::size_t n = x.size();
  std::vector<double> ys(n, 0.0);
  if (n < 2 || y.size() != n) {
    return n == 0 ? ys : y;
  }
  const double delta = 0.01 * (x[n - 1] - x[0]);
  int ns_int = static_cast<int>(frac * static_cast<double>(n));
  ns_int = std::max(2, std::min(ns_int, static_cast<int>(n)));
  const std::size_t ns = static_cast<std::size_t>(ns_int);

  std::vector<double> rw(n, 0.0);

  for (int it = 0; it <= nsteps; ++it) {
    std::size_t nleft = 0;
    std::size_t nright = ns - 1;
    std::ptrdiff_t last = -1;
    std::size_t i = 0;
    while (true) {
      while (nright < n - 1) {
        const double d1 = x[i] - x[nleft];
        const double d2 = x[nright + 1] - x[i];
        if (d1 <= d2) {
          break;
        }
        ++nleft;
        ++nright;
      }
      const std::optional<double> fit =
          r_lowest(x, y, x[i], nleft, nright, rw, it > 0);
      ys[i] = fit.has_value() ? *fit : y[i];
      if (last < static_cast<std::ptrdiff_t>(i) - 1) {
        const double denom = x[i] - x[static_cast<std::size_t>(last)];
        for (std::size_t j = static_cast<std::size_t>(last + 1); j < i; ++j) {
          const double alpha =
              (x[j] - x[static_cast<std::size_t>(last)]) / denom;
          ys[j] = alpha * ys[i] +
                  (1.0 - alpha) * ys[static_cast<std::size_t>(last)];
        }
      }
      last = static_cast<std::ptrdiff_t>(i);
      const double cut = x[static_cast<std::size_t>(last)] + delta;
      i = static_cast<std::size_t>(last) + 1;
      while (i < n) {
        if (x[i] > cut) {
          break;
        }
        if (x[i] == x[static_cast<std::size_t>(last)]) {
          ys[i] = ys[static_cast<std::size_t>(last)];
          last = static_cast<std::ptrdiff_t>(i);
        }
        ++i;
      }
      const std::size_t next =
          std::max(static_cast<std::size_t>(last) + 1, i - 1);
      i = next;
      if (static_cast<std::size_t>(last) >= n - 1) {
        break;
      }
    }

    if (it >= nsteps) {
      break;
    }

    // Robustness weights from |residuals| (R clowess).
    std::vector<double> abs_res(n);
    for (std::size_t k = 0; k < n; ++k) {
      abs_res[k] = std::abs(y[k] - ys[k]);
    }
    std::vector<double> sorted = abs_res;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t m1 = n / 2;
    const std::size_t m2 = n - m1 - 1;
    const double cmad = 3.0 * (sorted[m1] + sorted[m2]);
    const double c9 = 0.999 * cmad;
    const double c1 = 0.001 * cmad;
    for (std::size_t k = 0; k < n; ++k) {
      const double r = abs_res[k];
      if (cmad == 0.0) {
        rw[k] = 1.0;
      } else if (r <= c1) {
        rw[k] = 1.0;
      } else if (r <= c9) {
        const double u = r / cmad;
        const double t = 1.0 - u * u;
        rw[k] = t * t;
      } else {
        rw[k] = 0.0;
      }
    }
  }
  return ys;
}

}  // namespace ccdeseq2::pydeseq2::utils
