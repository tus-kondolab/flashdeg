#include "ccdeseq2/pydeseq2_grid_search.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "ccdeseq2/constants.hpp"
#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/linalg.hpp"
#include "ccdeseq2/nb.hpp"
#include "ccdeseq2/numpy_compat.hpp"
#include "ccdeseq2/optimize.hpp"
#include "ccdeseq2/pydeseq2_utils.hpp"
#include "ccdeseq2/special.hpp"

namespace ccdeseq2::pydeseq2::grid_search {
namespace {

constexpr double kMinBeta = -30.0;
constexpr double kMaxBeta = 30.0;
constexpr int kGridLength = 60;

[[nodiscard]] double logdet_xtwx(const DesignMatrix& design,
                                 std::span<const double> weights,
                                 ThreadWorkspace& workspace) {
  const std::size_t n = design.sample_count();
  const std::size_t p = design.column_count();
  if (weights.size() != n) {
    throw Error(ExitCode::numeric_error,
                "Internal error: Cox-Reid weights have an invalid size.");
  }
  std::vector<double>& xtwx = workspace.system_matrix_buffer;
  xtwx.assign(p * p, 0.0);
  for (std::size_t sample = 0; sample < n; ++sample) {
    const double w = weights[sample];
    for (std::size_t i = 0; i < p; ++i) {
      for (std::size_t j = 0; j <= i; ++j) {
        xtwx[i * p + j] += design(sample, i) * w * design(sample, j);
      }
    }
  }
  for (std::size_t i = 0; i < p; ++i) {
    for (std::size_t j = i + 1; j < p; ++j) {
      xtwx[i * p + j] = xtwx[j * p + i];
    }
  }
  return positive_definite_logdet(xtwx, p);
}

[[nodiscard]] double cox_reid_gradient_log_alpha(
    const DesignMatrix& design, std::span<const double> mu, double alpha,
    ThreadWorkspace& workspace) {
  const std::size_t n = design.sample_count();
  const std::size_t p = design.column_count();
  if (mu.size() != n) {
    throw Error(ExitCode::numeric_error,
                "Internal error: Cox-Reid mu has an invalid size.");
  }

  std::vector<double>& xtwx = workspace.system_matrix_buffer;
  std::vector<double>& chol = workspace.chol_factor_buffer;
  xtwx.assign(p * p, 0.0);
  for (std::size_t sample = 0; sample < n; ++sample) {
    const double w = mu[sample] / (1.0 + mu[sample] * alpha);
    for (std::size_t i = 0; i < p; ++i) {
      for (std::size_t j = 0; j <= i; ++j) {
        xtwx[i * p + j] += design(sample, i) * w * design(sample, j);
      }
    }
  }
  for (std::size_t i = 0; i < p; ++i) {
    for (std::size_t j = i + 1; j < p; ++j) {
      xtwx[i * p + j] = xtwx[j * p + i];
    }
  }
  cholesky_decompose_into(xtwx, p, chol);

  std::vector<double>& rhs = workspace.rhs_buffer;
  std::vector<double>& solution = workspace.coefficient_buffer;
  std::vector<double>& temp = workspace.solve_temp_buffer;
  rhs.assign(p, 0.0);
  double trace_term = 0.0;
  for (std::size_t col = 0; col < p; ++col) {
    std::fill(rhs.begin(), rhs.end(), 0.0);
    rhs[col] = 1.0;
    cholesky_solve_from_factor_into(chol, rhs, p, solution, temp);
    for (std::size_t row = 0; row < p; ++row) {
      double dxtwx = 0.0;
      for (std::size_t sample = 0; sample < n; ++sample) {
        const double w = mu[sample] / (1.0 + mu[sample] * alpha);
        const double dw = -(w * w);
        dxtwx += design(sample, row) * dw * design(sample, col);
      }
      trace_term += solution[row] * dxtwx;
    }
  }
  return 0.5 * trace_term * alpha;
}

[[nodiscard]] double alpha_loss(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double log_alpha, double alpha_hat,
    std::optional<double> prior_disp_var, bool cox_reid, bool prior_reg,
    ThreadWorkspace& workspace, bool deseq2_constant_free = false) {
  const double alpha = std::exp(log_alpha);
  double loss = 0.0;
  if (deseq2_constant_free) {
    if (alpha <= 0.0 || counts.size() != mu.size()) {
      return std::numeric_limits<double>::infinity();
    }
    const double alpha_inv = 1.0 / alpha;
    const double lgamma_alpha_inv = gammaln(alpha_inv);
    for (std::size_t i = 0; i < counts.size(); ++i) {
      if (mu[i] <= 0.0 || !std::isfinite(mu[i])) {
        return std::numeric_limits<double>::infinity();
      }
      loss += -gammaln(counts[i] + alpha_inv) + lgamma_alpha_inv +
              counts[i] * std::log(mu[i] + alpha_inv) +
              alpha_inv * std::log1p(mu[i] * alpha);
    }
  } else {
    loss = negative_binomial_nll(counts, mu, alpha);
  }
  if (cox_reid) {
    std::vector<double>& weights = workspace.sample_buffer;
    weights.resize(mu.size());
    for (std::size_t i = 0; i < mu.size(); ++i) {
      weights[i] = mu[i] / (1.0 + mu[i] * alpha);
    }
    const double logdet = logdet_xtwx(design, weights, workspace);
    if (!std::isfinite(logdet)) {
      return std::numeric_limits<double>::infinity();
    }
    loss += 0.5 * logdet;
  }
  if (prior_reg) {
    if (!prior_disp_var.has_value()) {
      throw Error(ExitCode::numeric_error,
                  "prior_disp_var is required for dispersion prior regularization.");
    }
    const double delta = log_alpha - std::log(alpha_hat);
    loss += (delta * delta) / (2.0 * prior_disp_var.value());
  }
  return loss;
}

[[nodiscard]] double alpha_loss_gradient_log_alpha(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double log_alpha, double alpha_hat,
    std::optional<double> prior_disp_var, bool cox_reid, bool prior_reg,
    ThreadWorkspace& workspace) {
  const double alpha = std::exp(log_alpha);
  double gradient =
      alpha * negative_binomial_nll_derivative_alpha(counts, mu, alpha);
  if (cox_reid) {
    try {
      gradient += cox_reid_gradient_log_alpha(design, mu, alpha, workspace);
    } catch (const Error&) {
      return std::numeric_limits<double>::infinity();
    }
  }
  if (prior_reg) {
    if (!prior_disp_var.has_value()) {
      throw Error(ExitCode::numeric_error,
                  "prior_disp_var is required for dispersion prior regularization.");
    }
    gradient += (log_alpha - std::log(alpha_hat)) / prior_disp_var.value();
  }
  return gradient;
}

[[nodiscard]] double linear_predictor(const DesignMatrix& design,
                                      std::size_t sample,
                                      const std::vector<double>& beta) {
  double eta = 0.0;
  for (std::size_t col = 0; col < design.column_count(); ++col) {
    eta += design(sample, col) * beta[col];
  }
  return eta;
}

void compute_mu(const DesignMatrix& design, const std::vector<double>& size_factors,
                const std::vector<double>& beta, double min_mu,
                std::vector<double>& mu) {
  mu.resize(design.sample_count());
  for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
    mu[sample] =
        std::max(size_factors[sample] * std::exp(linear_predictor(design, sample, beta)),
                 min_mu);
  }
}

[[nodiscard]] double beta_loss(std::span<const double> counts,
                               const std::vector<double>& size_factors,
                               const DesignMatrix& design, double dispersion,
                               double min_mu, const std::vector<double>& beta,
                               std::vector<double>& mu) {
  compute_mu(design, size_factors, beta, min_mu, mu);
  double penalty = 0.0;
  for (double value : beta) {
    penalty += kDefaultRidgeFactor * value * value;
  }
  return negative_binomial_nll(counts, mu, dispersion) + 0.5 * penalty;
}

}  // namespace

double grid_fit_alpha(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double alpha_hat, double min_disp,
    double max_disp, std::optional<double> prior_disp_var, bool cox_reid,
    bool prior_reg, ThreadWorkspace& workspace, std::size_t grid_length) {
  struct SearchPoint {
    double log_alpha = std::numeric_limits<double>::quiet_NaN();
    double loss = std::numeric_limits<double>::infinity();
  };

  const double min_log_alpha = std::log(min_disp);
  const double max_log_alpha = std::log(max_disp);
  const double step =
      (max_log_alpha - min_log_alpha) / static_cast<double>(grid_length - 1);
  auto best_grid_point = [&](double begin, double delta) {
    SearchPoint best{begin, std::numeric_limits<double>::infinity()};
    for (std::size_t i = 0; i < grid_length; ++i) {
      const double log_alpha = begin + static_cast<double>(i) * delta;
      const double loss =
          alpha_loss(counts, design, mu, log_alpha, alpha_hat, prior_disp_var,
                     cox_reid, prior_reg, workspace);
      if (loss < best.loss) {
        best = {log_alpha, loss};
      }
    }
    return best;
  };
  auto bounded_loss = [&](double log_alpha) {
    return alpha_loss(counts, design, mu,
                      std::clamp(log_alpha, min_log_alpha, max_log_alpha),
                      alpha_hat, prior_disp_var, cox_reid, prior_reg,
                      workspace);
  };

  double initial_log_alpha = std::log(alpha_hat);
  if (!std::isfinite(initial_log_alpha)) {
    initial_log_alpha = 0.5 * (min_log_alpha + max_log_alpha);
  }
  initial_log_alpha =
      std::clamp(initial_log_alpha, min_log_alpha, max_log_alpha);
  const LbfgsbResult lbfgsb = minimize_l_bfgs_b_scipy(
      [&](std::span<const double> x) { return bounded_loss(x[0]); },
      {initial_log_alpha}, {Bound{min_log_alpha, max_log_alpha}}, {},
      [&](std::span<const double> x, std::vector<double>& gradient) {
        const double log_alpha =
            std::clamp(x[0], min_log_alpha, max_log_alpha);
        gradient.assign(1, alpha_loss_gradient_log_alpha(
                               counts, design, mu, log_alpha, alpha_hat,
                               prior_disp_var, cox_reid, prior_reg, workspace));
      });
  if (lbfgsb.converged && std::isfinite(lbfgsb.value) &&
      lbfgsb.x.size() == 1 && std::isfinite(lbfgsb.x[0])) {
    return std::clamp(lbfgsb.x[0], min_log_alpha, max_log_alpha);
  }

  const SearchPoint coarse = best_grid_point(min_log_alpha, step);
  const double fine_step = (2.0 * step) / static_cast<double>(grid_length - 1);
  const SearchPoint fine = best_grid_point(coarse.log_alpha - step, fine_step);

  // PyDESeq2 falls back to the grid path only when L-BFGS-B fails. Refine the
  // fallback bracket so boundary-sensitive decisions are not pinned to spacing.
  double lo = std::max(min_log_alpha, fine.log_alpha - fine_step);
  double hi = std::min(max_log_alpha, fine.log_alpha + fine_step);
  if (!(lo < hi)) {
    return std::clamp(fine.log_alpha, min_log_alpha, max_log_alpha);
  }

  const ScalarMinimizeResult refined =
      golden_section_minimize(bounded_loss, lo, hi);
  const SearchPoint fallback =
      refined.minimum <= fine.loss
          ? SearchPoint{refined.argmin, refined.minimum}
          : SearchPoint{std::clamp(fine.log_alpha, min_log_alpha, max_log_alpha),
                        fine.loss};
  return fallback.log_alpha;
}

double grid_fit_alpha_deseq2_fallback(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double prior_alpha, double min_disp,
    double max_disp, std::optional<double> prior_disp_var, bool cox_reid,
    bool prior_reg, ThreadWorkspace& workspace, std::size_t grid_length) {
  if (grid_length < 2) {
    throw Error(ExitCode::input_error,
                "DESeq2 dispersion grid fallback requires at least two points.");
  }

  struct SearchPoint {
    double log_alpha = std::numeric_limits<double>::quiet_NaN();
    double log_posterior = -std::numeric_limits<double>::infinity();
  };

  auto evaluate = [&](double log_alpha) {
    return -alpha_loss(counts, design, mu, log_alpha, prior_alpha,
                       prior_disp_var, cox_reid, prior_reg, workspace, true);
  };
  auto best_on_grid = [&](double begin, double delta) {
    SearchPoint best;
    for (std::size_t i = 0; i < grid_length; ++i) {
      const double log_alpha = begin + static_cast<double>(i) * delta;
      const double log_posterior = evaluate(log_alpha);
      if (log_posterior > best.log_posterior) {
        best = {log_alpha, log_posterior};
      }
    }
    return best;
  };

  const double min_log_alpha = std::log(min_disp);
  const double max_log_alpha = std::log(max_disp);
  const double delta =
      (max_log_alpha - min_log_alpha) / static_cast<double>(grid_length - 1);
  const SearchPoint coarse = best_on_grid(min_log_alpha, delta);
  const SearchPoint fine = best_on_grid(coarse.log_alpha - delta,
                                        (2.0 * delta) /
                                            static_cast<double>(grid_length - 1));
  return fine.log_alpha;
}

AlphaLineSearchResult fit_alpha_deseq2_line_search(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double initial_alpha, double prior_alpha,
    double min_disp, std::optional<double> prior_disp_var, bool cox_reid,
    bool prior_reg, ThreadWorkspace& workspace, double kappa0,
    double tolerance, int max_iterations) {
  constexpr double kArmijoEpsilon = 1.0e-4;
  constexpr double kLowerLogAlpha = -30.0;
  constexpr double kUpperLogAlpha = 10.0;

  AlphaLineSearchResult result;
  if (max_iterations <= 0) {
    result.log_alpha = std::log(initial_alpha);
    return result;
  }

  const double min_log_alpha = std::log(min_disp / 10.0);
  auto log_posterior = [&](double log_alpha) {
    return -alpha_loss(counts, design, mu, log_alpha, prior_alpha,
                       prior_disp_var, cox_reid, prior_reg, workspace, true);
  };
  auto dlog_posterior = [&](double log_alpha) {
    return -alpha_loss_gradient_log_alpha(counts, design, mu, log_alpha,
                                          prior_alpha, prior_disp_var,
                                          cox_reid, prior_reg, workspace);
  };

  double a = std::log(initial_alpha);
  if (!std::isfinite(a)) {
    a = std::log(std::max(prior_alpha, min_disp));
  }
  if (!std::isfinite(a)) {
    a = 0.0;
  }

  double lp = log_posterior(a);
  double dlp = dlog_posterior(a);
  double kappa = kappa0;
  double change = -1.0;

  result.initial_log_posterior = lp;
  result.last_log_posterior = lp;
  result.last_gradient = dlp;
  result.last_change = change;

  for (int t = 0; t < max_iterations; ++t) {
    ++result.iterations;
    if (!std::isfinite(lp) || !std::isfinite(dlp) || !std::isfinite(kappa)) {
      break;
    }

    const double a_propose = a + kappa * dlp;
    if (a_propose < kLowerLogAlpha) {
      if (dlp == 0.0) {
        break;
      }
      kappa = (kLowerLogAlpha - a) / dlp;
    }
    if (a_propose > kUpperLogAlpha) {
      if (dlp == 0.0) {
        break;
      }
      kappa = (kUpperLogAlpha - a) / dlp;
    }

    const double next_a = a + kappa * dlp;
    const double theta_kappa = -log_posterior(next_a);
    const double theta_hat_kappa =
        -lp - kappa * kArmijoEpsilon * dlp * dlp;
    if (theta_kappa <= theta_hat_kappa) {
      ++result.accepted_iterations;
      a = next_a;
      const double lpnew = log_posterior(a);
      change = lpnew - lp;
      if (change < tolerance) {
        lp = lpnew;
        break;
      }
      if (a < min_log_alpha) {
        break;
      }
      lp = lpnew;
      dlp = dlog_posterior(a);
      kappa = std::min(kappa * 1.1, kappa0);
      if (result.accepted_iterations % 5 == 0) {
        kappa /= 2.0;
      }
    } else {
      kappa /= 2.0;
    }
  }

  result.log_alpha = a;
  result.last_log_posterior = lp;
  result.last_gradient = dlp;
  result.last_change = change;
  return result;
}

std::vector<double> grid_fit_beta(
    std::span<const double> counts, const std::vector<double>& size_factors,
    const DesignMatrix& design, double dispersion, double min_mu,
    ThreadWorkspace& workspace) {
  const std::size_t p = design.column_count();
  if (p > 2) {
    throw Error(ExitCode::convergence_error,
                "grid beta fallback is implemented only for up to two coefficients.");
  }

  std::vector<double>& beta = workspace.coefficient_step_buffer;
  std::vector<double>& mu = workspace.sample_buffer;
  beta.assign(p, 0.0);
  if (p == 0) {
    return beta;
  }

  auto best_on_x_grid = [&](const std::vector<double>& xs) {
    double best_loss = std::numeric_limits<double>::infinity();
    std::size_t best_i = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
      beta[0] = xs[i];
      const double loss =
          beta_loss(counts, size_factors, design, dispersion, min_mu, beta, mu);
      if (loss < best_loss) {
        best_loss = loss;
        best_i = i;
      }
    }
    return best_i;
  };

  auto best_on_grid = [&](const std::vector<double>& xs,
                          const std::vector<double>& ys) {
    double best_loss = std::numeric_limits<double>::infinity();
    std::size_t best_i = 0;
    std::size_t best_j = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
      beta[0] = xs[i];
      for (std::size_t j = 0; j < ys.size(); ++j) {
        beta[1] = ys[j];
        const double loss =
            beta_loss(counts, size_factors, design, dispersion, min_mu, beta, mu);
        if (loss < best_loss) {
          best_loss = loss;
          best_i = i;
          best_j = j;
        }
      }
    }
    return std::pair<std::size_t, std::size_t>{best_i, best_j};
  };

  const std::vector<double> x_grid = linspace(kMinBeta, kMaxBeta, kGridLength);
  if (p == 1) {
    const std::size_t coarse = best_on_x_grid(x_grid);
    const double delta = x_grid[1] - x_grid[0];
    const std::vector<double> fine_x =
        linspace(x_grid[coarse] - delta, x_grid[coarse] + delta, kGridLength);
    const std::size_t fine = best_on_x_grid(fine_x);
    return {fine_x[fine]};
  }

  const std::vector<double> y_grid = linspace(kMinBeta, kMaxBeta, kGridLength);
  const auto coarse = best_on_grid(x_grid, y_grid);
  const double delta = x_grid[1] - x_grid[0];
  const std::vector<double> fine_x =
      linspace(x_grid[coarse.first] - delta, x_grid[coarse.first] + delta,
               kGridLength);
  const std::vector<double> fine_y =
      linspace(y_grid[coarse.second] - delta, y_grid[coarse.second] + delta,
               kGridLength);
  const auto fine = best_on_grid(fine_x, fine_y);

  return {fine_x[fine.first], fine_y[fine.second]};
}

std::vector<double> grid_fit_shrink_beta(
    std::span<const double> counts, std::span<const double> offset,
    const DesignMatrix& design, double size, double prior_no_shrink_scale,
    double prior_scale, double scale_cnst, std::size_t grid_length,
    double min_beta, double max_beta) {
  if (design.column_count() != 2) {
    throw Error(ExitCode::convergence_error,
                "shrink beta grid fallback is implemented only for two coefficients.");
  }
  if (grid_length < 2) {
    throw Error(ExitCode::input_error,
                "shrink beta grid fallback requires at least two grid points.");
  }

  std::vector<double> beta(2, 0.0);
  auto loss = [&](double x, double y) {
    beta[0] = x;
    beta[1] = y;
    return pydeseq2::utils::nbinomFn(
               beta, design, counts, size, offset, prior_no_shrink_scale,
               prior_scale, 1) /
           scale_cnst;
  };
  auto best_on_grid = [&](const std::vector<double>& xs,
                          const std::vector<double>& ys) {
    double best_loss = std::numeric_limits<double>::infinity();
    std::size_t best_i = 0;
    std::size_t best_j = 0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
      for (std::size_t j = 0; j < ys.size(); ++j) {
        const double value = loss(xs[i], ys[j]);
        if (value < best_loss) {
          best_loss = value;
          best_i = i;
          best_j = j;
        }
      }
    }
    return std::pair<std::size_t, std::size_t>{best_i, best_j};
  };

  const std::vector<double> x_grid = linspace(min_beta, max_beta, grid_length);
  const std::vector<double> y_grid = linspace(min_beta, max_beta, grid_length);
  const auto coarse = best_on_grid(x_grid, y_grid);
  const double delta = x_grid[1] - x_grid[0];
  const std::vector<double> fine_x =
      linspace(x_grid[coarse.first] - delta, x_grid[coarse.first] + delta,
               grid_length);
  const std::vector<double> fine_y =
      linspace(y_grid[coarse.second] - delta, y_grid[coarse.second] + delta,
               grid_length);
  const auto fine = best_on_grid(fine_x, fine_y);
  return {fine_x[fine.first], fine_y[fine.second]};
}

}  // namespace ccdeseq2::pydeseq2::grid_search
