#pragma once

#include <cstddef>
#include <utility>
#include <vector>

#include "ccdeseq2/core_types.hpp"
#include "ccdeseq2/constants.hpp"
#include "ccdeseq2/design.hpp"
#include "ccdeseq2/normalization.hpp"
#include "ccdeseq2/table.hpp"

namespace ccdeseq2 {

struct MoMDispersions {
  ByteMask non_zero;
  std::vector<double> rough;
  std::vector<double> moments;
  std::vector<double> estimates;
};

struct GeneWiseDispersions {
  ByteMask non_zero;
  MoMDispersions mom;
  CountMatrix mu_hat;
  std::vector<double> genewise;
  ByteMask converged;
  std::vector<double> iterations;
};

struct DispersionTrendFit {
  DispersionTrendKind kind = DispersionTrendKind::mean;
  std::vector<double> fitted;
  double a0 = 0.0;
  double a1 = 0.0;
  double mean = 0.0;
  std::vector<double> local_log_means;
  std::vector<double> local_log_disps;
  std::vector<double> local_weights;
  std::vector<double> local_log_fitted;
  bool converged = false;
};

struct DispersionPriorFit {
  std::vector<double> log_residuals;
  double squared_logres = 0.0;
  double prior_disp_var = 0.0;
};

struct MAPDispersions {
  std::vector<double> map;
  std::vector<double> dispersions;
  ByteMask outlier;
  ByteMask converged;
};

struct LFCFit {
  std::vector<double> lfc_row_major;
  CountMatrix mu;
  CountMatrix hat_diagonals;
  ByteMask converged;
  std::vector<double> iterations;
  ByteMask fallback;
};

struct CookOutlierResult {
  CountMatrix cooks;
  ByteMask pvalue_cooks_outlier;
};

struct CookReplacementResult {
  CountMatrix counts;
  CountMatrix replace_cooks;
  ByteMask replaceable_samples;
  ByteMask replaced;
  ByteMask refitted;
  ByteMask new_all_zeroes;
};

struct VstFit {
  DispersionTrendKind kind = DispersionTrendKind::parametric;
  double a0 = 0.0;
  double a1 = 0.0;
  double mean = 0.0;
  bool use_design = false;
  bool converged = false;
};

}  // namespace ccdeseq2

namespace ccdeseq2::pydeseq2::dds {

[[nodiscard]] NormalizedCounts fit_size_factors(const CountMatrix& counts,
                                                SizeFactorFitType fit_type);

[[nodiscard]] MoMDispersions fit_MoM_dispersions(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, double min_disp, double max_disp);

[[nodiscard]] GeneWiseDispersions fit_genewise_dispersions(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, double min_mu, double min_disp,
    double max_disp, int requested_threads = 1, bool deterministic = true,
    double beta_tol = 1e-8,
    CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] DispersionTrendFit fit_parametric_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp, CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] DispersionTrendFit fit_mean_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp);

[[nodiscard]] DispersionTrendFit fit_local_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp);

[[nodiscard]] DispersionTrendFit fit_dispersion_trend(
    const std::vector<double>& genewise_dispersions,
    const ByteMask& non_zero, const std::vector<double>& base_means,
    double min_disp, DispersionTrendKind kind = DispersionTrendKind::parametric,
    CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] std::vector<double> fitted_dispersions_from_trend(
    const DispersionTrendFit& trend, const std::vector<double>& base_means,
    const ByteMask& non_zero);

[[nodiscard]] DispersionPriorFit fit_dispersion_prior(
    const std::vector<double>& genewise_dispersions,
    const std::vector<double>& fitted_dispersions,
    const ByteMask& non_zero, std::size_t num_samples,
    std::size_t num_vars, double min_disp);

[[nodiscard]] MAPDispersions fit_MAP_dispersions(
    const CountMatrix& counts, const DesignMatrix& design_matrix,
    const CountMatrix& mu_hat, const std::vector<double>& genewise_dispersions,
    const std::vector<double>& fitted_dispersions,
    const ByteMask& non_zero, double min_disp, double max_disp,
    double prior_disp_var, double squared_logres, int requested_threads = 1,
    bool deterministic = true,
    CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] LFCFit fit_LFC(const CountMatrix& counts,
                             const NormalizedCounts& normalized,
                             const DesignMatrix& design_matrix,
                             const std::vector<double>& dispersions,
                             const ByteMask& non_zero, double min_mu,
                             double beta_tol, int requested_threads = 1,
                             bool deterministic = true,
                             CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] CountMatrix calculate_cooks(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, const LFCFit& lfc,
    const ByteMask& non_zero, int requested_threads = 1,
    bool deterministic = true);

[[nodiscard]] CookOutlierResult calculate_cooks_outliers(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, const LFCFit& lfc,
    const ByteMask& non_zero, int requested_threads = 1,
    bool deterministic = true);

[[nodiscard]] ByteMask cooks_outlier(
    const CountMatrix& counts, const DesignMatrix& design_matrix,
    const CountMatrix& cooks, const CountMatrix* candidate_cooks = nullptr,
    int requested_threads = 1, bool deterministic = true);

[[nodiscard]] CookReplacementResult replace_outliers(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, const CountMatrix& cooks,
    int min_replicates, int requested_threads = 1, bool deterministic = true);

[[nodiscard]] VstFit vst_fit(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, bool use_design,
    DispersionTrendKind kind, double min_mu, double min_disp, double max_disp,
    double beta_tol, int requested_threads = 1, bool deterministic = true);

[[nodiscard]] CountMatrix vst_transform(
    const CountMatrix& counts, const std::vector<double>& size_factors,
    const VstFit& fit);

[[nodiscard]] std::pair<CountMatrix, VstFit> vst(
    const CountMatrix& counts, const NormalizedCounts& normalized,
    const DesignMatrix& design_matrix, bool use_design,
    DispersionTrendKind kind, double min_mu, double min_disp, double max_disp,
    double beta_tol, int requested_threads = 1, bool deterministic = true);

}  // namespace ccdeseq2::pydeseq2::dds
