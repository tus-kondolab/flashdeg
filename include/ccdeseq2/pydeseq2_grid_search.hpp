#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "ccdeseq2/design.hpp"
#include "ccdeseq2/workspace.hpp"

namespace ccdeseq2::pydeseq2::grid_search {

struct AlphaLineSearchResult {
  double log_alpha = 0.0;
  int iterations = 0;
  int accepted_iterations = 0;
  double initial_log_posterior = 0.0;
  double last_log_posterior = 0.0;
  double last_gradient = 0.0;
  double last_change = -1.0;
};

[[nodiscard]] double grid_fit_alpha(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double alpha_hat, double min_disp,
    double max_disp, std::optional<double> prior_disp_var, bool cox_reid,
    bool prior_reg, ThreadWorkspace& workspace, std::size_t grid_length = 100);

[[nodiscard]] double grid_fit_alpha_deseq2_fallback(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double prior_alpha, double min_disp,
    double max_disp, std::optional<double> prior_disp_var, bool cox_reid,
    bool prior_reg, ThreadWorkspace& workspace, std::size_t grid_length = 20);

[[nodiscard]] AlphaLineSearchResult fit_alpha_deseq2_line_search(
    std::span<const double> counts, const DesignMatrix& design,
    std::span<const double> mu, double initial_alpha, double prior_alpha,
    double min_disp, std::optional<double> prior_disp_var, bool cox_reid,
    bool prior_reg, ThreadWorkspace& workspace, double kappa0 = 1.0,
    double tolerance = 1e-6, int max_iterations = 100);

[[nodiscard]] std::vector<double> grid_fit_beta(
    std::span<const double> counts, const std::vector<double>& size_factors,
    const DesignMatrix& design, double dispersion, double min_mu,
    ThreadWorkspace& workspace);

[[nodiscard]] std::vector<double> grid_fit_shrink_beta(
    std::span<const double> counts, std::span<const double> offset,
    const DesignMatrix& design, double size, double prior_no_shrink_scale,
    double prior_scale, double scale_cnst, std::size_t grid_length = 60,
    double min_beta = -30.0, double max_beta = 30.0);

}  // namespace ccdeseq2::pydeseq2::grid_search
