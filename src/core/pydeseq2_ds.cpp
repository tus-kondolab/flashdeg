#include "ccdeseq2/pydeseq2_ds.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <utility>

#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/executor.hpp"
#include "ccdeseq2/linalg.hpp"
#include "ccdeseq2/nb.hpp"
#include "ccdeseq2/numpy_compat.hpp"
#include "ccdeseq2/pydeseq2_utils.hpp"
#include "ccdeseq2/special.hpp"
#include "ccdeseq2/workspace.hpp"

namespace ccdeseq2::pydeseq2::ds {
namespace {

constexpr double kLog2 = 0.69314718055994530942;

struct GeneWaldResult {
  double se = std::numeric_limits<double>::quiet_NaN();
  double statistic = std::numeric_limits<double>::quiet_NaN();
  double pvalue = std::numeric_limits<double>::quiet_NaN();
};

[[nodiscard]] double prior_variance_objective(
    const std::vector<double>& squared_lfc,
    const std::vector<double>& squared_se, double variance) {
  double numerator = 0.0;
  double denominator = 0.0;
  for (std::size_t i = 0; i < squared_lfc.size(); ++i) {
    const double coeff =
        1.0 / (2.0 * (variance + squared_se[i]) *
                       (variance + squared_se[i]));
    numerator += (squared_lfc[i] - squared_se[i]) * coeff;
    denominator += coeff;
  }
  return numerator / denominator - variance;
}

[[nodiscard]] double fit_prior_variance(const LFCFit& lfc,
                                        const WaldSummary& summary,
                                        std::size_t coeff_index) {
  constexpr double kMinVar = 1e-6;
  constexpr double kMaxVar = 400.0;
  const std::size_t genes = summary.lfc_se.size();
  if (genes == 0 || lfc.lfc_row_major.size() % genes != 0) {
    throw Error(ExitCode::input_error,
                "LFC shrinkage prior inputs have inconsistent dimensions.");
  }
  const std::size_t cols = lfc.lfc_row_major.size() / genes;
  if (coeff_index >= cols) {
    throw Error(ExitCode::input_error,
                "LFC shrinkage coefficient index is out of range.");
  }

  std::vector<double> squared_lfc;
  std::vector<double> squared_se;
  squared_lfc.reserve(genes);
  squared_se.reserve(genes);
  for (std::size_t gene = 0; gene < genes; ++gene) {
    const double beta = lfc.lfc_row_major[gene * cols + coeff_index];
    const double se = summary.lfc_se[gene] * kLog2;
    if (std::isfinite(beta) && std::isfinite(se)) {
      squared_lfc.push_back(beta * beta);
      squared_se.push_back(se * se);
    }
  }
  if (squared_lfc.empty()) {
    return kMinVar;
  }

  const double at_min =
      prior_variance_objective(squared_lfc, squared_se, kMinVar);
  if (at_min < 0.0) {
    return kMinVar;
  }
  double lo = kMinVar;
  double hi = kMaxVar;
  double f_lo = at_min;
  double f_hi = prior_variance_objective(squared_lfc, squared_se, hi);
  if (!std::isfinite(f_hi) || f_hi > 0.0) {
    return kMaxVar;
  }
  for (int iter = 0; iter < 100; ++iter) {
    const double mid = 0.5 * (lo + hi);
    const double f_mid =
        prior_variance_objective(squared_lfc, squared_se, mid);
    if (!std::isfinite(f_mid)) {
      break;
    }
    if (f_mid == 0.0 || (hi - lo) <= 1e-12 * std::max(1.0, mid)) {
      return mid;
    }
    if ((f_mid > 0.0) == (f_lo > 0.0)) {
      lo = mid;
      f_lo = f_mid;
    } else {
      hi = mid;
      f_hi = f_mid;
      (void)f_hi;
    }
  }
  return 0.5 * (lo + hi);
}

void build_weighted_xtx(const DesignMatrix& design,
                        std::span<const double> mu, double dispersion,
                        double min_mu, std::vector<double>& matrix) {
  const std::size_t n = design.sample_count();
  const std::size_t p = design.column_count();
  matrix.assign(p * p, 0.0);
  for (std::size_t sample = 0; sample < n; ++sample) {
    const double bounded_mu =
        min_mu > 0.0 ? std::max(mu[sample], min_mu) : mu[sample];
    const double weight =
        bounded_mu / (1.0 + bounded_mu * dispersion);
    for (std::size_t i = 0; i < p; ++i) {
      const double xi = design(sample, i);
      for (std::size_t j = 0; j <= i; ++j) {
        matrix[i * p + j] += xi * weight * design(sample, j);
      }
    }
  }
  for (std::size_t i = 0; i < p; ++i) {
    for (std::size_t j = i + 1; j < p; ++j) {
      matrix[i * p + j] = matrix[j * p + i];
    }
  }
}

[[nodiscard]] GeneWaldResult wald_test_gene(
    const DesignMatrix& design, std::span<const double> lfc,
    std::span<const double> mu, double dispersion,
    const std::vector<double>& contrast, const WaldTestOptions& options,
    ThreadWorkspace& workspace) {
  const std::size_t p = design.column_count();
  GeneWaldResult result;

  std::vector<double>& regularized = workspace.system_matrix_buffer;
  std::vector<double>& chol = workspace.chol_factor_buffer;
  std::vector<double>& hc = workspace.coefficient_buffer;
  std::vector<double>& temp = workspace.solve_temp_buffer;
  build_weighted_xtx(design, mu, dispersion, options.min_mu, regularized);
  for (std::size_t i = 0; i < p; ++i) {
    regularized[i * p + i] += options.ridge_factor;
  }

  try {
    cholesky_decompose_into(regularized, p, chol);
    cholesky_solve_from_factor_into(chol, contrast, p, hc, temp);
  } catch (const Error&) {
    result.se = 0.0;
    result.statistic = 0.0;
    result.pvalue = 1.0;
    return result;
  }

  double hc_dot_contrast = 0.0;
  double hc_dot_hc = 0.0;
  for (std::size_t i = 0; i < p; ++i) {
    hc_dot_contrast += hc[i] * contrast[i];
    hc_dot_hc += hc[i] * hc[i];
  }
  double se_squared = hc_dot_contrast - options.ridge_factor * hc_dot_hc;
  if (se_squared < 0.0 && se_squared > -1e-12) {
    se_squared = 0.0;
  }
  result.se = std::sqrt(se_squared);
  if (!std::isfinite(result.se) || result.se <= 0.0) {
    result.se = 0.0;
    result.statistic = 0.0;
    result.pvalue = 1.0;
    return result;
  }

  const double lfc_null = options.lfc_null_log2 * kLog2;
  auto greater = [&](double null_value) {
    double stat = 0.0;
    for (std::size_t i = 0; i < p; ++i) {
      stat += contrast[i] * std::max((lfc[i] - null_value) / result.se, 0.0);
    }
    return std::pair<double, double>{stat, normal_sf(stat)};
  };
  auto less = [&](double null_value) {
    double stat = 0.0;
    for (std::size_t i = 0; i < p; ++i) {
      stat += contrast[i] * std::min((lfc[i] - null_value) / result.se, 0.0);
    }
    return std::pair<double, double>{stat, normal_sf(std::abs(stat))};
  };
  auto greater_abs = [&]() {
    double stat = 0.0;
    for (std::size_t i = 0; i < p; ++i) {
      const double sign = (lfc[i] > 0.0) - (lfc[i] < 0.0);
      stat += contrast[i] * sign *
              std::max((std::abs(lfc[i]) - lfc_null) / result.se, 0.0);
    }
    return std::pair<double, double>{stat, 2.0 * normal_sf(std::abs(stat))};
  };

  switch (options.alternative) {
    case AlternativeHypothesis::greater_abs: {
      const auto [stat, pvalue] = greater_abs();
      result.statistic = stat;
      result.pvalue = pvalue;
      break;
    }
    case AlternativeHypothesis::less_abs: {
      const auto above = greater(-std::abs(lfc_null));
      const auto below = less(std::abs(lfc_null));
      result.statistic =
          std::abs(above.first) <= std::abs(below.first) ? above.first
                                                         : below.first;
      result.pvalue = std::max(above.second, below.second);
      break;
    }
    case AlternativeHypothesis::greater: {
      const auto [stat, pvalue] = greater(lfc_null);
      result.statistic = stat;
      result.pvalue = pvalue;
      break;
    }
    case AlternativeHypothesis::less: {
      const auto [stat, pvalue] = less(lfc_null);
      result.statistic = stat;
      result.pvalue = pvalue;
      break;
    }
    case AlternativeHypothesis::two_sided:
      result.statistic = 0.0;
      for (std::size_t i = 0; i < p; ++i) {
        result.statistic += contrast[i] * (lfc[i] - lfc_null) / result.se;
      }
      result.pvalue = 2.0 * normal_sf(std::abs(result.statistic));
      break;
  }
  result.pvalue = std::clamp(result.pvalue, 0.0, 1.0);
  return result;
}

}  // namespace

WaldSummary run_wald_test(
    const DesignMatrix& design_matrix, const NormalizedCounts& normalized,
    const LFCFit& lfc, const std::vector<double>& dispersions,
    const ByteMask& non_zero, const std::vector<double>& contrast,
    const WaldTestOptions& options) {
  const std::size_t genes = normalized.normalized_counts.gene_count();
  const std::size_t samples = normalized.normalized_counts.sample_count();
  const std::size_t p = design_matrix.column_count();
  if (contrast.size() != p || lfc.lfc_row_major.size() != genes * p ||
      lfc.converged.size() != genes || dispersions.size() != genes ||
      non_zero.size() != genes || lfc.mu.gene_count() != genes ||
      lfc.mu.sample_count() != samples || normalized.base_means.size() != genes ||
      design_matrix.sample_count() != samples ||
      (options.cooks_outlier != nullptr &&
       options.cooks_outlier->size() != genes) ||
      (options.new_all_zeroes != nullptr &&
       options.new_all_zeroes->size() != genes)) {
    throw Error(ExitCode::input_error,
                "Wald test inputs have inconsistent dimensions.");
  }

  WaldSummary result;
  result.base_mean = normalized.base_means;
  result.log2_fold_change.assign(genes,
                                 std::numeric_limits<double>::quiet_NaN());
  result.lfc_se.assign(genes, std::numeric_limits<double>::quiet_NaN());
  result.statistic.assign(genes, std::numeric_limits<double>::quiet_NaN());
  result.pvalue.assign(genes, std::numeric_limits<double>::quiet_NaN());
  result.padj.assign(genes, std::numeric_limits<double>::quiet_NaN());

  for (std::size_t gene = 0; gene < genes; ++gene) {
    bool finite = true;
    double contrast_lfc = 0.0;
    for (std::size_t col = 0; col < p; ++col) {
      const double beta = lfc.lfc_row_major[gene * p + col];
      if (!std::isfinite(beta)) {
        finite = false;
        break;
      }
      contrast_lfc += contrast[col] * beta;
    }
    if (finite) {
      result.log2_fold_change[gene] = contrast_lfc / kLog2;
    }
  }

  const GeneBlockExecutor executor(options.requested_threads,
                                   options.deterministic);
  executor.run_with_workspace(genes, [&](GeneBlock block,
                                         ThreadWorkspace& workspace) {
    workspace.reserve_for_design_columns(
        static_cast<unsigned int>(design_matrix.column_count()));
    for (std::size_t gene = block.begin; gene < block.end; ++gene) {
      if (!non_zero[gene]) {
        continue;
      }
      if (options.new_all_zeroes != nullptr &&
          (*options.new_all_zeroes)[gene] != 0) {
        result.lfc_se[gene] = 0.0;
        result.statistic[gene] = 0.0;
        result.pvalue[gene] = 1.0;
        continue;
      }
      if (!lfc.converged[gene]) {
        result.lfc_se[gene] = 0.0;
        result.statistic[gene] = 0.0;
        result.pvalue[gene] = 1.0;
        continue;
      }
      const std::span<const double> lfc_gene(&lfc.lfc_row_major[gene * p], p);
      const std::span<const double> mu_gene(lfc.mu.gene_data(gene), samples);
      GeneWaldResult gene_result = wald_test_gene(
          design_matrix, lfc_gene, mu_gene, dispersions[gene], contrast,
          options, workspace);
      result.lfc_se[gene] = gene_result.se / kLog2;
      result.statistic[gene] = gene_result.statistic;
      result.pvalue[gene] = gene_result.pvalue;
    }
  });

  if (options.cooks_outlier != nullptr) {
    for (std::size_t gene = 0; gene < genes; ++gene) {
      if ((*options.cooks_outlier)[gene] != 0) {
        result.pvalue[gene] = std::numeric_limits<double>::quiet_NaN();
      }
    }
  }

  // DESeq2 R applies the "new all zero after replacement" override after
  // Cook filtering in results.R, so these genes keep pvalue=1 even if their
  // pre-refit Cook distance exceeded the cutoff.
  if (options.new_all_zeroes != nullptr) {
    for (std::size_t gene = 0; gene < genes; ++gene) {
      if ((*options.new_all_zeroes)[gene] == 0) {
        continue;
      }
      result.lfc_se[gene] = 0.0;
      result.statistic[gene] = 0.0;
      result.pvalue[gene] = 1.0;
    }
  }

  if (options.independent_filter) {
    result.independent_filtering =
        independent_filtering_summary(result.base_mean, result.pvalue,
                                      options.alpha, options.compat_mode);
    result.padj = result.independent_filtering->padj;
  } else {
    result.padj = p_value_adjustment(result.pvalue);
  }
  return result;
}

WaldSummary summary(const DesignMatrix& design_matrix,
                    const NormalizedCounts& normalized, const LFCFit& lfc,
                    const std::vector<double>& dispersions,
                    const ByteMask& non_zero,
                    const std::vector<double>& contrast,
                    const WaldTestOptions& options) {
  return run_wald_test(design_matrix, normalized, lfc, dispersions, non_zero,
                       contrast, options);
}

WaldSummary summary_lrt(const CountMatrix& counts_for_test,
                        const DesignMatrix& full_design,
                        const NormalizedCounts& normalized,
                        const LFCFit& full_lfc, const LFCFit& reduced_lfc,
                        const std::vector<double>& dispersions,
                        const ByteMask& non_zero,
                        const std::vector<double>& report_contrast,
                        const LrtTestOptions& options) {
  const std::size_t genes = normalized.normalized_counts.gene_count();
  const std::size_t samples = normalized.normalized_counts.sample_count();
  if (full_lfc.mu.gene_count() != genes || reduced_lfc.mu.gene_count() != genes ||
      full_lfc.converged.size() != genes ||
      reduced_lfc.converged.size() != genes || dispersions.size() != genes ||
      non_zero.size() != genes || counts_for_test.gene_count() != genes ||
      counts_for_test.sample_count() != samples ||
      full_lfc.mu.sample_count() != samples ||
      reduced_lfc.mu.sample_count() != samples ||
      (options.cooks_outlier != nullptr &&
       options.cooks_outlier->size() != genes) ||
      (options.new_all_zeroes != nullptr &&
       options.new_all_zeroes->size() != genes)) {
    throw Error(ExitCode::input_error,
                "LRT summary inputs have inconsistent dimensions.");
  }

  // 1. Reporting columns (baseMean / log2FoldChange / lfcSE) come from the
  //    full-model Wald test on the display contrast. Filtering and the Cook /
  //    new-all-zero masks are disabled here; the LRT overwrites the rest. The
  //    extra Wald pass is cheap relative to the GLM fits and keeps the existing
  //    Wald SE path untouched.
  WaldTestOptions report_opts;
  report_opts.alpha = options.alpha;
  report_opts.ridge_factor = options.ridge_factor;
  report_opts.min_mu = options.min_mu;
  report_opts.alternative = AlternativeHypothesis::two_sided;
  report_opts.independent_filter = false;
  report_opts.cooks_outlier = nullptr;
  report_opts.new_all_zeroes = nullptr;
  report_opts.requested_threads = options.requested_threads;
  report_opts.deterministic = options.deterministic;
  report_opts.compat_mode = options.compat_mode;
  WaldSummary result =
      run_wald_test(full_design, normalized, full_lfc, dispersions, non_zero,
                    report_contrast, report_opts);

  // 2. LRT statistic and p-value per gene, overwriting the Wald stat / pvalue.
  const double df = static_cast<double>(options.degrees_of_freedom);
  const GeneBlockExecutor executor(options.requested_threads,
                                   options.deterministic);
  executor.run(genes, [&](GeneBlock block) {
    for (std::size_t gene = block.begin; gene < block.end; ++gene) {
      result.statistic[gene] = std::numeric_limits<double>::quiet_NaN();
      result.pvalue[gene] = std::numeric_limits<double>::quiet_NaN();
      if (!non_zero[gene]) {
        continue;
      }
      // Evaluate the new-all-zero mask before the NB likelihood: the full and
      // reduced means need not agree for these genes, so skip the NLL and report
      // "no evidence" (matches DESeq2's post-replacement override).
      if (options.new_all_zeroes != nullptr &&
          (*options.new_all_zeroes)[gene] != 0) {
        result.lfc_se[gene] = 0.0;
        result.statistic[gene] = 0.0;
        result.pvalue[gene] = 1.0;
        continue;
      }
      // Untestable genes stay NA (not pvalue=1) so they leave the BH /
      // independent-filtering pool, matching DESeq2's LRT behaviour.
      if (!full_lfc.converged[gene] || !reduced_lfc.converged[gene]) {
        continue;
      }
      const double dispersion = dispersions[gene];
      if (!std::isfinite(dispersion)) {
        continue;
      }
      const std::span<const double> counts_gene(counts_for_test.gene_data(gene),
                                                samples);
      const std::span<const double> mu_full(full_lfc.mu.gene_data(gene), samples);
      const std::span<const double> mu_reduced(reduced_lfc.mu.gene_data(gene),
                                               samples);
      const double full_nll =
          negative_binomial_nll(counts_gene, mu_full, dispersion);
      const double reduced_nll =
          negative_binomial_nll(counts_gene, mu_reduced, dispersion);
      if (!std::isfinite(full_nll) || !std::isfinite(reduced_nll)) {
        continue;
      }
      // Keep the raw statistic, including small negatives. A correctly nested
      // model gives statistic >= 0 in theory, but the NB GLM fit can produce a
      // tiny negative on pathological genes (very low counts, Cook-replaced
      // cells). DESeq2 reports those raw as well (verified: matches DESeq2 to
      // ~1e-4 on such genes), so keeping them maximizes compatibility;
      // chi_square_sf maps any value <= 0 to pvalue = 1. Nestedness itself is
      // guaranteed upstream by validate_nested_designs.
      const double statistic = 2.0 * (reduced_nll - full_nll);
      result.statistic[gene] = statistic;
      result.pvalue[gene] = chi_square_sf(statistic, df);
    }
  });

  // 3. Cook filtering (NaN) then 4. new-all-zero override (pvalue=1), mirroring
  //    the exact ordering of run_wald_test.
  if (options.cooks_outlier != nullptr) {
    for (std::size_t gene = 0; gene < genes; ++gene) {
      if ((*options.cooks_outlier)[gene] != 0) {
        result.pvalue[gene] = std::numeric_limits<double>::quiet_NaN();
      }
    }
  }
  if (options.new_all_zeroes != nullptr) {
    for (std::size_t gene = 0; gene < genes; ++gene) {
      if ((*options.new_all_zeroes)[gene] == 0) {
        continue;
      }
      result.lfc_se[gene] = 0.0;
      result.statistic[gene] = 0.0;
      result.pvalue[gene] = 1.0;
    }
  }

  // 5. Multiple-testing correction on the LRT p-values.
  if (options.independent_filter) {
    result.independent_filtering = independent_filtering_summary(
        result.base_mean, result.pvalue, options.alpha, options.compat_mode);
    result.padj = result.independent_filtering->padj;
  } else {
    result.independent_filtering.reset();
    result.padj = p_value_adjustment(result.pvalue);
  }
  return result;
}

std::vector<double> contrast_log2_fold_change(
    const LFCFit& fit, const std::vector<double>& contrast) {
  if (contrast.empty() || fit.lfc_row_major.size() % contrast.size() != 0) {
    throw Error(ExitCode::input_error,
                "Contrast vector length does not match LFC matrix columns.");
  }
  const std::size_t genes = fit.lfc_row_major.size() / contrast.size();
  std::vector<double> values(genes, std::numeric_limits<double>::quiet_NaN());
  for (std::size_t gene = 0; gene < genes; ++gene) {
    double value = 0.0;
    bool finite = true;
    for (std::size_t col = 0; col < contrast.size(); ++col) {
      const double beta = fit.lfc_row_major[gene * contrast.size() + col];
      if (!std::isfinite(beta)) {
        finite = false;
        break;
      }
      value += beta * contrast[col];
    }
    if (finite) {
      values[gene] = value / kLog2;
    }
  }
  return values;
}

std::vector<double> p_value_adjustment(const std::vector<double>& pvalues) {
  struct IndexedPvalue {
    double value = 0.0;
    std::size_t index = 0;
  };
  std::vector<IndexedPvalue> finite;
  finite.reserve(pvalues.size());
  std::vector<double> adjusted(pvalues.size(),
                               std::numeric_limits<double>::quiet_NaN());
  for (std::size_t i = 0; i < pvalues.size(); ++i) {
    if (!std::isnan(pvalues[i])) {
      finite.push_back({pvalues[i], i});
    }
  }
  std::stable_sort(finite.begin(), finite.end(),
                   [](const IndexedPvalue& lhs, const IndexedPvalue& rhs) {
                     return lhs.value < rhs.value;
                   });

  const double n = static_cast<double>(finite.size());
  double cumulative_min = std::numeric_limits<double>::infinity();
  for (std::size_t reverse = 0; reverse < finite.size(); ++reverse) {
    const std::size_t sorted_index = finite.size() - 1 - reverse;
    const double rank = static_cast<double>(sorted_index + 1);
    const double raw = finite[sorted_index].value * n / rank;
    cumulative_min = std::min(cumulative_min, raw);
    adjusted[finite[sorted_index].index] =
        std::clamp(cumulative_min, 0.0, 1.0);
  }
  return adjusted;
}

IndependentFilteringResult independent_filtering_summary(
    const std::vector<double>& base_mean, const std::vector<double>& pvalues,
    double alpha, CompatMode compat_mode) {
  const std::size_t n = pvalues.size();
  if (base_mean.size() != n) {
    throw Error(ExitCode::input_error,
                "Independent filtering inputs have inconsistent lengths.");
  }
  IndependentFilteringResult result;
  result.padj.assign(n, std::numeric_limits<double>::quiet_NaN());
  result.independent_filter_alpha = alpha;
  if (n == 0) {
    return result;
  }

  const std::size_t zero_base = static_cast<std::size_t>(
      std::count(base_mean.begin(), base_mean.end(), 0.0));
  const double lower_quantile =
      static_cast<double>(zero_base) / static_cast<double>(n);
  const double upper_quantile = lower_quantile < 0.95 ? 0.95 : 1.0;
  const std::vector<double> theta =
      linspace(lower_quantile, upper_quantile, 50);
  result.independent_filter_theta = theta;
  std::vector<double> sorted_base_mean = base_mean;
  std::sort(sorted_base_mean.begin(), sorted_base_mean.end());
  result.independent_filter_cutoff.reserve(theta.size());

  std::vector<std::vector<double>> adjusted_by_theta(
      theta.size(),
      std::vector<double>(n, std::numeric_limits<double>::quiet_NaN()));
  std::vector<double> num_rej(theta.size(), 0.0);
  for (std::size_t i = 0; i < theta.size(); ++i) {
    const double cutoff = quantile_from_sorted(sorted_base_mean, theta[i]);
    result.independent_filter_cutoff.push_back(cutoff);
    std::vector<double> filtered(n, std::numeric_limits<double>::quiet_NaN());
    for (std::size_t gene = 0; gene < n; ++gene) {
      if (base_mean[gene] >= cutoff && !std::isnan(pvalues[gene])) {
        filtered[gene] = pvalues[gene];
      }
    }
    adjusted_by_theta[i] = p_value_adjustment(filtered);
    num_rej[i] = static_cast<double>(std::count_if(
        adjusted_by_theta[i].begin(), adjusted_by_theta[i].end(),
        [alpha](double value) { return !std::isnan(value) && value < alpha; }));
  }
  result.independent_filter_num_rej = num_rej;

  const auto max_rej = *std::max_element(num_rej.begin(), num_rej.end());
  std::size_t selected = 0;
  result.independent_filter_lo_fit.assign(theta.size(),
                                          std::numeric_limits<double>::quiet_NaN());
  if (max_rej > 10.0) {
    const bool r_compat = (compat_mode == CompatMode::deseq2_r);
    // DESeq2 R calls stats::lowess(theta, numRej, f = 1/5) (iter = 3 default).
    // Use a faithful port of that algorithm in deseq2-r compat mode; the
    // PyDESeq2-derived lowess (iter = 3) is kept for pydeseq2 compat mode.
    // The earlier iter=4 + r_compat_bandwidth patch matched TCGA-BRCA but was
    // numerically unstable on large/steep numRej curves (blew up to ~2^16 on
    // a 4,500-sample GTEx-like dataset), so it is replaced here.
    const std::vector<double> smooth =
        r_compat ? pydeseq2::utils::lowess_r_compat(theta, num_rej, 1.0 / 5.0, 3)
                 : pydeseq2::utils::lowess(theta, num_rej, 1.0 / 5.0, 3);
    result.independent_filter_lo_fit = smooth;
    double residual_square_sum = 0.0;
    std::size_t residual_count = 0;
    for (std::size_t i = 0; i < num_rej.size(); ++i) {
      if (num_rej[i] > 0.0) {
        const double residual = num_rej[i] - smooth[i];
        residual_square_sum += residual * residual;
        ++residual_count;
      }
    }
    const double rmse =
        residual_count == 0
            ? 0.0
            : std::sqrt(residual_square_sum /
                        static_cast<double>(residual_count));
    const double smooth_max = *std::max_element(smooth.begin(), smooth.end());
    const double threshold = smooth_max - rmse;
    result.independent_filter_rmse = rmse;
    result.independent_filter_max_fit = smooth_max;
    result.independent_filter_threshold = threshold;

    const auto first_above = [&num_rej](double value) -> std::optional<std::size_t> {
      for (std::size_t i = 0; i < num_rej.size(); ++i) {
        if (num_rej[i] > value) {
          return i;
        }
      }
      return std::nullopt;
    };

    if (const auto threshold_index = first_above(threshold);
        threshold_index.has_value()) {
      selected = *threshold_index;
    } else if (compat_mode == CompatMode::deseq2_r) {
      if (const auto ninety_percent_index = first_above(0.9 * smooth_max);
          ninety_percent_index.has_value()) {
        selected = *ninety_percent_index;
      } else if (const auto eighty_percent_index =
                     first_above(0.8 * smooth_max);
                 eighty_percent_index.has_value()) {
        selected = *eighty_percent_index;
      }
    }
  }
  result.independent_filter_selected = selected;
  result.padj = adjusted_by_theta[selected];
  return result;
}

std::vector<double> independent_filtering(
    const std::vector<double>& base_mean, const std::vector<double>& pvalues,
    double alpha, CompatMode compat_mode) {
  return independent_filtering_summary(base_mean, pvalues, alpha, compat_mode)
      .padj;
}

std::vector<double> cooks_filtering(const std::vector<double>& pvalues,
                                    const ByteMask& cooks_outlier) {
  if (pvalues.size() != cooks_outlier.size()) {
    throw Error(ExitCode::input_error,
                "cooks_filtering: p-values and Cook outlier mask differ in size.");
  }
  std::vector<double> filtered = pvalues;
  for (std::size_t i = 0; i < filtered.size(); ++i) {
    if (cooks_outlier[i]) {
      filtered[i] = std::numeric_limits<double>::quiet_NaN();
    }
  }
  return filtered;
}

LfcShrinkResult lfc_shrink(
    const CountMatrix& counts, const DesignMatrix& design_matrix,
    const NormalizedCounts& normalized, const LFCFit& lfc,
    const WaldSummary& summary, const std::vector<double>& dispersions,
    const ByteMask& non_zero, std::size_t coeff_index, bool adapt,
    int requested_threads, bool deterministic, CompatMode compat_mode) {
  const std::size_t genes = counts.gene_count();
  const std::size_t samples = counts.sample_count();
  const std::size_t p = design_matrix.column_count();
  if (design_matrix.sample_count() != samples ||
      normalized.sample_wise_size_factors().size() != samples ||
      normalized.base_means.size() != genes ||
      lfc.lfc_row_major.size() != genes * p ||
      summary.log2_fold_change.size() != genes ||
      summary.lfc_se.size() != genes || dispersions.size() != genes ||
      non_zero.size() != genes || coeff_index >= p) {
    throw Error(ExitCode::input_error,
                "LFC shrinkage inputs have inconsistent dimensions.");
  }

  LfcShrinkResult result;
  result.log2_fold_change = summary.log2_fold_change;
  result.lfc_se = summary.lfc_se;
  result.converged.assign(genes, 0);
  result.coeff_index = coeff_index;
  result.adapt = adapt;

  constexpr double kPriorNoShrinkScale = 15.0;
  double prior_scale = 1.0;
  if (adapt) {
    const double prior_var = fit_prior_variance(lfc, summary, coeff_index);
    prior_scale = std::min(std::sqrt(prior_var), 1.0);
  }
  result.prior_scale = prior_scale;

  std::vector<double> offset(samples, 0.0);
  const std::vector<double>& size_factors =
      normalized.sample_wise_size_factors();
  for (std::size_t sample = 0; sample < samples; ++sample) {
    offset[sample] = std::log(size_factors[sample]);
  }

  const GeneBlockExecutor executor(requested_threads, deterministic);
  executor.run(genes, [&](GeneBlock block) {
    for (std::size_t gene = block.begin; gene < block.end; ++gene) {
      if (!non_zero[gene]) {
        continue;
      }
      const double dispersion = dispersions[gene];
      if (!(dispersion > 0.0) || !std::isfinite(dispersion)) {
        continue;
      }
      const std::span<const double> counts_gene(counts.gene_data(gene),
                                                samples);
      const double size = 1.0 / dispersion;
      const pydeseq2::utils::NbinomGlmResult fitted =
          pydeseq2::utils::nbinomGLM(
              design_matrix, counts_gene, size, offset,
              kPriorNoShrinkScale, prior_scale, coeff_index, compat_mode);
      if (fitted.beta.size() != p ||
          fitted.inv_hessian_row_major.size() != p * p) {
        continue;
      }
      const double beta = fitted.beta[coeff_index];
      const double var =
          fitted.inv_hessian_row_major[coeff_index * p + coeff_index];
      const double se = std::sqrt(std::abs(var));
      if (!std::isfinite(beta) || !std::isfinite(se)) {
        continue;
      }
      result.log2_fold_change[gene] = beta / kLog2;
      result.lfc_se[gene] = se / kLog2;
      result.converged[gene] = static_cast<std::uint8_t>(fitted.converged);
    }
  });

  return result;
}

}  // namespace ccdeseq2::pydeseq2::ds
