#pragma once

#include <limits>
#include <optional>
#include <vector>

#include "ccdeseq2/constants.hpp"
#include "ccdeseq2/core_types.hpp"
#include "ccdeseq2/design.hpp"
#include "ccdeseq2/normalization.hpp"
#include "ccdeseq2/pydeseq2_dds.hpp"

namespace ccdeseq2 {

enum class AlternativeHypothesis {
  two_sided,
  greater_abs,
  less_abs,
  greater,
  less,
};

struct WaldTestOptions {
  double alpha = 0.05;
  double lfc_null_log2 = 0.0;
  double ridge_factor = kDefaultRidgeFactor;
  double min_mu = 0.0;
  AlternativeHypothesis alternative = AlternativeHypothesis::two_sided;
  const ByteMask* cooks_outlier = nullptr;
  const ByteMask* new_all_zeroes = nullptr;
  bool independent_filter = true;
  int requested_threads = 1;
  bool deterministic = true;
  CompatMode compat_mode = CompatMode::pydeseq2;
};

// Options for the likelihood-ratio test summary. Carries the WaldTestOptions
// fields needed to (a) compute the reporting Wald SE for the display contrast
// and (b) post-process p-values (Cook filtering, new-all-zero override, BH /
// independent filtering) exactly like the Wald path.
struct LrtTestOptions {
  double alpha = 0.05;
  std::size_t degrees_of_freedom = 1;  // rank(full) - rank(reduced)
  double ridge_factor = kDefaultRidgeFactor;
  double min_mu = 0.0;
  const ByteMask* cooks_outlier = nullptr;
  const ByteMask* new_all_zeroes = nullptr;
  bool independent_filter = true;
  int requested_threads = 1;
  bool deterministic = true;
  CompatMode compat_mode = CompatMode::pydeseq2;
};

struct IndependentFilteringResult {
  std::vector<double> padj;
  std::vector<double> independent_filter_theta;
  std::vector<double> independent_filter_cutoff;
  std::vector<double> independent_filter_num_rej;
  std::vector<double> independent_filter_lo_fit;
  std::size_t independent_filter_selected = 0;
  double independent_filter_alpha = 0.05;
  double independent_filter_threshold = std::numeric_limits<double>::quiet_NaN();
  double independent_filter_max_fit = std::numeric_limits<double>::quiet_NaN();
  double independent_filter_rmse = std::numeric_limits<double>::quiet_NaN();
};

struct WaldSummary {
  std::vector<double> base_mean;
  std::vector<double> log2_fold_change;
  std::vector<double> lfc_se;
  std::vector<double> statistic;
  std::vector<double> pvalue;
  std::vector<double> padj;
  std::optional<IndependentFilteringResult> independent_filtering;
};

struct LfcShrinkResult {
  std::vector<double> log2_fold_change;
  std::vector<double> lfc_se;
  ByteMask converged;
  std::size_t coeff_index = 0;
  double prior_scale = 1.0;
  bool adapt = true;
};

}  // namespace ccdeseq2

namespace ccdeseq2::pydeseq2::ds {

[[nodiscard]] WaldSummary run_wald_test(
    const DesignMatrix& design_matrix, const NormalizedCounts& normalized,
    const LFCFit& lfc, const std::vector<double>& dispersions,
    const ByteMask& non_zero, const std::vector<double>& contrast,
    const WaldTestOptions& options);

// PyDESeq2's DeseqStats.summary() builds the final results table after Wald,
// Cook filtering, independent filtering, and p-value adjustment. The C++
// wald_summary path already performs those steps according to WaldTestOptions,
// so this facade is an alias with PyDESeq2 naming.
[[nodiscard]] WaldSummary summary(
    const DesignMatrix& design_matrix, const NormalizedCounts& normalized,
    const LFCFit& lfc, const std::vector<double>& dispersions,
    const ByteMask& non_zero, const std::vector<double>& contrast,
    const WaldTestOptions& options);

// Likelihood-ratio test result table, reusing the WaldSummary shape. baseMean /
// log2_fold_change / lfc_se come from the full-model Wald reporting contrast,
// while statistic / pvalue / padj are the LRT (full vs reduced). The LRT
// statistic is 2*(NLL_reduced - NLL_full) at the shared full-model dispersions,
// and the p-value is its chi-square survival with options.degrees_of_freedom.
// counts_for_test supplies the per-gene response used in the NB log-likelihoods
// (the same counts the full model was fit on, including Cook replacement on
// refit genes). Non-converged / non-computable genes are left as NaN (NA) so
// they leave the BH / independent-filtering pool, matching DESeq2.
[[nodiscard]] WaldSummary summary_lrt(
    const CountMatrix& counts_for_test, const DesignMatrix& full_design,
    const NormalizedCounts& normalized, const LFCFit& full_lfc,
    const LFCFit& reduced_lfc, const std::vector<double>& dispersions,
    const ByteMask& non_zero, const std::vector<double>& report_contrast,
    const LrtTestOptions& options);

[[nodiscard]] std::vector<double> contrast_log2_fold_change(
    const LFCFit& fit, const std::vector<double>& contrast);

[[nodiscard]] std::vector<double> p_value_adjustment(
    const std::vector<double>& pvalues);

[[nodiscard]] std::vector<double> independent_filtering(
    const std::vector<double>& base_mean, const std::vector<double>& pvalues,
    double alpha, CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] IndependentFilteringResult independent_filtering_summary(
    const std::vector<double>& base_mean, const std::vector<double>& pvalues,
    double alpha, CompatMode compat_mode = CompatMode::pydeseq2);

[[nodiscard]] std::vector<double> cooks_filtering(
    const std::vector<double>& pvalues, const ByteMask& cooks_outlier);

[[nodiscard]] LfcShrinkResult lfc_shrink(
    const CountMatrix& counts, const DesignMatrix& design_matrix,
    const NormalizedCounts& normalized, const LFCFit& lfc,
    const WaldSummary& summary, const std::vector<double>& dispersions,
    const ByteMask& non_zero, std::size_t coeff_index, bool adapt,
    int requested_threads = 1, bool deterministic = true,
    CompatMode compat_mode = CompatMode::pydeseq2);

}  // namespace ccdeseq2::pydeseq2::ds
