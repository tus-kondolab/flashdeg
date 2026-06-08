#pragma once

#include <span>
#include <vector>

#include "ccdeseq2/constants.hpp"
#include "ccdeseq2/core_types.hpp"
#include "ccdeseq2/design.hpp"
#include "ccdeseq2/nb.hpp"
#include "ccdeseq2/normalization.hpp"
#include "ccdeseq2/pydeseq2_dds.hpp"
#include "ccdeseq2/table.hpp"

namespace ccdeseq2::pydeseq2::utils {

struct NbinomGlmResult {
  std::vector<double> beta;
  std::vector<double> inv_hessian_row_major;
  bool converged = false;
};

[[nodiscard]] double dispersion_trend(double base_mean, double a0, double a1);

[[nodiscard]] bool is_non_zero_gene(const CountMatrix& counts,
                                    std::size_t gene);

[[nodiscard]] double mean_absolute_deviation(std::vector<double> values);

[[nodiscard]] double nb_nll(std::span<const double> counts,
                            std::span<const double> mu, double alpha);

[[nodiscard]] double dnb_nll(std::span<const double> counts,
                             std::span<const double> mu, double alpha);

[[nodiscard]] double nbinomFn(
    std::span<const double> beta, const DesignMatrix& design_matrix,
    std::span<const double> counts, double size,
    std::span<const double> offset, double prior_no_shrink_scale,
    double prior_scale, std::size_t shrink_index);

[[nodiscard]] NbinomGlmResult nbinomGLM(
    const DesignMatrix& design_matrix, std::span<const double> counts,
    double size, std::span<const double> offset,
    double prior_no_shrink_scale, double prior_scale,
    std::size_t shrink_index = 1,
    CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] LFCFit irls(const CountMatrix& counts,
                          const NormalizedCounts& normalized,
                          const DesignMatrix& design_matrix,
                          const std::vector<double>& dispersions,
                          const ByteMask& non_zero, double min_mu,
                          double beta_tol, int requested_threads = 1,
                          bool deterministic = true);

[[nodiscard]] std::vector<double> fit_rough_dispersions(
    const CountMatrix& normed_counts, const DesignMatrix& design_matrix);

[[nodiscard]] std::vector<double> fit_moments_dispersions(
    const CountMatrix& normed_counts, const std::vector<double>& size_factors);

[[nodiscard]] CountMatrix fit_lin_mu(const CountMatrix& counts,
                                     const NormalizedCounts& normalized,
                                     const DesignMatrix& design_matrix,
                                     double min_mu);

[[nodiscard]] std::vector<double> robust_method_of_moments_disp(
    const CountMatrix& normed_counts, const DesignMatrix& design_matrix,
    const ByteMask& non_zero);

[[nodiscard]] ByteMask n_or_more_replicates(const DesignMatrix& design_matrix,
                                            int min_replicates);

// Mirrors PyDESeq2 utils.lowess(features, targets, frac=2/3, iter=3).
// The C++ parameter is named `iterations` to avoid the Python-specific `iter`.
// When r_compat_bandwidth is true, the neighborhood bandwidth uses
// `ns = int(frac*n + 1e-7), h = sorted_distances[ns - 1]` to match R's
// stats::lowess sliding-window edge bandwidth. Otherwise the PyDESeq2 default
// `r = ceil(frac*n), h = sorted_distances[r]` is used.
[[nodiscard]] std::vector<double> lowess(const std::vector<double>& features,
                                         const std::vector<double>& targets,
                                         double frac = 2.0 / 3.0,
                                         int iterations = 3,
                                         bool r_compat_bandwidth = false);

// Faithful port of R's stats::lowess (Cleveland 1979 clowess/lowest), used
// for DESeq2-R-compatible independent filtering. `x` must be sorted
// ascending. `nsteps` is the number of robustness iterations (R default 3).
// Unlike the PyDESeq2-derived `lowess` above, this uses R's sliding window,
// delta-based interpolation, and the x-spread guard that drops the local
// linear term (falling back to the weighted mean) when the neighborhood
// collapses -- preventing the singular-fit blow-up on steep numRej curves.
[[nodiscard]] std::vector<double> lowess_r_compat(
    const std::vector<double>& x, const std::vector<double>& y, double frac,
    int nsteps);

}  // namespace ccdeseq2::pydeseq2::utils
