#include "ccdeseq2/pydeseq2_dds.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <span>
#include <string>
#include <utility>

#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/executor.hpp"
#include "ccdeseq2/constants.hpp"
#include "ccdeseq2/linalg.hpp"
#include "ccdeseq2/nb.hpp"
#include "ccdeseq2/numpy_compat.hpp"
#include "ccdeseq2/optimize.hpp"
#include "ccdeseq2/pydeseq2_utils.hpp"
#include "ccdeseq2/pydeseq2_grid_search.hpp"
#include "ccdeseq2/special.hpp"
#include "ccdeseq2/workspace.hpp"

namespace ccdeseq2::pydeseq2::dds {
namespace {

constexpr double kLfcRidge = kDefaultRidgeFactor;
constexpr double kPydeseq2MaxBeta = 30.0;
constexpr double kDeseq2RMaxBeta = 30.0;
constexpr int kMaxIrlsIterations = 250;
constexpr int kDeseq2RMaxIrlsIterations = 100;
constexpr int kDeseq2RMaxDispIterations = 100;
constexpr double kDeseq2RDispTolerance = 1.0e-6;
constexpr double kLog2 = 0.69314718055994530942;

[[nodiscard]] double max_beta_for_compat(CompatMode compat_mode) {
  return compat_mode == CompatMode::deseq2_r ? kDeseq2RMaxBeta
                                             : kPydeseq2MaxBeta;
}

[[nodiscard]] int max_irls_iterations_for_compat(CompatMode compat_mode) {
  return compat_mode == CompatMode::deseq2_r ? kDeseq2RMaxIrlsIterations
                                             : kMaxIrlsIterations;
}

[[nodiscard]] double lfc_ridge_for_compat(CompatMode compat_mode) {
  return compat_mode == CompatMode::deseq2_r ? kLfcRidge / (kLog2 * kLog2)
                                             : kLfcRidge;
}

[[nodiscard]] double clip_dispersion(double value, double min_disp,
                                     double max_disp) {
  if (!std::isfinite(value)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::clamp(value, min_disp, max_disp);
}

[[nodiscard]] DesignMatrix intercept_design(
    const std::vector<std::string>& sample_names) {
  return DesignMatrix(sample_names, {"Intercept"},
                      std::vector<double>(sample_names.size(), 1.0), {});
}

[[nodiscard]] std::size_t distinct_design_row_count(
    const DesignMatrix& design) {
  std::vector<std::vector<double>> rows;
  rows.reserve(design.sample_count());
  for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
    std::vector<double> row;
    row.reserve(design.column_count());
    for (std::size_t col = 0; col < design.column_count(); ++col) {
      row.push_back(design(sample, col));
    }
    rows.push_back(std::move(row));
  }
  std::sort(rows.begin(), rows.end());
  return static_cast<std::size_t>(std::unique(rows.begin(), rows.end()) -
                                  rows.begin());
}

struct LocalDispersionPoint {
  double log_mean = 0.0;
  double log_disp = 0.0;
  double weight = 1.0;
};

[[nodiscard]] double tricube_weight(double scaled_distance) {
  if (scaled_distance >= 1.0) {
    return 0.0;
  }
  const double one_minus_cube =
      1.0 - scaled_distance * scaled_distance * scaled_distance;
  return one_minus_cube * one_minus_cube * one_minus_cube;
}

[[nodiscard]] double local_quadratic_prediction_at(
    const std::vector<LocalDispersionPoint>& points, double x0,
    std::size_t window) {
  const std::size_t n = points.size();
  if (n == 0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (n == 1) {
    return points.front().log_disp;
  }

  const auto it = std::lower_bound(
      points.begin(), points.end(), x0,
      [](const LocalDispersionPoint& point, double value) {
        return point.log_mean < value;
      });
  std::size_t center = static_cast<std::size_t>(it - points.begin());
  if (center >= n) {
    center = n - 1;
  } else if (center > 0 &&
             std::abs(x0 - points[center - 1].log_mean) <
                 std::abs(points[center].log_mean - x0)) {
    --center;
  }

  const std::size_t half = window / 2;
  std::size_t begin = center > half ? center - half : 0;
  std::size_t end = std::min(n, begin + window);
  begin = end > window ? end - window : 0;

  const double radius =
      std::max(std::abs(x0 - points[begin].log_mean),
               std::abs(points[end - 1].log_mean - x0));
  if (radius <= 0.0) {
    double weighted_sum = 0.0;
    double weight_sum = 0.0;
    for (std::size_t i = begin; i < end; ++i) {
      weighted_sum += points[i].weight * points[i].log_disp;
      weight_sum += points[i].weight;
    }
    return weight_sum > 0.0 ? weighted_sum / weight_sum
                            : points[center].log_disp;
  }

  double sw = 0.0;
  double sx = 0.0;
  double sy = 0.0;
  double sxx = 0.0;
  double sxy = 0.0;
  double sxxx = 0.0;
  double sxxxx = 0.0;
  double sxxy = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    const double scaled =
        std::abs(points[i].log_mean - x0) / std::max(radius, 1e-300);
    const double w = points[i].weight * tricube_weight(scaled);
    if (w <= 0.0) {
      continue;
    }
    const double x = points[i].log_mean - x0;
    const double y = points[i].log_disp;
    const double x2 = x * x;
    sw += w;
    sx += w * x;
    sy += w * y;
    sxx += w * x2;
    sxy += w * x * y;
    sxxx += w * x2 * x;
    sxxxx += w * x2 * x2;
    sxxy += w * x2 * y;
  }
  if (sw <= 0.0) {
    return points[center].log_disp;
  }

  try {
    const std::vector<double> xtx{sw, sx, sxx, sx, sxx, sxxx,
                                  sxx, sxxx, sxxxx};
    const std::vector<double> xty{sy, sxy, sxxy};
    const std::vector<double> beta = cholesky_solve(xtx, xty, 3);
    if (std::isfinite(beta[0])) {
      return beta[0];
    }
  } catch (const Error&) {
    // Fall back to local linear below.
  }

  const double denom = sw * sxx - sx * sx;
  if (std::abs(denom) <= 1e-18 * std::max(1.0, sw * sxx)) {
    return sy / sw;
  }
  return (sy * sxx - sx * sxy) / denom;
}

[[nodiscard]] double interpolate_local_log_dispersion(
    const std::vector<LocalDispersionPoint>& points,
    const std::vector<double>& smoothed, double log_mean) {
  if (points.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (points.size() == 1 || log_mean <= points.front().log_mean) {
    return smoothed.front();
  }
  if (log_mean >= points.back().log_mean) {
    return smoothed.back();
  }
  const auto it = std::lower_bound(
      points.begin(), points.end(), log_mean,
      [](const LocalDispersionPoint& point, double value) {
        return point.log_mean < value;
      });
  const std::size_t right = static_cast<std::size_t>(it - points.begin());
  const std::size_t left = right - 1;
  const double x_left = points[left].log_mean;
  const double x_right = points[right].log_mean;
  if (x_right <= x_left) {
    return smoothed[left];
  }
  const double t = (log_mean - x_left) / (x_right - x_left);
  return smoothed[left] + t * (smoothed[right] - smoothed[left]);
}

[[nodiscard]] double interpolate_sorted_values(const std::vector<double>& xs,
                                               const std::vector<double>& ys,
                                               double x) {
  if (xs.empty() || ys.empty() || xs.size() != ys.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (xs.size() == 1 || x <= xs.front()) {
    return ys.front();
  }
  if (x >= xs.back()) {
    return ys.back();
  }
  const auto it = std::lower_bound(xs.begin(), xs.end(), x);
  const std::size_t right = static_cast<std::size_t>(it - xs.begin());
  const std::size_t left = right - 1;
  if (xs[right] <= xs[left]) {
    return ys[left];
  }
  const double t = (x - xs[left]) / (xs[right] - xs[left]);
  return ys[left] + t * (ys[right] - ys[left]);
}

struct GammaGlmFit {
  double a0 = std::numeric_limits<double>::quiet_NaN();
  double a1 = std::numeric_limits<double>::quiet_NaN();
  bool converged = false;
};

[[nodiscard]] double gamma_glm_loss(
    const std::vector<double>& covariates,
    const std::vector<double>& targets, double a0, double a1) {
  double loss = 0.0;
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const double mu = a0 + a1 * covariates[i];
    if (mu <= 0.0 || !std::isfinite(mu)) {
      return std::numeric_limits<double>::infinity();
    }
    loss += targets[i] / mu + std::log(mu);
  }
  return loss / static_cast<double>(targets.size());
}

[[nodiscard]] GammaGlmFit fit_gamma_glm_lbfgsb(
    const std::vector<double>& covariates,
    const std::vector<double>& targets) {
  GammaGlmFit fit;
  if (covariates.size() != targets.size() || targets.empty()) {
    return fit;
  }

  auto objective = [&](std::span<const double> x) {
    return gamma_glm_loss(covariates, targets, x[0], x[1]);
  };
  auto gradient = [&](std::span<const double> x,
                      std::vector<double>& grad) {
    grad.assign(2, 0.0);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const double mu = x[0] + x[1] * covariates[i];
      if (mu <= 0.0 || !std::isfinite(mu)) {
        grad[0] = std::numeric_limits<double>::infinity();
        grad[1] = std::numeric_limits<double>::infinity();
        return;
      }
      const double scale = (mu - targets[i]) / (mu * mu);
      grad[0] += scale;
      grad[1] += scale * covariates[i];
    }
    const double inv_n = 1.0 / static_cast<double>(targets.size());
    grad[0] *= inv_n;
    grad[1] *= inv_n;
  };

  const LbfgsbResult result =
      minimize_l_bfgs_b_scipy(objective, {1.0, 1.0},
                              {Bound{1e-12, INFINITY},
                               Bound{1e-12, INFINITY}},
                              {}, gradient);
  if (result.converged && result.x.size() == 2 &&
      std::isfinite(result.value) && result.x[0] > 0.0 &&
      result.x[1] > 0.0) {
    fit = {result.x[0], result.x[1], true};
  }
  return fit;
}

[[nodiscard]] GammaGlmFit fit_gamma_glm_identity_irls(
    const std::vector<double>& covariates,
    const std::vector<double>& targets, double start_a0, double start_a1) {
  GammaGlmFit fit;
  if (covariates.size() != targets.size() || targets.empty()) {
    return fit;
  }

  double a0 = start_a0;
  double a1 = start_a1;
  for (int iter = 0; iter < 25; ++iter) {
    std::vector<double> weighted_x(targets.size() * 2, 0.0);
    std::vector<double> weighted_y(targets.size(), 0.0);
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const double mu = a0 + a1 * covariates[i];
      if (mu <= 0.0 || !std::isfinite(mu)) {
        return fit;
      }
      const double sqrt_w = 1.0 / mu;
      weighted_x[i * 2 + 0] = sqrt_w;
      weighted_x[i * 2 + 1] = covariates[i] * sqrt_w;
      weighted_y[i] = targets[i] * sqrt_w;
    }

    std::vector<double> next;
    try {
      next = least_squares(weighted_x, weighted_y, targets.size(), 2);
    } catch (const Error&) {
      return fit;
    }
    if (next.size() != 2 || !std::isfinite(next[0]) || !std::isfinite(next[1])) {
      return fit;
    }

    // R-style step halving (stats::glm.fit). The unconstrained weighted least
    // squares step can land at a candidate (a0, a1) where mu = a0 + a1/baseMean
    // is non-positive for some observation, which is invalid for Gamma. R's
    // glm.fit halves the step toward the previous coefficients until validmu
    // holds. Without this safeguard the parametric trend fit on small or
    // noisy active sets bails out and silently falls back to local trend,
    // diverging from DESeq2 R by 10-30% on per-gene dispersions.
    double cand_a0 = next[0];
    double cand_a1 = next[1];
    auto candidate_is_valid = [&](double c0, double c1) {
      for (std::size_t i = 0; i < covariates.size(); ++i) {
        const double cand_mu = c0 + c1 * covariates[i];
        if (!std::isfinite(cand_mu) || cand_mu <= 0.0) {
          return false;
        }
      }
      return true;
    };
    int halvings = 0;
    constexpr int max_halvings = 50;
    while (halvings < max_halvings && !candidate_is_valid(cand_a0, cand_a1)) {
      cand_a0 = (cand_a0 + a0) / 2.0;
      cand_a1 = (cand_a1 + a1) / 2.0;
      ++halvings;
    }
    if (halvings >= max_halvings) {
      return fit;
    }
    if (cand_a0 <= 0.0 || cand_a1 <= 0.0) {
      // Coefficients still non-positive after halving (e.g. sign-flipped but
      // mu stayed positive because covariates compensate). Treat as a valid
      // step toward the solution but skip the log-based convergence check
      // this iteration; the next IRLS step will produce positive candidates
      // once the iterate stabilises in the feasible region.
      a0 = cand_a0;
      a1 = cand_a1;
      continue;
    }
    const double change =
        std::pow(std::log(cand_a0 / a0), 2.0) +
        std::pow(std::log(cand_a1 / a1), 2.0);
    a0 = cand_a0;
    a1 = cand_a1;
    if (change < 1.0e-8) {
      fit = {a0, a1, true};
      return fit;
    }
  }

  fit = {a0, a1, true};
  return fit;
}

[[nodiscard]] GammaGlmFit fit_gamma_glm_newton(
    const std::vector<double>& covariates,
    const std::vector<double>& targets) {
  GammaGlmFit fit;
  if (covariates.size() != targets.size() || targets.empty()) {
    return fit;
  }

  double a0 = 1.0;
  double a1 = 1.0;
  double old_loss = gamma_glm_loss(covariates, targets, a0, a1);
  for (int iter = 0; iter < 100; ++iter) {
    double g0 = 0.0;
    double g1 = 0.0;
    double h00 = 0.0;
    double h01 = 0.0;
    double h11 = 0.0;
    for (std::size_t i = 0; i < targets.size(); ++i) {
      const double x = covariates[i];
      const double t = targets[i];
      const double mu = a0 + a1 * x;
      if (mu <= 0.0 || !std::isfinite(mu)) {
        return fit;
      }
      const double inv_mu = 1.0 / mu;
      const double grad_scale = (mu - t) * inv_mu * inv_mu;
      const double h_scale = (2.0 * t - mu) * inv_mu * inv_mu * inv_mu;
      g0 += grad_scale;
      g1 += grad_scale * x;
      h00 += h_scale;
      h01 += h_scale * x;
      h11 += h_scale * x * x;
    }
    const double inv_n = 1.0 / static_cast<double>(targets.size());
    g0 *= inv_n;
    g1 *= inv_n;
    h00 *= inv_n;
    h01 *= inv_n;
    h11 *= inv_n;
    if (std::max(std::abs(g0), std::abs(g1)) < 1e-8) {
      fit = {a0, a1, true};
      return fit;
    }

    double step0 = g0;
    double step1 = g1;
    const double det = h00 * h11 - h01 * h01;
    if (std::isfinite(det) && std::abs(det) > 1e-18) {
      const double candidate0 = (h11 * g0 - h01 * g1) / det;
      const double candidate1 = (-h01 * g0 + h00 * g1) / det;
      if (candidate0 * g0 + candidate1 * g1 > 0.0) {
        step0 = candidate0;
        step1 = candidate1;
      }
    }

    bool accepted = false;
    for (int backtrack = 0; backtrack < 80; ++backtrack) {
      const double scale = std::ldexp(1.0, -backtrack);
      const double next_a0 = std::max(1e-12, a0 - scale * step0);
      const double next_a1 = std::max(1e-12, a1 - scale * step1);
      const double next_loss =
          gamma_glm_loss(covariates, targets, next_a0, next_a1);
      if (std::isfinite(next_loss) && next_loss <= old_loss) {
        const double change =
            std::max(std::abs(next_a0 - a0), std::abs(next_a1 - a1));
        a0 = next_a0;
        a1 = next_a1;
        accepted = true;
        if (change <=
            1e-10 * std::max({1.0, std::abs(a0), std::abs(a1)})) {
          fit = {a0, a1, true};
          return fit;
        }
        old_loss = next_loss;
        break;
      }
    }
    if (!accepted) {
      fit = {a0, a1, false};
      return fit;
    }
  }
  fit = {a0, a1, true};
  return fit;
}

[[nodiscard]] double linear_predictor(
    const DesignMatrix& design, std::size_t sample,
    const std::vector<double>& beta) {
  double eta = 0.0;
  for (std::size_t col = 0; col < design.column_count(); ++col) {
    eta += design(sample, col) * beta[col];
  }
  return eta;
}

void compute_mu(const DesignMatrix& design,
                        const std::vector<double>& size_factors,
                        const std::vector<double>& beta, double min_mu,
                        std::vector<double>& mu) {
  mu.resize(design.sample_count());
  for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
    mu[sample] = std::max(
        size_factors[sample] *
            std::exp(linear_predictor(design, sample, beta)),
        min_mu);
  }
}

void compute_mu_unthresholded(
    const DesignMatrix& design, const std::vector<double>& size_factors,
    const std::vector<double>& beta, std::vector<double>& mu) {
  mu.resize(design.sample_count());
  for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
    mu[sample] = size_factors[sample] *
                 std::exp(linear_predictor(design, sample, beta));
  }
}

[[nodiscard]] double beta_loss(std::span<const double> counts,
                               const std::vector<double>& size_factors,
                               const DesignMatrix& design, double dispersion,
                               double min_mu,
                               std::span<const double> beta,
                               std::vector<double>& mu) {
  mu.resize(design.sample_count());
  for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
    double eta = 0.0;
    for (std::size_t col = 0; col < design.column_count(); ++col) {
      eta += design(sample, col) * beta[col];
    }
    mu[sample] = std::max(size_factors[sample] * std::exp(eta), min_mu);
  }
  double penalty = 0.0;
  for (double value : beta) {
    penalty += kLfcRidge * value * value;
  }
  return negative_binomial_nll(counts, mu, dispersion) + 0.5 * penalty;
}

[[nodiscard]] double beta_loss_log2_scale(
    std::span<const double> counts, const std::vector<double>& size_factors,
    const DesignMatrix& design, double dispersion,
    std::span<const double> beta_log2, std::vector<double>& mu) {
  mu.resize(design.sample_count());
  for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
    double eta_log2 = 0.0;
    for (std::size_t col = 0; col < design.column_count(); ++col) {
      eta_log2 += design(sample, col) * beta_log2[col];
    }
    mu[sample] = size_factors[sample] * std::exp(kLog2 * eta_log2);
  }
  double penalty = 0.0;
  for (double value : beta_log2) {
    penalty += kLfcRidge * value * value;
  }
  return negative_binomial_nll(counts, mu, dispersion) + 0.5 * penalty;
}

[[nodiscard]] LbfgsbResult fit_beta_lbfgsb(
    std::span<const double> counts, const std::vector<double>& size_factors,
    const DesignMatrix& design, double dispersion, double min_mu,
    const std::vector<double>& beta_init, double max_beta,
    CompatMode compat_mode) {
  if (beta_init.empty()) {
    LbfgsbResult result;
    result.message = "empty beta";
    return result;
  }
  std::vector<double> mu;
  if (compat_mode == CompatMode::deseq2_r) {
    std::vector<double> beta_log2_init(beta_init.size(), 0.0);
    bool use_log2_initial = true;
    for (std::size_t col = 0; col < beta_init.size(); ++col) {
      beta_log2_init[col] = beta_init[col] / kLog2;
      if (!std::isfinite(beta_log2_init[col]) ||
          std::abs(beta_log2_init[col]) >= max_beta) {
        use_log2_initial = false;
      }
    }
    if (!use_log2_initial) {
      // DESeq2's R fallback uses betaMatrix (log2) when stable and within
      // bounds, otherwise the raw fitBeta beta_mat values as the starting
      // vector for the log2-scale optim() objective.
      beta_log2_init = beta_init;
    }

    auto objective = [&](std::span<const double> beta_log2) {
      return beta_loss_log2_scale(counts, size_factors, design, dispersion,
                                  beta_log2, mu);
    };
    std::vector<double> gradient_mu;
    auto gradient = [&](std::span<const double> beta_log2,
                        std::vector<double>& grad) {
      gradient_mu.resize(design.sample_count());
      for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
        double eta_log2 = 0.0;
        for (std::size_t col = 0; col < design.column_count(); ++col) {
          eta_log2 += design(sample, col) * beta_log2[col];
        }
        gradient_mu[sample] = size_factors[sample] * std::exp(kLog2 * eta_log2);
      }

      grad.assign(beta_log2.size(), 0.0);
      const double alpha_inv = 1.0 / dispersion;
      for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
        const double factor =
            kLog2 * (-counts[sample] +
                     (alpha_inv + counts[sample]) * gradient_mu[sample] /
                         (alpha_inv + gradient_mu[sample]));
        for (std::size_t col = 0; col < design.column_count(); ++col) {
          grad[col] += design(sample, col) * factor;
        }
      }
      for (std::size_t col = 0; col < beta_log2.size(); ++col) {
        grad[col] += kLfcRidge * beta_log2[col];
      }
    };
    std::vector<Bound> bounds(beta_log2_init.size(),
                              Bound{-max_beta, max_beta});
    LbfgsbResult result =
        minimize_l_bfgs_b_scipy(objective, beta_log2_init, bounds, {},
                                gradient);
    for (double& value : result.x) {
      value *= kLog2;
    }
    return result;
  }

  auto objective = [&](std::span<const double> beta) {
    return beta_loss(counts, size_factors, design, dispersion, min_mu, beta, mu);
  };
  std::vector<double> gradient_mu;
  auto gradient = [&](std::span<const double> beta, std::vector<double>& grad) {
    gradient_mu.resize(design.sample_count());
    for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
      double eta = 0.0;
      for (std::size_t col = 0; col < design.column_count(); ++col) {
        eta += design(sample, col) * beta[col];
      }
      gradient_mu[sample] =
          std::max(size_factors[sample] * std::exp(eta), min_mu);
    }

    grad.assign(beta.size(), 0.0);
    const double alpha_inv = 1.0 / dispersion;
    for (std::size_t sample = 0; sample < design.sample_count(); ++sample) {
      const double factor =
          -counts[sample] +
          (alpha_inv + counts[sample]) * gradient_mu[sample] /
              (alpha_inv + gradient_mu[sample]);
      for (std::size_t col = 0; col < design.column_count(); ++col) {
        grad[col] += design(sample, col) * factor;
      }
    }
    for (std::size_t col = 0; col < beta.size(); ++col) {
      grad[col] += kLfcRidge * beta[col];
    }
  };
  std::vector<Bound> bounds(beta_init.size(), Bound{-max_beta, max_beta});
  return minimize_l_bfgs_b_scipy(objective, beta_init, bounds, {}, gradient);
}

void fit_initial_beta(std::span<const double> counts,
                              const std::vector<double>& size_factors,
                              const DesignMatrix& design,
                              ThreadWorkspace& workspace,
                              std::vector<double>& beta) {
  const std::size_t n = design.sample_count();
  const std::size_t p = design.column_count();
  beta.assign(p, 0.0);

  std::vector<double>& response = workspace.rhs_buffer;
  response.assign(n, 0.0);
  for (std::size_t sample = 0; sample < n; ++sample) {
    response[sample] = std::log(counts[sample] / size_factors[sample] + 0.1);
  }

  auto assign_rank_deficient_fallback = [&]() {
    beta.assign(p, 0.0);
    double mean_log = 0.0;
    for (std::size_t sample = 0; sample < n; ++sample) {
      mean_log += std::log(counts[sample] / size_factors[sample]);
    }
    beta[0] = mean_log / static_cast<double>(n);
  };

  if (matrix_rank(design.values_row_major(), n, p) != p) {
    assign_rank_deficient_fallback();
    return;
  }

  try {
    beta = least_squares(design.values_row_major(), response, n, p);
  } catch (const Error&) {
    assign_rank_deficient_fallback();
  }
}

struct IrlsResult {
  std::vector<double> beta;
  std::vector<double> mu;
  std::vector<double> hat;
  bool converged = true;
  int iterations = 0;
  bool fallback = false;
};

[[nodiscard]] IrlsResult irls_solver(
    std::span<const double> counts, const std::vector<double>& size_factors,
    const DesignMatrix& design, double dispersion, double min_mu,
    double beta_tol, CompatMode compat_mode, ThreadWorkspace& workspace) {
  const std::size_t n = design.sample_count();
  const std::size_t p = design.column_count();

  IrlsResult result;
  const double max_beta = max_beta_for_compat(compat_mode);
  const int max_iterations = max_irls_iterations_for_compat(compat_mode);
  const double ridge_factor = lfc_ridge_for_compat(compat_mode);
  std::vector<double>& beta = workspace.coefficient_buffer;
  std::vector<double>& beta_hat = workspace.coefficient_step_buffer;
  std::vector<double>& rhs = workspace.rhs_buffer;
  std::vector<double>& temp = workspace.solve_temp_buffer;
  std::vector<double>& hessian = workspace.system_matrix_buffer;
  std::vector<double>& lower = workspace.chol_factor_buffer;
  fit_initial_beta(counts, size_factors, design, workspace, beta);
  const std::vector<double> beta_init = beta;

  std::vector<double> mu;
  std::vector<double> weights(n, 0.0);
  std::vector<double> z(n, 0.0);
  compute_mu(design, size_factors, beta, min_mu, mu);

  double dev = compat_mode == CompatMode::deseq2_r ? 0.0 : 1000.0;
  int iteration = 0;
  bool fallback = false;

  auto solve_irls_step = [&]() {
    for (std::size_t sample = 0; sample < n; ++sample) {
      weights[sample] = mu[sample] / (1.0 + mu[sample] * dispersion);
      z[sample] = std::log(mu[sample] / size_factors[sample]) +
                  (counts[sample] - mu[sample]) / mu[sample];
    }

    if (compat_mode == CompatMode::deseq2_r) {
      hessian.assign((n + p) * p, 0.0);
      rhs.assign(n + p, 0.0);
      for (std::size_t sample = 0; sample < n; ++sample) {
        const double sqrt_w = std::sqrt(weights[sample]);
        rhs[sample] = z[sample] * sqrt_w;
        for (std::size_t col = 0; col < p; ++col) {
          hessian[sample * p + col] = design(sample, col) * sqrt_w;
        }
      }
      const double sqrt_ridge = std::sqrt(ridge_factor);
      for (std::size_t col = 0; col < p; ++col) {
        hessian[(n + col) * p + col] = sqrt_ridge;
      }
      try {
        beta_hat = least_squares(hessian, rhs, n + p, p);
      } catch (const Error&) {
        return false;
      }
      return true;
    }

    hessian.assign(p * p, 0.0);
    rhs.assign(p, 0.0);
    for (std::size_t sample = 0; sample < n; ++sample) {
      const double weighted_z = weights[sample] * z[sample];
      for (std::size_t i = 0; i < p; ++i) {
        const double xi = design(sample, i);
        rhs[i] += xi * weighted_z;
        for (std::size_t j = 0; j <= i; ++j) {
          hessian[i * p + j] += xi * weights[sample] * design(sample, j);
        }
      }
    }
    for (std::size_t i = 0; i < p; ++i) {
      hessian[i * p + i] += ridge_factor;
      for (std::size_t j = i + 1; j < p; ++j) {
        hessian[i * p + j] = hessian[j * p + i];
      }
    }

    try {
      cholesky_decompose_into(hessian, p, lower);
      cholesky_solve_from_factor_into(lower, rhs, p, beta_hat, temp);
    } catch (const Error&) {
      return false;
    }
    return true;
  };

  auto beta_hat_invalid = [&]() {
    return std::any_of(beta_hat.begin(), beta_hat.end(), [](double value) {
             return !std::isfinite(value);
           }) ||
           std::any_of(beta_hat.begin(), beta_hat.end(), [&](double value) {
             return std::abs(value) > max_beta;
           });
  };

  if (compat_mode == CompatMode::deseq2_r) {
    for (int t = 0; t < max_iterations; ++t) {
      if (!solve_irls_step()) {
        fallback = true;
        break;
      }
      ++iteration;
      if (beta_hat_invalid()) {
        fallback = true;
        iteration = max_iterations;
        break;
      }

      beta = beta_hat;
      compute_mu(design, size_factors, beta, min_mu, mu);
      const double dev_old = dev;
      dev = 2.0 * negative_binomial_nll(counts, mu, dispersion);
      const double conv_test = std::abs(dev - dev_old) / (std::abs(dev) + 0.1);
      if (std::isnan(conv_test)) {
        fallback = true;
        iteration = max_iterations;
        break;
      }
      if (t > 0 && conv_test < beta_tol) {
        break;
      }
      if (t + 1 == max_iterations) {
        fallback = true;
      }
    }
  } else {
    double dev_ratio = 1.0;
    while (dev_ratio > beta_tol) {
      if (!solve_irls_step()) {
        fallback = true;
        break;
      }
      ++iteration;

      if (beta_hat_invalid() || iteration >= max_iterations) {
        fallback = true;
        break;
      }

      beta = beta_hat;
      compute_mu(design, size_factors, beta, min_mu, mu);
      const double old_dev = dev;
      dev = -2.0 * negative_binomial_nll(counts, mu, dispersion);
      dev_ratio = std::abs(dev - old_dev) / (std::abs(dev) + 0.1);
    }
  }

  bool converged = true;
  // DESeq2 R stores Cook's leverage values from fitBeta() before the R-level
  // optim fallback updates beta/mu for problematic rows. Preserve the final
  // IRLS mu for hat diagonals in deseq2-r mode while still returning fallback
  // mu and beta for Wald/results.
  std::vector<double> hat_mu = mu;
  if (fallback) {
    std::vector<double> fallback_initial = beta;
    if (fallback_initial.size() != p ||
        !std::all_of(fallback_initial.begin(), fallback_initial.end(),
                     [](double value) { return std::isfinite(value); })) {
      fallback_initial = beta_init;
    }
    beta = beta_init;
    bool inner_converged = false;
    const LbfgsbResult beta_fit = fit_beta_lbfgsb(
        counts, size_factors, design, dispersion, min_mu,
        compat_mode == CompatMode::deseq2_r ? fallback_initial : beta_init,
        max_beta, compat_mode);
    if (beta_fit.converged && beta_fit.x.size() == p &&
        std::isfinite(beta_fit.value)) {
      beta = beta_fit.x;
      inner_converged = true;
    } else if (p <= 2) {
      beta = ccdeseq2::pydeseq2::grid_search::grid_fit_beta(
          counts, size_factors, design, dispersion, min_mu, workspace);
    }
    compute_mu(design, size_factors, beta, min_mu, mu);
    converged = inner_converged;
    if (compat_mode != CompatMode::deseq2_r) {
      hat_mu = mu;
    }
  }

  for (std::size_t sample = 0; sample < n; ++sample) {
    weights[sample] = hat_mu[sample] / (1.0 + hat_mu[sample] * dispersion);
  }
  hessian.assign(p * p, 0.0);
  for (std::size_t sample = 0; sample < n; ++sample) {
    for (std::size_t i = 0; i < p; ++i) {
      const double xi = design(sample, i);
      for (std::size_t j = 0; j <= i; ++j) {
        hessian[i * p + j] += xi * weights[sample] * design(sample, j);
      }
    }
  }
  for (std::size_t i = 0; i < p; ++i) {
    hessian[i * p + i] += ridge_factor;
    for (std::size_t j = i + 1; j < p; ++j) {
      hessian[i * p + j] = hessian[j * p + i];
    }
  }

  std::vector<double> hat(n, std::numeric_limits<double>::quiet_NaN());
  try {
    cholesky_decompose_into(hessian, p, lower);
    std::vector<double>& x_row = rhs;
    std::vector<double>& solved = beta_hat;
    std::vector<double>& solve_temp = temp;
    x_row.assign(p, 0.0);
    for (std::size_t sample = 0; sample < n; ++sample) {
      for (std::size_t col = 0; col < p; ++col) {
        x_row[col] = design(sample, col);
      }
      cholesky_solve_from_factor_into(lower, x_row, p, solved, solve_temp);
      double leverage = 0.0;
      for (std::size_t col = 0; col < p; ++col) {
        leverage += x_row[col] * solved[col];
      }
      hat[sample] = weights[sample] * leverage;
    }
  } catch (const Error&) {
    converged = false;
  }

  compute_mu_unthresholded(design, size_factors, beta, mu);
  result.beta = beta;
  result.mu = mu;
  result.hat = std::move(hat);
  result.converged = converged;
  result.iterations = iteration;
  result.fallback = fallback;
  return result;
}

}  // namespace

NormalizedCounts fit_size_factors(const CountMatrix& counts,
                                  SizeFactorFitType fit_type) {
  return ccdeseq2::fit_size_factors(counts, fit_type);
}

MoMDispersions fit_MoM_dispersions(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, double min_disp, double max_disp) {
  MoMDispersions result;
  result.non_zero.assign(counts.gene_count(), 0);
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    result.non_zero[gene] = static_cast<std::uint8_t>(
        pydeseq2::utils::is_non_zero_gene(counts, gene));
  }

  result.rough =
      pydeseq2::utils::fit_rough_dispersions(normalized.normalized_counts,
                                             design_matrix);
  result.moments = pydeseq2::utils::fit_moments_dispersions(
      normalized.normalized_counts, normalized.sample_wise_size_factors());
  result.estimates.assign(counts.gene_count(),
                          std::numeric_limits<double>::quiet_NaN());

  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    if (!result.non_zero[gene]) {
      continue;
    }
    result.estimates[gene] = clip_dispersion(
        std::min(result.rough[gene], result.moments[gene]), min_disp,
        max_disp);
  }
  return result;
}

GeneWiseDispersions fit_genewise_dispersions(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, double min_mu, double min_disp,
    double max_disp, int requested_threads, bool deterministic,
    double beta_tol, CompatMode compat_mode) {
  GeneWiseDispersions result;
  result.mom =
      fit_MoM_dispersions(counts, normalized, design_matrix, min_disp, max_disp);
  result.non_zero = result.mom.non_zero;
  if (distinct_design_row_count(design_matrix) ==
      design_matrix.column_count()) {
    result.mu_hat =
        pydeseq2::utils::fit_lin_mu(counts, normalized, design_matrix, min_mu);
  } else {
    result.mu_hat =
        fit_LFC(counts, normalized, design_matrix, result.mom.estimates,
                result.non_zero, min_mu, beta_tol, requested_threads,
                deterministic, compat_mode)
            .mu;
    for (std::size_t gene = 0; gene < result.mu_hat.gene_count(); ++gene) {
      if (!result.non_zero[gene]) {
        continue;
      }
      for (std::size_t sample = 0; sample < result.mu_hat.sample_count();
           ++sample) {
        if (result.mu_hat(sample, gene) < min_mu) {
          result.mu_hat(sample, gene) = min_mu;
        }
      }
    }
  }
  result.genewise.assign(counts.gene_count(),
                         std::numeric_limits<double>::quiet_NaN());
  result.converged.assign(counts.gene_count(), 0);
  result.iterations.assign(counts.gene_count(),
                           std::numeric_limits<double>::quiet_NaN());

  const GeneBlockExecutor executor(requested_threads, deterministic);
  executor.run_with_workspace(
      counts.gene_count(), [&](GeneBlock block, ThreadWorkspace& workspace) {
        workspace.sample_buffer.reserve(counts.sample_count());
        workspace.system_matrix_buffer.reserve(design_matrix.column_count() *
                                               design_matrix.column_count());
        for (std::size_t gene = block.begin; gene < block.end; ++gene) {
          if (!result.non_zero[gene]) {
            continue;
          }
          const std::span<const double> counts_gene(counts.gene_data(gene),
                                                    counts.sample_count());
          const std::span<const double> mu_gene(result.mu_hat.gene_data(gene),
                                                result.mu_hat.sample_count());
          if (compat_mode == CompatMode::deseq2_r) {
            const auto fit = grid_search::fit_alpha_deseq2_line_search(
                counts_gene, design_matrix, mu_gene, result.mom.estimates[gene],
                result.mom.estimates[gene], min_disp, std::nullopt, true, false,
                workspace, 1.0, kDeseq2RDispTolerance,
                kDeseq2RMaxDispIterations);
            double estimate = std::min(std::exp(fit.log_alpha), max_disp);
            if (fit.last_log_posterior <
                fit.initial_log_posterior +
                    std::abs(fit.initial_log_posterior) / 1.0e6) {
              estimate = result.mom.estimates[gene];
            }
            const bool converged =
                fit.iterations < kDeseq2RMaxDispIterations &&
                fit.iterations != 1;
            if (!converged && estimate > min_disp * 10.0) {
              const double fallback_log_alpha =
                  grid_search::grid_fit_alpha_deseq2_fallback(
                  counts_gene, design_matrix, mu_gene, result.mom.estimates[gene],
                  min_disp, max_disp, std::nullopt, true, false, workspace, 20);
              estimate = std::exp(fallback_log_alpha);
            }
            result.genewise[gene] =
                clip_dispersion(estimate, min_disp, max_disp);
            result.converged[gene] = static_cast<std::uint8_t>(converged);
            result.iterations[gene] = static_cast<double>(fit.iterations);
          } else {
            const double log_alpha = grid_search::grid_fit_alpha(
                counts_gene, design_matrix, mu_gene, result.mom.estimates[gene],
                min_disp, max_disp, std::nullopt, true, false, workspace);
            result.genewise[gene] =
                clip_dispersion(std::exp(log_alpha), min_disp, max_disp);
            result.converged[gene] = 1;
            result.iterations[gene] = 1.0;
          }
        }
      });
  return result;
}

DispersionTrendFit fit_parametric_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp, CompatMode compat_mode) {
  if (genewise_dispersions.size() != non_zero.size() ||
      genewise_dispersions.size() != base_means.size()) {
    throw Error(ExitCode::input_error,
                "Dispersion trend inputs have inconsistent dimensions.");
  }

  std::vector<std::size_t> active_genes;
  for (std::size_t gene = 0; gene < genewise_dispersions.size(); ++gene) {
    if (non_zero[gene] && base_means[gene] > 0.0 &&
        std::isfinite(base_means[gene]) &&
        std::isfinite(genewise_dispersions[gene]) &&
        (compat_mode != CompatMode::deseq2_r ||
         genewise_dispersions[gene] > 100.0 * min_disp)) {
      active_genes.push_back(gene);
    }
  }

  std::vector<double> old_coeffs{0.1, 0.1};
  std::vector<double> coeffs =
      compat_mode == CompatMode::deseq2_r ? std::vector<double>{0.1, 1.0}
                                          : std::vector<double>{1.0, 1.0};
  GammaGlmFit fit;
  int outer_iterations = 0;
  while ((coeffs[0] > 1e-10 && coeffs[1] > 1e-10) &&
         (std::pow(std::log(std::abs(coeffs[0] / old_coeffs[0])), 2.0) +
              std::pow(std::log(std::abs(coeffs[1] / old_coeffs[1])), 2.0) >=
          1e-6)) {
    std::vector<double> covariates;
    std::vector<double> targets;
    covariates.reserve(active_genes.size());
    targets.reserve(active_genes.size());
    for (std::size_t gene : active_genes) {
      const double prediction = pydeseq2::utils::dispersion_trend(
          base_means[gene], coeffs[0], coeffs[1]);
      const double ratio = genewise_dispersions[gene] / prediction;
      if (compat_mode != CompatMode::deseq2_r ||
          (ratio > 1.0e-4 && ratio < 15.0)) {
        covariates.push_back(1.0 / base_means[gene]);
        targets.push_back(genewise_dispersions[gene]);
      }
    }

    old_coeffs = coeffs;
    if (compat_mode == CompatMode::deseq2_r) {
      fit = fit_gamma_glm_identity_irls(covariates, targets, old_coeffs[0],
                                        old_coeffs[1]);
    } else {
      fit = fit_gamma_glm_lbfgsb(covariates, targets);
      if (!fit.converged) {
        fit = fit_gamma_glm_newton(covariates, targets);
      }
    }
    if (!fit.converged || fit.a0 <= 1e-10 || fit.a1 <= 1e-10) {
      if (compat_mode == CompatMode::deseq2_r) {
        DispersionTrendFit local = fit_local_dispersion_trend(
            genewise_dispersions, non_zero, base_means, min_disp);
        if (local.converged) {
          return local;
        }
      }
      return fit_mean_dispersion_trend(genewise_dispersions, non_zero,
                                       base_means, min_disp);
    }
    coeffs = {fit.a0, fit.a1};
    ++outer_iterations;
    if (outer_iterations > 10) {
      if (compat_mode == CompatMode::deseq2_r) {
        DispersionTrendFit local = fit_local_dispersion_trend(
            genewise_dispersions, non_zero, base_means, min_disp);
        if (local.converged) {
          return local;
        }
      }
      return fit_mean_dispersion_trend(genewise_dispersions, non_zero,
                                       base_means, min_disp);
    }

    if (compat_mode != CompatMode::deseq2_r) {
      std::vector<std::size_t> kept;
      kept.reserve(active_genes.size());
      for (std::size_t gene : active_genes) {
        const double prediction =
            pydeseq2::utils::dispersion_trend(base_means[gene], fit.a0, fit.a1);
        const double ratio = genewise_dispersions[gene] / prediction;
        if (ratio >= 1e-4 && ratio < 15.0) {
          kept.push_back(gene);
        }
      }
      active_genes = std::move(kept);
    }
  }

  DispersionTrendFit result;
  result.kind = DispersionTrendKind::parametric;
  result.a0 = coeffs[0];
  result.a1 = coeffs[1];
  result.converged = true;
  result.fitted.assign(genewise_dispersions.size(),
                       std::numeric_limits<double>::quiet_NaN());
  for (std::size_t gene = 0; gene < genewise_dispersions.size(); ++gene) {
    if (non_zero[gene]) {
      result.fitted[gene] = pydeseq2::utils::dispersion_trend(
          base_means[gene], result.a0, result.a1);
    }
  }
  return result;
}

DispersionTrendFit fit_mean_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp) {
  if (genewise_dispersions.size() != non_zero.size() ||
      genewise_dispersions.size() != base_means.size()) {
    throw Error(ExitCode::input_error,
                "Dispersion trend inputs have inconsistent dimensions.");
  }
  std::vector<double> values;
  for (std::size_t gene = 0; gene < genewise_dispersions.size(); ++gene) {
    if (non_zero[gene] && genewise_dispersions[gene] > 10.0 * min_disp) {
      values.push_back(genewise_dispersions[gene]);
    }
  }

  DispersionTrendFit result;
  result.kind = DispersionTrendKind::mean;
  result.mean = trim_mean(std::move(values), 0.001);
  result.converged = true;
  result.fitted.assign(genewise_dispersions.size(), result.mean);
  return result;
}

DispersionTrendFit fit_local_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp) {
  if (genewise_dispersions.size() != non_zero.size() ||
      genewise_dispersions.size() != base_means.size()) {
    throw Error(ExitCode::input_error,
                "Dispersion trend inputs have inconsistent dimensions.");
  }

  std::vector<LocalDispersionPoint> points;
  points.reserve(genewise_dispersions.size());
  for (std::size_t gene = 0; gene < genewise_dispersions.size(); ++gene) {
    if (non_zero[gene] && std::isfinite(base_means[gene]) &&
        base_means[gene] > 0.0 &&
        std::isfinite(genewise_dispersions[gene]) &&
        genewise_dispersions[gene] >= min_disp * 10.0) {
      points.push_back({std::log(base_means[gene]),
                        std::log(genewise_dispersions[gene]),
                        base_means[gene]});
    }
  }
  std::sort(points.begin(), points.end(),
            [](const LocalDispersionPoint& left,
               const LocalDispersionPoint& right) {
              return left.log_mean < right.log_mean;
            });

  DispersionTrendFit result;
  result.kind = DispersionTrendKind::local;
  result.converged = !points.empty();
  result.fitted.assign(genewise_dispersions.size(),
                       std::numeric_limits<double>::quiet_NaN());
  if (points.empty()) {
    return result;
  }

  if (std::all_of(points.begin(), points.end(), [min_disp](const auto& point) {
        return std::exp(point.log_disp) < min_disp * 10.0;
      })) {
    for (std::size_t gene = 0; gene < genewise_dispersions.size(); ++gene) {
      if (non_zero[gene]) {
        result.fitted[gene] = min_disp;
      }
    }
    return result;
  }

  // DESeq2 R uses locfit(log(disps) ~ log(means), weights=means). FlashDEG
  // keeps the same log-scale/mean-weighted contract with a bounded local
  // linear tricube smoother so the fallback remains cross-platform and fast.
  const std::size_t window = std::min(
      points.size(),
      std::max<std::size_t>(
          3, std::min<std::size_t>(
                  1024, static_cast<std::size_t>(
                            std::ceil(0.7 * static_cast<double>(points.size()))))));
  std::vector<double> smoothed(points.size(),
                               std::numeric_limits<double>::quiet_NaN());
  for (std::size_t i = 0; i < points.size(); ++i) {
    smoothed[i] =
        local_quadratic_prediction_at(points, points[i].log_mean, window);
  }
  result.local_log_means.reserve(points.size());
  result.local_log_disps.reserve(points.size());
  result.local_weights.reserve(points.size());
  result.local_log_fitted.reserve(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double fit = std::exp(smoothed[i]);
    result.local_log_means.push_back(points[i].log_mean);
    result.local_log_disps.push_back(points[i].log_disp);
    result.local_weights.push_back(points[i].weight);
    result.local_log_fitted.push_back(
        std::log(std::isfinite(fit) && fit > 0.0 ? std::max(fit, min_disp)
                                                 : min_disp));
  }

  for (std::size_t gene = 0; gene < genewise_dispersions.size(); ++gene) {
    if (!non_zero[gene] || !std::isfinite(base_means[gene]) ||
        base_means[gene] <= 0.0) {
      continue;
    }
    const double log_fit = interpolate_local_log_dispersion(
        points, smoothed, std::log(base_means[gene]));
    const double direct_log_fit = local_quadratic_prediction_at(
        points, std::log(base_means[gene]), window);
    const double selected_log_fit =
        std::isfinite(direct_log_fit) ? direct_log_fit : log_fit;
    const double fit = std::exp(selected_log_fit);
    result.fitted[gene] =
        std::isfinite(fit) && fit > 0.0 ? std::max(fit, min_disp) : min_disp;
  }
  return result;
}

DispersionTrendFit fit_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp, DispersionTrendKind kind, CompatMode compat_mode) {
  if (kind == DispersionTrendKind::mean) {
    return fit_mean_dispersion_trend(genewise_dispersions, non_zero, base_means,
                                     min_disp);
  }
  if (kind == DispersionTrendKind::local) {
    DispersionTrendFit local = fit_local_dispersion_trend(
        genewise_dispersions, non_zero, base_means, min_disp);
    if (local.converged) {
      return local;
    }
    return fit_mean_dispersion_trend(genewise_dispersions, non_zero, base_means,
                                     min_disp);
  }
  return fit_parametric_dispersion_trend(genewise_dispersions, non_zero,
                                         base_means, min_disp, compat_mode);
}

std::vector<double> fitted_dispersions_from_trend(
    const DispersionTrendFit& trend, const std::vector<double>& base_means,
    const ByteMask& non_zero) {
  if (base_means.size() != non_zero.size()) {
    throw Error(ExitCode::input_error,
                "Dispersion trend evaluation inputs have inconsistent dimensions.");
  }
  std::vector<double> fitted(base_means.size(),
                             std::numeric_limits<double>::quiet_NaN());
  for (std::size_t gene = 0; gene < base_means.size(); ++gene) {
    if (!non_zero[gene]) {
      continue;
    }
    if (trend.kind == DispersionTrendKind::mean) {
      fitted[gene] = trend.mean;
    } else if (trend.kind == DispersionTrendKind::local) {
      if (trend.local_log_means.empty() || trend.local_log_fitted.empty() ||
          trend.local_log_means.size() != trend.local_log_fitted.size()) {
        fitted[gene] = trend.fitted.size() == base_means.size()
                           ? trend.fitted[gene]
                           : std::numeric_limits<double>::quiet_NaN();
      } else if (std::isfinite(base_means[gene]) && base_means[gene] > 0.0) {
        double log_fit = std::numeric_limits<double>::quiet_NaN();
        if (trend.local_log_disps.size() == trend.local_log_means.size() &&
            trend.local_weights.size() == trend.local_log_means.size()) {
          std::vector<LocalDispersionPoint> points;
          points.reserve(trend.local_log_means.size());
          for (std::size_t i = 0; i < trend.local_log_means.size(); ++i) {
            points.push_back({trend.local_log_means[i],
                              trend.local_log_disps[i],
                              trend.local_weights[i]});
          }
          const std::size_t window = std::min(
              points.size(),
              std::max<std::size_t>(
                  3, std::min<std::size_t>(
                         1024, static_cast<std::size_t>(std::ceil(
                                   0.7 * static_cast<double>(points.size()))))));
          log_fit = local_quadratic_prediction_at(
              points, std::log(base_means[gene]), window);
        }
        if (!std::isfinite(log_fit)) {
          log_fit = interpolate_sorted_values(
              trend.local_log_means, trend.local_log_fitted,
              std::log(base_means[gene]));
        }
        const double fit = std::exp(log_fit);
        fitted[gene] = std::isfinite(fit) && fit > 0.0
                           ? fit
                           : std::numeric_limits<double>::quiet_NaN();
      }
    } else {
      fitted[gene] = pydeseq2::utils::dispersion_trend(
          base_means[gene], trend.a0, trend.a1);
    }
  }
  return fitted;
}

DispersionPriorFit fit_dispersion_prior(
    const std::vector<double>& genewise_dispersions,
    const std::vector<double>& fitted_dispersions,
    const ByteMask& non_zero, std::size_t num_samples,
    std::size_t num_vars, double min_disp) {
  if (genewise_dispersions.size() != fitted_dispersions.size() ||
      genewise_dispersions.size() != non_zero.size()) {
    throw Error(ExitCode::input_error,
                "Dispersion prior inputs have inconsistent dimensions.");
  }

  DispersionPriorFit result;
  result.log_residuals.assign(genewise_dispersions.size(),
                              std::numeric_limits<double>::quiet_NaN());
  std::vector<double> above_min_residuals;
  for (std::size_t gene = 0; gene < genewise_dispersions.size(); ++gene) {
    if (!non_zero[gene]) {
      continue;
    }
    const double residual =
        std::log(genewise_dispersions[gene]) - std::log(fitted_dispersions[gene]);
    result.log_residuals[gene] = residual;
    if (genewise_dispersions[gene] >= 100.0 * min_disp) {
      above_min_residuals.push_back(residual);
    }
  }
  const double mad =
      pydeseq2::utils::mean_absolute_deviation(std::move(above_min_residuals));
  result.squared_logres = mad * mad;
  const double residual_dof =
      (static_cast<double>(num_samples) - static_cast<double>(num_vars)) / 2.0;
  result.prior_disp_var =
      std::max(result.squared_logres - trigamma(residual_dof), 0.25);
  return result;
}

MAPDispersions fit_MAP_dispersions(
    const CountMatrix& counts, const DesignMatrix& design_matrix,
    const CountMatrix& mu_hat, const std::vector<double>& genewise_dispersions,
    const std::vector<double>& fitted_dispersions,
    const ByteMask& non_zero, double min_disp, double max_disp,
    double prior_disp_var, double squared_logres, int requested_threads,
    bool deterministic, CompatMode compat_mode) {
  if (counts.gene_count() != genewise_dispersions.size() ||
      counts.gene_count() != fitted_dispersions.size() ||
      counts.gene_count() != non_zero.size()) {
    throw Error(ExitCode::input_error,
                "MAP dispersion inputs have inconsistent dimensions.");
  }

  MAPDispersions result;
  result.map.assign(counts.gene_count(), std::numeric_limits<double>::quiet_NaN());
  result.dispersions.assign(counts.gene_count(),
                            std::numeric_limits<double>::quiet_NaN());
  result.outlier.assign(counts.gene_count(), 0);
  result.converged.assign(counts.gene_count(), 0);

  const GeneBlockExecutor executor(requested_threads, deterministic);
  executor.run_with_workspace(
      counts.gene_count(), [&](GeneBlock block, ThreadWorkspace& workspace) {
        workspace.sample_buffer.reserve(counts.sample_count());
        workspace.system_matrix_buffer.reserve(design_matrix.column_count() *
                                               design_matrix.column_count());
        for (std::size_t gene = block.begin; gene < block.end; ++gene) {
          if (!non_zero[gene]) {
            continue;
          }
          const std::span<const double> counts_gene(counts.gene_data(gene),
                                                    counts.sample_count());
          const std::span<const double> mu_gene(mu_hat.gene_data(gene),
                                                mu_hat.sample_count());
          if (compat_mode == CompatMode::deseq2_r) {
            double initial_alpha =
                genewise_dispersions[gene] > 0.1 * fitted_dispersions[gene]
                    ? genewise_dispersions[gene]
                    : fitted_dispersions[gene];
            if (!std::isfinite(initial_alpha) || initial_alpha <= 0.0) {
              initial_alpha = fitted_dispersions[gene];
            }
            const auto fit = grid_search::fit_alpha_deseq2_line_search(
                counts_gene, design_matrix, mu_gene, initial_alpha,
                fitted_dispersions[gene], min_disp, prior_disp_var, true, true,
                workspace, 1.0, kDeseq2RDispTolerance,
                kDeseq2RMaxDispIterations);
            double estimate = std::exp(fit.log_alpha);
            const bool converged =
                fit.iterations < kDeseq2RMaxDispIterations;
            if (!converged) {
              const double fallback_log_alpha =
                  grid_search::grid_fit_alpha_deseq2_fallback(
                  counts_gene, design_matrix, mu_gene, fitted_dispersions[gene],
                  min_disp, max_disp, prior_disp_var, true, true, workspace, 20);
              estimate = std::exp(fallback_log_alpha);
            }
            result.map[gene] =
                clip_dispersion(estimate, min_disp, max_disp);
            result.converged[gene] = static_cast<std::uint8_t>(converged);
          } else {
            const double log_alpha = grid_search::grid_fit_alpha(
                counts_gene, design_matrix, mu_gene, fitted_dispersions[gene],
                min_disp, max_disp, prior_disp_var, true, true, workspace);
            result.map[gene] =
                clip_dispersion(std::exp(log_alpha), min_disp, max_disp);
            result.converged[gene] = 1;
          }
        }
      });

  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    if (!non_zero[gene]) {
      continue;
    }
    const bool is_outlier =
        std::log(genewise_dispersions[gene]) >
        std::log(fitted_dispersions[gene]) + 2.0 * std::sqrt(squared_logres);
    result.outlier[gene] = static_cast<std::uint8_t>(is_outlier);
    result.dispersions[gene] =
        is_outlier ? genewise_dispersions[gene] : result.map[gene];
  }
  return result;
}

LFCFit fit_LFC(const CountMatrix& counts, const NormalizedCounts& normalized,
               const DesignMatrix& design_matrix,
               const std::vector<double>& dispersions,
               const ByteMask& non_zero, double min_mu, double beta_tol,
               int requested_threads, bool deterministic,
               CompatMode compat_mode) {
  if (dispersions.size() != counts.gene_count() ||
      non_zero.size() != counts.gene_count()) {
    throw Error(ExitCode::input_error,
                "LFC fitting inputs have inconsistent gene counts.");
  }
  if (design_matrix.sample_count() != counts.sample_count()) {
    throw Error(ExitCode::input_error,
                "LFC fitting design matrix sample count does not match counts.");
  }
  if (design_matrix.column_count() == 0) {
    throw Error(ExitCode::input_error,
                "LFC fitting requires at least one design matrix column.");
  }
  const std::vector<double>& size_factors =
      normalized.sample_wise_size_factors();
  if (size_factors.size() != counts.sample_count()) {
    throw Error(ExitCode::input_error,
                "LFC fitting requires sample-wise size factors.");
  }

  LFCFit fit;
  const std::size_t p = design_matrix.column_count();
  fit.lfc_row_major.assign(
      counts.gene_count() * p, std::numeric_limits<double>::quiet_NaN());
  fit.mu = CountMatrix(counts.sample_names(), counts.gene_names());
  fit.hat_diagonals = CountMatrix(counts.sample_names(), counts.gene_names());
  fit.mu.fill(std::numeric_limits<double>::quiet_NaN());
  fit.hat_diagonals.fill(std::numeric_limits<double>::quiet_NaN());
  fit.converged.assign(counts.gene_count(), 0);
  fit.iterations.assign(counts.gene_count(),
                        std::numeric_limits<double>::quiet_NaN());
  fit.fallback.assign(counts.gene_count(), 0);

  const GeneBlockExecutor executor(requested_threads, deterministic);
  executor.run_with_workspace(
      counts.gene_count(), [&](GeneBlock block, ThreadWorkspace& workspace) {
        workspace.reserve_for_design_columns(
            static_cast<unsigned int>(design_matrix.column_count()));
        for (std::size_t gene = block.begin; gene < block.end; ++gene) {
          if (!non_zero[gene]) {
            continue;
          }
          const std::span<const double> counts_gene(counts.gene_data(gene),
                                                    counts.sample_count());
          IrlsResult result = irls_solver(
              counts_gene, size_factors, design_matrix, dispersions[gene],
              min_mu, beta_tol, compat_mode, workspace);
          for (std::size_t col = 0; col < p; ++col) {
            fit.lfc_row_major[gene * p + col] = result.beta[col];
          }
          for (std::size_t sample = 0; sample < counts.sample_count();
               ++sample) {
            fit.mu(sample, gene) = result.mu[sample];
            fit.hat_diagonals(sample, gene) = result.hat[sample];
          }
          fit.converged[gene] = static_cast<std::uint8_t>(result.converged);
          fit.iterations[gene] = static_cast<double>(result.iterations);
          fit.fallback[gene] = static_cast<std::uint8_t>(result.fallback);
        }
      });

  return fit;
}

CountMatrix calculate_cooks(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, const LFCFit& lfc,
    const ByteMask& non_zero, int requested_threads, bool deterministic) {
  const std::size_t samples = counts.sample_count();
  const std::size_t genes = counts.gene_count();
  if (normalized.normalized_counts.sample_count() != samples ||
      normalized.normalized_counts.gene_count() != genes ||
      lfc.mu.sample_count() != samples || lfc.mu.gene_count() != genes ||
      lfc.hat_diagonals.sample_count() != samples ||
      lfc.hat_diagonals.gene_count() != genes ||
      design_matrix.sample_count() != samples || non_zero.size() != genes) {
    throw Error(ExitCode::input_error,
                "Cook distance inputs have inconsistent dimensions.");
  }

  const std::vector<double> dispersions =
      pydeseq2::utils::robust_method_of_moments_disp(
          normalized.normalized_counts, design_matrix, non_zero);
  CountMatrix cooks(counts.sample_names(), counts.gene_names());
  cooks.fill(std::numeric_limits<double>::quiet_NaN());
  const double num_vars = static_cast<double>(design_matrix.column_count());
  const GeneBlockExecutor executor(requested_threads, deterministic);
  executor.run(genes, [&](GeneBlock block) {
    for (std::size_t gene = block.begin; gene < block.end; ++gene) {
      if (!non_zero[gene]) {
        continue;
      }
      const double* counts_gene = counts.gene_data(gene);
      const double* mu_gene = lfc.mu.gene_data(gene);
      const double* hat_gene = lfc.hat_diagonals.gene_data(gene);
      double* cooks_gene = cooks.gene_data(gene);
      for (std::size_t sample = 0; sample < samples; ++sample) {
        const double mu = mu_gene[sample];
        const double variance = mu + dispersions[gene] * mu * mu;
        const double h = hat_gene[sample];
        const double denom = (1.0 - h) * (1.0 - h);
        if (variance <= 0.0 || denom <= 0.0 || !std::isfinite(variance) ||
            !std::isfinite(h)) {
          continue;
        }
        const double residual = counts_gene[sample] - mu;
        cooks_gene[sample] =
            (residual * residual / variance / num_vars) * (h / denom);
      }
    }
  });
  return cooks;
}

CookOutlierResult calculate_cooks_outliers(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, const LFCFit& lfc,
    const ByteMask& non_zero, int requested_threads, bool deterministic) {
  CookOutlierResult result;
  result.cooks = calculate_cooks(counts, normalized, design_matrix, lfc,
                                 non_zero, requested_threads, deterministic);
  result.pvalue_cooks_outlier =
      cooks_outlier(counts, design_matrix, result.cooks, nullptr,
                    requested_threads, deterministic);
  return result;
}

ByteMask cooks_outlier(const CountMatrix& counts,
                       const DesignMatrix& design_matrix,
                       const CountMatrix& cooks,
                       const CountMatrix* candidate_cooks,
                       int requested_threads, bool deterministic) {
  if (counts.sample_count() != cooks.sample_count() ||
      counts.gene_count() != cooks.gene_count() ||
      design_matrix.sample_count() != counts.sample_count() ||
      (candidate_cooks != nullptr &&
       (candidate_cooks->sample_count() != counts.sample_count() ||
        candidate_cooks->gene_count() != counts.gene_count()))) {
    throw Error(ExitCode::input_error,
                "Cook outlier inputs have inconsistent dimensions.");
  }
  ByteMask outlier(counts.gene_count(), 0);
  if (counts.sample_count() <= design_matrix.column_count()) {
    return outlier;
  }
  const double cutoff = f_distribution_quantile(
      0.99, static_cast<double>(design_matrix.column_count()),
      static_cast<double>(counts.sample_count() - design_matrix.column_count()));
  if (!std::isfinite(cutoff)) {
    return outlier;
  }
  const ByteMask use_for_max =
      pydeseq2::utils::n_or_more_replicates(design_matrix, 3);
  const GeneBlockExecutor executor(requested_threads, deterministic);
  executor.run(counts.gene_count(), [&](GeneBlock block) {
    for (std::size_t gene = block.begin; gene < block.end; ++gene) {
      const double* counts_gene = counts.gene_data(gene);
      const double* cooks_gene = cooks.gene_data(gene);
      const double* candidate_gene =
          candidate_cooks == nullptr ? cooks_gene : candidate_cooks->gene_data(gene);
      bool candidate = false;
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        if (use_for_max[sample] && candidate_gene[sample] > cutoff) {
          candidate = true;
          break;
        }
      }
      if (!candidate) {
        continue;
      }

      std::size_t max_pos = 0;
      double max_cook = -std::numeric_limits<double>::infinity();
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        const double value = cooks_gene[sample];
        if (!std::isnan(value) && value > max_cook) {
          max_cook = value;
          max_pos = sample;
        }
      }
      std::size_t larger_counts = 0;
      const double max_count = counts_gene[max_pos];
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        if (counts_gene[sample] > max_count) {
          ++larger_counts;
        }
      }
      outlier[gene] = static_cast<std::uint8_t>(larger_counts < 3);
    }
  });
  return outlier;
}

CookReplacementResult replace_outliers(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, const CountMatrix& cooks,
    int min_replicates, int requested_threads, bool deterministic) {
  if (counts.sample_count() != cooks.sample_count() ||
      counts.gene_count() != cooks.gene_count() ||
      normalized.normalized_counts.sample_count() != counts.sample_count() ||
      normalized.normalized_counts.gene_count() != counts.gene_count() ||
      design_matrix.sample_count() != counts.sample_count()) {
    throw Error(ExitCode::input_error,
                "Cook replacement inputs have inconsistent dimensions.");
  }
  if (counts.sample_count() <= design_matrix.column_count()) {
    CookReplacementResult result;
    result.counts = counts;
    result.replace_cooks = cooks;
    result.replaceable_samples.assign(counts.sample_count(), 0);
    result.replaced.assign(counts.gene_count(), 0);
    result.refitted.assign(counts.gene_count(), 0);
    result.new_all_zeroes.assign(counts.gene_count(), 0);
    return result;
  }

  CookReplacementResult result;
  result.counts = counts;
  result.replace_cooks = cooks;
  result.replaceable_samples =
      pydeseq2::utils::n_or_more_replicates(design_matrix, min_replicates);
  result.replaced.assign(counts.gene_count(), 0);
  result.refitted.assign(counts.gene_count(), 0);
  result.new_all_zeroes.assign(counts.gene_count(), 0);
  if (std::none_of(result.replaceable_samples.begin(),
                   result.replaceable_samples.end(),
                   [](std::uint8_t value) { return value != 0; })) {
    return result;
  }

  const double cutoff = f_distribution_quantile(
      0.99, static_cast<double>(design_matrix.column_count()),
      static_cast<double>(counts.sample_count() - design_matrix.column_count()));
  if (!std::isfinite(cutoff)) {
    return result;
  }
  const std::vector<double>& size_factors = normalized.sample_wise_size_factors();
  const GeneBlockExecutor executor(requested_threads, deterministic);
  executor.run(counts.gene_count(), [&](GeneBlock block) {
    for (std::size_t gene = block.begin; gene < block.end; ++gene) {
      const double* cooks_gene = cooks.gene_data(gene);
      bool replaced = false;
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        if (cooks_gene[sample] > cutoff) {
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        continue;
      }
      result.replaced[gene] = 1;

      std::vector<double> normed_values;
      normed_values.reserve(counts.sample_count());
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        normed_values.push_back(normalized.normalized_counts(sample, gene));
      }
      const double trimmed_base_mean = trim_mean(
          std::move(normed_values), 0.2, EmptyInputPolicy::return_nan);
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        if (result.replaceable_samples[sample] && cooks_gene[sample] > cutoff) {
          const double replacement = trimmed_base_mean * size_factors[sample];
          result.counts(sample, gene) =
              std::isfinite(replacement) && replacement > 0.0
                  ? std::trunc(replacement)
                  : 0.0;
        }
      }

      bool all_zero = true;
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        if (result.counts(sample, gene) != 0.0) {
          all_zero = false;
          break;
        }
      }
      result.new_all_zeroes[gene] = static_cast<std::uint8_t>(all_zero);
      result.refitted[gene] = static_cast<std::uint8_t>(!all_zero);
      if (!all_zero) {
        for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
          if (result.replaceable_samples[sample]) {
            result.replace_cooks(sample, gene) = 0.0;
          }
        }
      }
    }
  });
  return result;
}

VstFit vst_fit(const CountMatrix& counts, const NormalizedCounts& normalized,
               const DesignMatrix& design_matrix, bool use_design,
               DispersionTrendKind kind, double min_mu, double min_disp,
               double max_disp, double beta_tol, int requested_threads,
               bool deterministic) {
  const DesignMatrix vst_design =
      use_design ? design_matrix : intercept_design(counts.sample_names());
  const GeneWiseDispersions genewise = fit_genewise_dispersions(
      counts, normalized, vst_design, min_mu, min_disp, max_disp,
      requested_threads, deterministic, beta_tol);

  VstFit fit;
  fit.kind = kind;
  fit.use_design = use_design;
  if (kind == DispersionTrendKind::parametric) {
    const DispersionTrendFit trend = fit_parametric_dispersion_trend(
        genewise.genewise, genewise.non_zero, normalized.base_means, min_disp);
    fit.a0 = trend.a0;
    fit.a1 = trend.a1;
    fit.converged = trend.converged;
    return fit;
  }
  if (kind == DispersionTrendKind::local) {
    throw Error(ExitCode::unsupported,
                "VST local dispersion trend is reserved; use parametric or mean.");
  }

  std::vector<double> values;
  for (std::size_t gene = 0; gene < genewise.genewise.size(); ++gene) {
    if (genewise.non_zero[gene] != 0 &&
        genewise.genewise[gene] > 10.0 * min_disp) {
      values.push_back(genewise.genewise[gene]);
    }
  }
  fit.mean = trim_mean(std::move(values), 0.001, EmptyInputPolicy::return_nan);
  fit.converged = std::isfinite(fit.mean);
  return fit;
}

CountMatrix vst_transform(const CountMatrix& counts,
                          const std::vector<double>& size_factors,
                          const VstFit& fit) {
  if (size_factors.size() != counts.sample_count()) {
    throw Error(ExitCode::input_error,
                "VST transform requires one size factor per sample.");
  }
  CountMatrix transformed(counts.sample_names(), counts.gene_names());
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
      const double normed = counts(sample, gene) / size_factors[sample];
      if (fit.kind == DispersionTrendKind::parametric) {
        transformed(sample, gene) =
            std::log((1.0 + fit.a1 + 2.0 * fit.a0 * normed +
                      2.0 * std::sqrt(fit.a0 * normed *
                                      (1.0 + fit.a1 + fit.a0 * normed))) /
                     (4.0 * fit.a0)) /
            kLog2;
      } else {
        transformed(sample, gene) =
            (2.0 * std::asinh(std::sqrt(fit.mean * normed)) -
             std::log(fit.mean) - std::log(4.0)) /
            kLog2;
      }
    }
  }
  return transformed;
}

std::pair<CountMatrix, VstFit> vst(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, bool use_design,
    DispersionTrendKind kind, double min_mu, double min_disp, double max_disp,
    double beta_tol, int requested_threads, bool deterministic) {
  VstFit fit = vst_fit(counts, normalized, design_matrix, use_design, kind,
                       min_mu, min_disp, max_disp, beta_tol,
                       requested_threads, deterministic);
  CountMatrix transformed =
      vst_transform(counts, normalized.sample_wise_size_factors(), fit);
  return {std::move(transformed), std::move(fit)};
}

}  // namespace ccdeseq2::pydeseq2::dds
