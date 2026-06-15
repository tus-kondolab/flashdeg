#pragma once

#include <optional>
#include <vector>

#include "ccdeseq2/constants.hpp"
#include "ccdeseq2/design.hpp"
#include "ccdeseq2/normalization.hpp"
#include "ccdeseq2/profile.hpp"
#include "ccdeseq2/pydeseq2_dds.hpp"
#include "ccdeseq2/pydeseq2_ds.hpp"
#include "ccdeseq2/table.hpp"

namespace ccdeseq2 {

enum class StatisticalTestKind {
  wald,
  lrt,
};

struct DeseqPipelineOptions {
  CompatMode compat_mode = CompatMode::pydeseq2;
  SizeFactorFitType size_factor_fit_type = SizeFactorFitType::ratio;
  DispersionTrendKind trend_kind = DispersionTrendKind::parametric;
  double min_mu = 0.5;
  double min_disp = 1e-8;
  double max_disp = 10.0;
  double beta_tol = 1e-8;
  int threads = 1;
  int min_replicates = 7;
  bool deterministic = true;
  bool compute_trend = true;
  bool compute_map = true;
  bool compute_lfc = true;
  bool compute_cooks = true;
  bool compute_replacement = true;
  bool refit_cooks = true;
  bool compute_wald = true;
  bool cooks_filter = true;
  bool compute_vst = false;
  bool vst_use_design = false;
  DispersionTrendKind vst_kind = DispersionTrendKind::parametric;
  bool compute_lfc_shrink = false;
  std::size_t lfc_shrink_coeff_index = 0;
  bool lfc_shrink_adapt = true;
  // Statistical test for the result table. wald (default) keeps the existing
  // path unchanged. lrt requires run_deseq_pipeline's reduced_design_matrix and
  // lrt_degrees_of_freedom (= rank(full) - rank(reduced) from
  // validate_nested_designs).
  StatisticalTestKind test_kind = StatisticalTestKind::wald;
  std::size_t lrt_degrees_of_freedom = 0;
  WaldTestOptions wald_options;
};

struct DeseqPipelineResult {
  NormalizedCounts normalized;
  GeneWiseDispersions genewise;
  std::optional<DispersionTrendFit> trend;
  std::optional<DispersionPriorFit> prior;
  std::optional<MAPDispersions> map;
  std::optional<LFCFit> lfc;
  std::optional<LFCFit> reduced_lfc;  // populated only for test_kind == lrt
  std::optional<CookOutlierResult> cooks;
  std::optional<CookReplacementResult> replacement;
  std::optional<WaldSummary> summary;
  std::optional<VstFit> vst_fit;
  std::optional<CountMatrix> vst_counts;
  std::optional<LfcShrinkResult> lfc_shrink;
  double effective_max_disp = 0.0;
};

// reduced_design_matrix is required when options.test_kind == lrt and must be
// nested within design_matrix (validate it with validate_nested_designs first and
// pass the rank difference via options.lrt_degrees_of_freedom). It is ignored for
// the Wald path, so existing call sites are unaffected.
[[nodiscard]] DeseqPipelineResult run_deseq_pipeline(
    const CountMatrix& counts, const DesignMatrix& design_matrix,
    const std::vector<double>& contrast, const DeseqPipelineOptions& options,
    ProfileReport* profile = nullptr,
    const DesignMatrix* reduced_design_matrix = nullptr);

}  // namespace ccdeseq2
