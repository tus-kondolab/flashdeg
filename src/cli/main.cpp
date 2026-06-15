#include <algorithm>
#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

#include "ccdeseq2/constants.hpp"
#include "ccdeseq2/csv.hpp"
#include "ccdeseq2/design.hpp"
#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/numeric_backend.hpp"
#include "ccdeseq2/normalization.hpp"
#include "ccdeseq2/pipeline.hpp"
#include "ccdeseq2/profile.hpp"
#include "ccdeseq2/pydeseq2_dds.hpp"
#include "ccdeseq2/pydeseq2_ds.hpp"
#include "ccdeseq2/version.hpp"

namespace {

using ccdeseq2::Error;
using ccdeseq2::ExitCode;

#ifndef FLASHDEG_DEFAULT_COMPAT_MODE
#define FLASHDEG_DEFAULT_COMPAT_MODE "deseq2-r"
#endif

#ifndef FLASHDEG_GIT_REV
#define FLASHDEG_GIT_REV "unknown"
#endif

#ifndef FLASHDEG_BUILD_DATE
#define FLASHDEG_BUILD_DATE "unknown"
#endif

#ifndef FLASHDEG_BUILD_USE_EIGEN
#define FLASHDEG_BUILD_USE_EIGEN "unknown"
#endif

#ifndef FLASHDEG_BUILD_USE_BOOST_MATH
#define FLASHDEG_BUILD_USE_BOOST_MATH "unknown"
#endif

#ifndef FLASHDEG_BUILD_USE_BLAS
#define FLASHDEG_BUILD_USE_BLAS "unknown"
#endif

#ifndef FLASHDEG_BUILD_USE_SCIPY_LBFGSB
#define FLASHDEG_BUILD_USE_SCIPY_LBFGSB "unknown"
#endif

#ifndef FLASHDEG_ENABLE_DEV_OPTIONS
#define FLASHDEG_ENABLE_DEV_OPTIONS 0
#endif

#ifndef FLASHDEG_ALLOW_NO_DEPS_BUILD
#define FLASHDEG_ALLOW_NO_DEPS_BUILD 0
#endif

#ifndef FLASHDEG_BUILD_ENABLE_DEV_OPTIONS
#define FLASHDEG_BUILD_ENABLE_DEV_OPTIONS "unknown"
#endif

#ifndef FLASHDEG_BUILD_ALLOW_NO_DEPS
#define FLASHDEG_BUILD_ALLOW_NO_DEPS "unknown"
#endif

[[nodiscard]] std::string metadata_value(
    const std::map<std::string, std::string>& metadata,
    const std::string& key) {
  const auto it = metadata.find(key);
  if (it == metadata.end()) {
    return "unknown";
  }
  return it->second;
}

void write_version(std::ostream& out) {
  out << ccdeseq2::kProjectName << " " << ccdeseq2::kVersion << '\n';
}

void write_build_info(std::ostream& out) {
  const auto metadata = ccdeseq2::numeric_backend_metadata();
  const std::string lbfgsb_backend =
      std::string(FLASHDEG_BUILD_USE_SCIPY_LBFGSB) == "ON"
          ? "bundled Fortran L-BFGS-B from SciPy"
          : "local fallback L-BFGS-B";
  out << ccdeseq2::kProjectName << " " << ccdeseq2::kVersion << '\n';
  out << "Git revision: " << FLASHDEG_GIT_REV << '\n';
  out << "Build date: " << FLASHDEG_BUILD_DATE << '\n';
  out << "Numerical backends:\n";
  out << "  Linear algebra: "
      << metadata_value(metadata, "linear_algebra_backend") << '\n';
  out << "  Special functions: "
      << metadata_value(metadata, "special_function_backend") << '\n';
  out << "  L-BFGS-B: " << lbfgsb_backend << '\n';
  out << "  BLAS/LAPACK: "
      << metadata_value(metadata, "blas_lapack_backend") << '\n';
  out << "  Internal fallbacks: grid search, golden-section search, "
         "Newton refinement\n";
  out << "Build flags:\n";
  out << "  FLASHDEG_USE_EIGEN=" << FLASHDEG_BUILD_USE_EIGEN << '\n';
  out << "  FLASHDEG_USE_BOOST_MATH=" << FLASHDEG_BUILD_USE_BOOST_MATH << '\n';
  out << "  FLASHDEG_USE_BLAS=" << FLASHDEG_BUILD_USE_BLAS << '\n';
  out << "  FLASHDEG_USE_SCIPY_LBFGSB=" << FLASHDEG_BUILD_USE_SCIPY_LBFGSB
      << '\n';
  out << "Developer options: " << FLASHDEG_BUILD_ENABLE_DEV_OPTIONS << '\n';
  out << "No-deps fallback allowed: " << FLASHDEG_BUILD_ALLOW_NO_DEPS << '\n';
}

struct RunOptions {
  std::filesystem::path counts_path;
  std::filesystem::path metadata_path;
  std::filesystem::path design_matrix_path;
  std::filesystem::path reduced_design_matrix_path;
  std::filesystem::path out_path;
  std::filesystem::path write_design_matrix_path;
  std::filesystem::path write_size_factors_path;
  std::filesystem::path write_normalized_counts_path;
  std::filesystem::path write_base_means_path;
  std::filesystem::path write_rough_dispersions_path;
  std::filesystem::path write_moments_dispersions_path;
  std::filesystem::path write_mom_dispersions_path;
  std::filesystem::path write_genewise_dispersions_path;
  std::filesystem::path write_dispersion_iterations_path;
  std::filesystem::path write_dispersion_outliers_path;
  std::filesystem::path write_mu_hat_path;
  std::filesystem::path write_fitted_dispersions_path;
  std::filesystem::path write_map_dispersions_path;
  std::filesystem::path write_dispersions_path;
  std::filesystem::path write_lfc_path;
  std::filesystem::path write_lfc_log2_path;
  std::filesystem::path write_lfc_converged_path;
  std::filesystem::path write_lfc_iterations_path;
  std::filesystem::path write_lfc_fallback_path;
  std::filesystem::path write_mu_lfc_path;
  std::filesystem::path write_hat_diagonals_path;
  std::filesystem::path write_cooks_path;
  std::filesystem::path write_replace_cooks_path;
  std::filesystem::path write_replaced_counts_path;
  std::filesystem::path write_pvalue_cooks_outlier_path;
  std::filesystem::path write_replaced_path;
  std::filesystem::path write_refitted_path;
  std::filesystem::path write_new_all_zeroes_path;
  std::filesystem::path write_independent_filtering_path;
  std::filesystem::path write_vst_counts_path;
  std::filesystem::path write_vst_trend_coeffs_path;
  std::filesystem::path write_shrunken_lfc_path;
  std::filesystem::path profile_json_path;
  std::string design_formula = "~ condition";
  std::vector<std::string> contrast;
  std::string contrast_vector;
  std::string contrast_name;
  std::string fit_type = "parametric";
  std::string vst_fit_type = "parametric";
  std::string lfc_shrink_coef;
  std::string test_kind = "Wald";
  std::string reduced_formula;
  ccdeseq2::CompatMode compat_mode = ccdeseq2::CompatMode::pydeseq2;
  std::map<std::string, std::string> ref_levels;
  ccdeseq2::CountOrientation orientation =
      ccdeseq2::CountOrientation::features_as_rows;
  ccdeseq2::SizeFactorFitType size_factor_fit_type =
      ccdeseq2::SizeFactorFitType::ratio;
  double min_mu = 0.5;
  double min_disp = 1e-8;
  double max_disp = 10.0;
  double beta_tol = 1e-8;
  double alpha = 0.05;
  double lfc_null = 0.0;
  ccdeseq2::AlternativeHypothesis alt_hypothesis =
      ccdeseq2::AlternativeHypothesis::two_sided;
  int threads = 0;
  int min_replicates = 7;
  bool deterministic = false;
  bool profile = false;
  bool profile_cpu = false;
  bool quiet = false;
  bool dry_run = false;
  bool design_provided = false;
  bool reduced_provided = false;
  bool reduced_design_matrix_provided = false;
  bool lfc_null_provided = false;
  bool alt_hypothesis_provided = false;
  bool alpha_provided = false;
  bool independent_filter = true;
  bool cooks_filter = true;
  bool refit_cooks = true;
  bool compute_vst = false;
  bool vst_blind_requested = false;
  bool vst_use_design_requested = false;
  bool compute_lfc_shrink = false;
  bool lfc_shrink_adapt = true;
  bool tximport_round = false;
};

[[nodiscard]] std::string usage() {
  return R"(FlashDEG 1.1.0

Usage:
  flashdeg <command> [options]
  flashdeg --help
  flashdeg --version
  flashdeg --build-info

Commands:
  run                 Run differential expression analysis

Global options:
  -h, --help          Show this help
  --version           Show the FlashDEG version
  --build-info        Show build information

Example:
  flashdeg run --counts counts.csv --metadata metadata.csv --design "~ condition" --ref-level "condition=control" --contrast "condition" "treated" "control" --out results.csv

Use "flashdeg run --help" for analysis options.
)";
}

[[nodiscard]] std::string run_usage() {
  std::string text = R"(Usage:
  flashdeg run [options]

Required:
  --counts <path>                            CSV by default; .tsv / .tab extensions read as tab-delimited.
  --metadata <path>                          Same delimiter rules as --counts.
  --design <formula>                         e.g. "~ condition" or "~ A + B + A:B"
  --contrast <metadata-column> <test-group> <reference-group>
                                             e.g. "condition" "treated" "control"
    or
  --contrast-name <design-column>            Test one model column by exact name
                                             e.g. "genotype[T.KO]:treatment[T.drug]"
    or
  --design-matrix <path>                     Advanced: precomputed design matrix CSV
  --contrast-vector <comma-separated doubles>
                                             Advanced: numeric contrast for design matrix

Likelihood-ratio test (optional):
  --test Wald|LRT                            Statistical test (default: Wald).
  --reduced <formula>                        Nested reduced model for --test LRT,
                                             e.g. "~ 1" or "~ batch".
  --reduced-design-matrix <path>             Reduced precomputed matrix (use with
                                             --design-matrix).
                                             For LRT, --contrast/--contrast-name/
                                             --contrast-vector selects the displayed
                                             log2FoldChange only; pvalue/stat test
                                             the full design against the reduced one.

Key options:
  --features-as-cols        Counts are samples x genes (default: genes x samples)
  --tximport-round          Allow non-integer estimated counts and round them
                            with R-compatible half-to-even rounding before
                            analysis; does not read tximport length offsets
  --ref-level <metadata-column=reference-group>
  --size-factors ratio|poscounts
  --write-size-factors <path>
  --write-normalized-counts <path>
  --write-base-means <path>
  --write-fitted-dispersions <path>
  --write-dispersions <path>
  --write-replaced-counts <path>
  --write-pvalue-cooks-outlier <path>
  --write-replaced <path>   1.0 for genes with replaced Cook outliers
  --write-refitted <path>   1.0 for replaced genes that were refitted
  --write-new-all-zeroes <path>
  --write-independent-filtering <path>
                            Candidate independent-filter cutoffs and rejection counts
  --vst                    Compute PyDESeq2-style variance-stabilized counts
  --vst-blind              Fit VST with intercept-only design (default)
  --vst-use-design         Fit VST with the analysis design
  --vst-fit-type parametric|mean
  --write-vst-counts <path>
  --lfc-shrink             Compute apeGLM shrunken LFC for one coefficient
  --lfc-shrink-coef <name> Coefficient column name to shrink
  --lfc-shrink-no-adapt    Use prior scale 1 instead of empirical adaptation
  --write-shrunken-lfc <path>
                            Write baseMean, shrunkenLog2FoldChange,
                            shrunkenLfcSE, pvalue, padj, converged
  --fit-type parametric|local|mean
                            Dispersion trend fit (default: parametric)
  --out <path>              Wald result CSV
  --refit-cooks true|false  Cook outlier replacement/refit (default: true)
  --cooks-filter true|false Cook p-value filtering (default: true)
  --independent-filter true|false
  --alpha <value>           Independent-filter target FDR
                            (default: 0.1)
  --lfc-null <value>        Null LFC on log2 scale
  --alt-hypothesis greaterAbs|lessAbs|greater|less
  --min-mu <value>
  --min-disp <value>
  --max-disp <value>
  --beta-tol <value>
  --min-replicates <n>
  --threads <n>             Worker thread count; 0=auto
  --deterministic           Force single-thread deterministic execution
  --profile
  --profile-cpu
  --profile-json <path>
  --dry-run
)";
#if FLASHDEG_ENABLE_DEV_OPTIONS
  text += R"(

Developer / Oracle Debug Outputs:
  --compat-mode pydeseq2|deseq2-r
                            Developer compatibility mode; pydeseq2 mode uses
                            alpha 0.05 when --alpha is omitted
  --write-design-matrix <path>
  --write-rough-dispersions <path>
  --write-moments-dispersions <path>
  --write-mom-dispersions <path>
  --write-genewise-dispersions <path>
  --write-dispersion-iterations <path>
  --write-dispersion-outliers <path>
  --write-mu-hat <path>     Dispersion-fit mu_hat matrix
  --write-map-dispersions <path>
  --write-lfc <path>        Natural-log beta matrix (PyDESeq2 LFC layer)
  --write-lfc-log2 <path>   Log2 beta matrix (DESeq2 coef(dds)-style dump)
  --write-lfc-converged <path>
  --write-lfc-iterations <path>
  --write-lfc-fallback <path>
  --write-mu-lfc <path>     Unthresholded LFC-fit mu matrix for Wald tests
  --write-hat-diagonals <path>
  --write-cooks <path>      Cook distance matrix
  --write-replace-cooks <path>
  --write-vst-trend-coeffs <path>
)";
#endif
  return text;
}

[[nodiscard]] bool is_option(const std::string& value) {
  return value.rfind("--", 0) == 0;
}

[[nodiscard]] bool is_developer_only_option(const std::string& value) {
  static const std::vector<std::string> developer_options = {
      "--compat-mode",
      "--write-design-matrix",
      "--write-rough-dispersions",
      "--write-moments-dispersions",
      "--write-mom-dispersions",
      "--write-genewise-dispersions",
      "--write-dispersion-iterations",
      "--write-dispersion-outliers",
      "--write-mu-hat",
      "--write-map-dispersions",
      "--write-lfc",
      "--write-lfc-log2",
      "--write-lfc-converged",
      "--write-lfc-iterations",
      "--write-lfc-fallback",
      "--write-mu-lfc",
      "--write-hat-diagonals",
      "--write-cooks",
      "--write-replace-cooks",
      "--write-vst-trend-coeffs",
  };
  return std::find(developer_options.begin(), developer_options.end(), value) !=
         developer_options.end();
}

void reject_developer_only_option(const std::string& option) {
  throw Error(ExitCode::unsupported,
              option +
                  " is a developer-only option and is not "
                  "available in this release build. Rebuild with a developer "
                  "preset such as dev-vcpkg-ninja to use it.");
}

[[nodiscard]] ccdeseq2::CompatMode parse_compat_mode(
    const std::string& value) {
  if (value == "pydeseq2") {
    return ccdeseq2::CompatMode::pydeseq2;
  }
  if (value == "deseq2-r") {
    return ccdeseq2::CompatMode::deseq2_r;
  }
  throw Error(ExitCode::input_error,
              "--compat-mode must be pydeseq2 or deseq2-r.");
}

[[nodiscard]] std::vector<double> cli_wald_summary_to_row_major(
    const ccdeseq2::WaldSummary& summary) {
  const std::size_t genes = summary.base_mean.size();
  std::vector<double> values(genes * 6,
                             std::numeric_limits<double>::quiet_NaN());
  for (std::size_t gene = 0; gene < genes; ++gene) {
    values[gene * 6 + 0] = summary.base_mean[gene];
    values[gene * 6 + 1] = summary.log2_fold_change[gene];
    values[gene * 6 + 2] = summary.lfc_se[gene];
    values[gene * 6 + 3] = summary.statistic[gene];
    values[gene * 6 + 4] = summary.pvalue[gene];
    values[gene * 6 + 5] = summary.padj[gene];
  }
  return values;
}

[[nodiscard]] std::vector<double> cli_lfc_shrink_to_row_major(
    const ccdeseq2::LfcShrinkResult& shrink,
    const ccdeseq2::WaldSummary& summary) {
  const std::size_t genes = shrink.log2_fold_change.size();
  std::vector<double> values(genes * 6,
                             std::numeric_limits<double>::quiet_NaN());
  for (std::size_t gene = 0; gene < genes; ++gene) {
    values[gene * 6 + 0] = summary.base_mean[gene];
    values[gene * 6 + 1] = shrink.log2_fold_change[gene];
    values[gene * 6 + 2] = shrink.lfc_se[gene];
    values[gene * 6 + 3] = summary.pvalue[gene];
    values[gene * 6 + 4] = summary.padj[gene];
    values[gene * 6 + 5] = shrink.converged[gene] ? 1.0 : 0.0;
  }
  return values;
}

void emit_lfc_shrink_warnings(const ccdeseq2::LfcShrinkResult& shrink,
                              const ccdeseq2::WaldSummary& summary) {
  constexpr double kSmallPriorScaleThreshold = 0.01;
  constexpr double kSignificantPadjThreshold = 0.05;
  constexpr double kNearZeroShrunkenLfcThreshold = 1e-5;
  constexpr double kNearZeroSignificantFractionThreshold = 0.5;
  constexpr std::size_t kNearZeroSignificantMinGenes = 10;

  if (std::isfinite(shrink.prior_scale) &&
      shrink.prior_scale <= kSmallPriorScaleThreshold) {
    std::cerr << "warning: apeGLM prior.scale is very small ("
              << shrink.prior_scale << " <= "
              << kSmallPriorScaleThreshold
              << "). Shrunken LFC may be over-compressed toward zero; "
                 "inspect shrunkenLog2FoldChange carefully and use the "
                 "Wald log2FoldChange for primary DEG decisions.\n";
  }

  const std::size_t genes =
      std::min(shrink.log2_fold_change.size(), summary.padj.size());
  std::size_t significant = 0;
  std::size_t near_zero = 0;
  for (std::size_t gene = 0; gene < genes; ++gene) {
    const double padj = summary.padj[gene];
    if (!std::isfinite(padj) || padj >= kSignificantPadjThreshold) {
      continue;
    }
    ++significant;
    const double shrunken_lfc = shrink.log2_fold_change[gene];
    if (std::isfinite(shrunken_lfc) &&
        std::abs(shrunken_lfc) < kNearZeroShrunkenLfcThreshold) {
      ++near_zero;
    }
  }

  if (significant >= kNearZeroSignificantMinGenes) {
    const double fraction =
        static_cast<double>(near_zero) / static_cast<double>(significant);
    if (fraction >= kNearZeroSignificantFractionThreshold) {
      std::cerr << "warning: apeGLM shrinkage compressed " << near_zero
                << "/" << significant << " genes with padj < "
                << kSignificantPadjThreshold
                << " to |shrunkenLog2FoldChange| < "
                << kNearZeroShrunkenLfcThreshold << " ("
                << (100.0 * fraction)
                << "%). Volcano/MA plots using shrunken LFC may place "
                   "significant genes near zero; inspect the Wald "
                   "log2FoldChange and apeGLM prior.scale.\n";
    }
  }
}

void write_independent_filtering_csv(
    const std::filesystem::path& path,
    const ccdeseq2::IndependentFilteringResult& filtering) {
  const std::size_t rows = filtering.independent_filter_theta.size();
  std::vector<std::string> row_names;
  row_names.reserve(rows);
  std::vector<double> values;
  constexpr std::size_t kCols = 11;
  values.reserve(rows * kCols);
  const bool selected_valid = filtering.independent_filter_selected < rows;
  const double selected_theta =
      selected_valid
          ? filtering.independent_filter_theta[filtering.independent_filter_selected]
          : std::numeric_limits<double>::quiet_NaN();
  const double selected_cutoff =
      selected_valid
          ? filtering.independent_filter_cutoff[filtering.independent_filter_selected]
          : std::numeric_limits<double>::quiet_NaN();

  for (std::size_t row = 0; row < rows; ++row) {
    row_names.push_back(std::to_string(row));
    values.push_back(filtering.independent_filter_theta[row]);
    values.push_back(filtering.independent_filter_cutoff[row]);
    values.push_back(filtering.independent_filter_num_rej[row]);
    values.push_back(filtering.independent_filter_lo_fit[row]);
    values.push_back(row == filtering.independent_filter_selected ? 1.0 : 0.0);
    values.push_back(filtering.independent_filter_alpha);
    values.push_back(filtering.independent_filter_threshold);
    values.push_back(filtering.independent_filter_max_fit);
    values.push_back(filtering.independent_filter_rmse);
    values.push_back(selected_theta);
    values.push_back(selected_cutoff);
  }

  ccdeseq2::write_matrix_csv(
      path, row_names,
      {"theta", "cutoff", "numRej", "loFit", "selected", "alpha",
       "selectionThreshold", "maxFit", "rmse", "filterTheta",
       "filterThreshold"},
      values, "index");
}

[[nodiscard]] std::size_t design_column_index(
    const ccdeseq2::DesignMatrix& design, const std::string& column_name) {
  const auto& columns = design.column_names();
  const auto it = std::find(columns.begin(), columns.end(), column_name);
  if (it == columns.end()) {
    throw Error(ExitCode::input_error,
                "--lfc-shrink-coef must match a design matrix column.");
  }
  return static_cast<std::size_t>(it - columns.begin());
}

[[nodiscard]] std::string require_value(const std::vector<std::string>& args,
                                        std::size_t& index,
                                        const std::string& option) {
  if (index + 1 >= args.size() || is_option(args[index + 1])) {
    throw Error(ExitCode::input_error, option + " requires a value.");
  }
  ++index;
  return args[index];
}

[[nodiscard]] int parse_int(const std::string& value, const std::string& option) {
  int parsed = 0;
  const auto [ptr, ec] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (ec != std::errc() || ptr != value.data() + value.size() || parsed < 0) {
    throw Error(ExitCode::input_error,
                option + " requires a non-negative integer.");
  }
  return parsed;
}

[[nodiscard]] double parse_double_option(const std::string& value,
                                         const std::string& option) {
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(value.c_str(), &end);
  if (errno == ERANGE || end != value.c_str() + value.size() || !std::isfinite(parsed)) {
    throw Error(ExitCode::input_error, option + " requires a finite number.");
  }
  return parsed;
}

[[nodiscard]] bool parse_bool_option(const std::string& value,
                                     const std::string& option) {
  if (value == "true" || value == "1") {
    return true;
  }
  if (value == "false" || value == "0") {
    return false;
  }
  throw Error(ExitCode::input_error, option + " must be true or false.");
}

[[nodiscard]] ccdeseq2::AlternativeHypothesis parse_alt_hypothesis(
    const std::string& value) {
  if (value == "greaterAbs") {
    return ccdeseq2::AlternativeHypothesis::greater_abs;
  }
  if (value == "lessAbs") {
    return ccdeseq2::AlternativeHypothesis::less_abs;
  }
  if (value == "greater") {
    return ccdeseq2::AlternativeHypothesis::greater;
  }
  if (value == "less") {
    return ccdeseq2::AlternativeHypothesis::less;
  }
  throw Error(ExitCode::input_error,
              "--alt-hypothesis must be greaterAbs, lessAbs, greater or less.");
}

[[nodiscard]] std::map<std::string, std::string> add_ref_level(
    std::map<std::string, std::string> ref_levels, const std::string& text) {
  const std::size_t eq = text.find('=');
  if (eq == std::string::npos || eq == 0 || eq + 1 == text.size()) {
    throw Error(ExitCode::input_error,
                "--ref-level must be in the form <factor>=<level>.");
  }
  ref_levels[text.substr(0, eq)] = text.substr(eq + 1);
  return ref_levels;
}

[[nodiscard]] RunOptions parse_run_options(const std::vector<std::string>& args) {
  RunOptions options;
  options.compat_mode = parse_compat_mode(FLASHDEG_DEFAULT_COMPAT_MODE);
  for (std::size_t i = 2; i < args.size(); ++i) {
    const std::string& arg = args[i];
    if (is_developer_only_option(arg)) {
#if !FLASHDEG_ENABLE_DEV_OPTIONS
      reject_developer_only_option(arg);
#endif
    }
    if (arg == "--help" || arg == "-h") {
      std::cout << run_usage();
      std::exit(static_cast<int>(ExitCode::success));
    } else if (arg == "--counts") {
      options.counts_path = require_value(args, i, arg);
    } else if (arg == "--metadata") {
      options.metadata_path = require_value(args, i, arg);
    } else if (arg == "--features-as-cols") {
      options.orientation = ccdeseq2::CountOrientation::features_as_cols;
    } else if (arg == "--tximport-round") {
      options.tximport_round = true;
    } else if (arg == "--design") {
      options.design_formula = require_value(args, i, arg);
      options.design_provided = true;
    } else if (arg == "--design-matrix") {
      options.design_matrix_path = require_value(args, i, arg);
    } else if (arg == "--test") {
      options.test_kind = require_value(args, i, arg);
    } else if (arg == "--reduced") {
      options.reduced_formula = require_value(args, i, arg);
      options.reduced_provided = true;
    } else if (arg == "--reduced-design-matrix") {
      options.reduced_design_matrix_path = require_value(args, i, arg);
      options.reduced_design_matrix_provided = true;
    } else if (arg == "--compat-mode") {
#if FLASHDEG_ENABLE_DEV_OPTIONS
      const std::string value = require_value(args, i, arg);
      options.compat_mode = parse_compat_mode(value);
#endif
      // Release builds reject this earlier via is_developer_only_option().
    } else if (arg == "--contrast") {
      if (i + 3 >= args.size()) {
        throw Error(ExitCode::input_error,
                    "--contrast requires <factor> <tested> <control>.");
      }
      options.contrast = {args[++i], args[++i], args[++i]};
    } else if (arg == "--contrast-name") {
      options.contrast_name = require_value(args, i, arg);
    } else if (arg == "--contrast-vector") {
      options.contrast_vector = require_value(args, i, arg);
    } else if (arg == "--ref-level") {
      options.ref_levels =
          add_ref_level(options.ref_levels, require_value(args, i, arg));
    } else if (arg == "--out") {
      options.out_path = require_value(args, i, arg);
    } else if (arg == "--write-design-matrix") {
      options.write_design_matrix_path = require_value(args, i, arg);
    } else if (arg == "--write-size-factors") {
      options.write_size_factors_path = require_value(args, i, arg);
    } else if (arg == "--write-normalized-counts") {
      options.write_normalized_counts_path = require_value(args, i, arg);
    } else if (arg == "--write-base-means") {
      options.write_base_means_path = require_value(args, i, arg);
    } else if (arg == "--write-rough-dispersions") {
      options.write_rough_dispersions_path = require_value(args, i, arg);
    } else if (arg == "--write-moments-dispersions") {
      options.write_moments_dispersions_path = require_value(args, i, arg);
    } else if (arg == "--write-mom-dispersions") {
      options.write_mom_dispersions_path = require_value(args, i, arg);
    } else if (arg == "--write-genewise-dispersions") {
      options.write_genewise_dispersions_path = require_value(args, i, arg);
    } else if (arg == "--write-dispersion-iterations") {
      options.write_dispersion_iterations_path = require_value(args, i, arg);
    } else if (arg == "--write-dispersion-outliers") {
      options.write_dispersion_outliers_path = require_value(args, i, arg);
    } else if (arg == "--write-mu-hat") {
      options.write_mu_hat_path = require_value(args, i, arg);
    } else if (arg == "--write-fitted-dispersions") {
      options.write_fitted_dispersions_path = require_value(args, i, arg);
    } else if (arg == "--write-map-dispersions") {
      options.write_map_dispersions_path = require_value(args, i, arg);
    } else if (arg == "--write-dispersions") {
      options.write_dispersions_path = require_value(args, i, arg);
    } else if (arg == "--write-lfc") {
      options.write_lfc_path = require_value(args, i, arg);
    } else if (arg == "--write-lfc-log2") {
      options.write_lfc_log2_path = require_value(args, i, arg);
    } else if (arg == "--write-lfc-converged") {
      options.write_lfc_converged_path = require_value(args, i, arg);
    } else if (arg == "--write-lfc-iterations") {
      options.write_lfc_iterations_path = require_value(args, i, arg);
    } else if (arg == "--write-lfc-fallback") {
      options.write_lfc_fallback_path = require_value(args, i, arg);
    } else if (arg == "--write-mu-lfc") {
      options.write_mu_lfc_path = require_value(args, i, arg);
    } else if (arg == "--write-hat-diagonals") {
      options.write_hat_diagonals_path = require_value(args, i, arg);
    } else if (arg == "--write-cooks") {
      options.write_cooks_path = require_value(args, i, arg);
    } else if (arg == "--write-replace-cooks") {
      options.write_replace_cooks_path = require_value(args, i, arg);
    } else if (arg == "--write-replaced-counts") {
      options.write_replaced_counts_path = require_value(args, i, arg);
    } else if (arg == "--write-pvalue-cooks-outlier") {
      options.write_pvalue_cooks_outlier_path = require_value(args, i, arg);
    } else if (arg == "--write-replaced") {
      options.write_replaced_path = require_value(args, i, arg);
    } else if (arg == "--write-refitted") {
      options.write_refitted_path = require_value(args, i, arg);
    } else if (arg == "--write-new-all-zeroes") {
      options.write_new_all_zeroes_path = require_value(args, i, arg);
    } else if (arg == "--write-independent-filtering") {
      options.write_independent_filtering_path = require_value(args, i, arg);
    } else if (arg == "--vst") {
      options.compute_vst = true;
    } else if (arg == "--vst-blind") {
      options.compute_vst = true;
      options.vst_blind_requested = true;
    } else if (arg == "--vst-use-design") {
      options.compute_vst = true;
      options.vst_use_design_requested = true;
    } else if (arg == "--vst-fit-type") {
      options.vst_fit_type = require_value(args, i, arg);
      if (options.vst_fit_type != "mean" &&
          options.vst_fit_type != "parametric") {
        throw Error(ExitCode::input_error,
                    "--vst-fit-type must be mean or parametric.");
      }
    } else if (arg == "--write-vst-counts") {
      options.compute_vst = true;
      options.write_vst_counts_path = require_value(args, i, arg);
    } else if (arg == "--write-vst-trend-coeffs") {
      options.compute_vst = true;
      options.write_vst_trend_coeffs_path = require_value(args, i, arg);
    } else if (arg == "--lfc-shrink") {
      options.compute_lfc_shrink = true;
    } else if (arg == "--lfc-shrink-coef") {
      options.compute_lfc_shrink = true;
      options.lfc_shrink_coef = require_value(args, i, arg);
    } else if (arg == "--lfc-shrink-no-adapt") {
      options.compute_lfc_shrink = true;
      options.lfc_shrink_adapt = false;
    } else if (arg == "--write-shrunken-lfc") {
      options.compute_lfc_shrink = true;
      options.write_shrunken_lfc_path = require_value(args, i, arg);
    } else if (arg == "--fit-type") {
      options.fit_type = require_value(args, i, arg);
      if (options.fit_type != "mean" && options.fit_type != "local" &&
          options.fit_type != "parametric") {
        throw Error(ExitCode::input_error,
                    "--fit-type must be parametric, local or mean.");
      }
    } else if (arg == "--size-factors") {
      const std::string value = require_value(args, i, arg);
      if (value == "ratio") {
        options.size_factor_fit_type = ccdeseq2::SizeFactorFitType::ratio;
      } else if (value == "poscounts") {
        options.size_factor_fit_type = ccdeseq2::SizeFactorFitType::poscounts;
      } else if (value == "iterative") {
        throw Error(ExitCode::unsupported,
                    "iterative size factors are reserved but not implemented yet.");
      } else {
        throw Error(ExitCode::input_error,
                    "--size-factors must be ratio or poscounts.");
      }
    } else if (arg == "--threads") {
      options.threads = parse_int(require_value(args, i, arg), arg);
    } else if (arg == "--min-replicates") {
      options.min_replicates = parse_int(require_value(args, i, arg), arg);
    } else if (arg == "--min-mu") {
      options.min_mu = parse_double_option(require_value(args, i, arg), arg);
    } else if (arg == "--min-disp") {
      options.min_disp = parse_double_option(require_value(args, i, arg), arg);
    } else if (arg == "--max-disp") {
      options.max_disp = parse_double_option(require_value(args, i, arg), arg);
    } else if (arg == "--beta-tol") {
      options.beta_tol = parse_double_option(require_value(args, i, arg), arg);
    } else if (arg == "--independent-filter") {
      options.independent_filter =
          parse_bool_option(require_value(args, i, arg), arg);
    } else if (arg == "--alpha") {
      options.alpha = parse_double_option(require_value(args, i, arg), arg);
      options.alpha_provided = true;
    } else if (arg == "--lfc-null") {
      options.lfc_null = parse_double_option(require_value(args, i, arg), arg);
      options.lfc_null_provided = true;
    } else if (arg == "--alt-hypothesis") {
      options.alt_hypothesis =
          parse_alt_hypothesis(require_value(args, i, arg));
      options.alt_hypothesis_provided = true;
    } else if (arg == "--refit-cooks") {
      options.refit_cooks = parse_bool_option(require_value(args, i, arg), arg);
    } else if (arg == "--cooks-filter") {
      options.cooks_filter = parse_bool_option(require_value(args, i, arg), arg);
    } else if (arg == "--deterministic") {
      options.deterministic = true;
    } else if (arg == "--profile") {
      options.profile = true;
    } else if (arg == "--profile-cpu") {
      options.profile_cpu = true;
    } else if (arg == "--profile-json") {
      options.profile_json_path = require_value(args, i, arg);
    } else if (arg == "--quiet") {
      options.quiet = true;
    } else if (arg == "--dry-run") {
      options.dry_run = true;
    } else if (arg == "--log" || arg == "--silence-warnings") {
      throw Error(ExitCode::unsupported,
                  arg + " is reserved but not implemented yet.");
    } else {
      throw Error(ExitCode::input_error, "Unknown option: " + arg);
    }
  }

  if (options.counts_path.empty()) {
    throw Error(ExitCode::input_error, "--counts is required.");
  }
  if (options.metadata_path.empty()) {
    throw Error(ExitCode::input_error, "--metadata is required.");
  }
  if (!options.design_matrix_path.empty() && options.design_provided) {
    throw Error(ExitCode::input_error,
                "--design and --design-matrix are mutually exclusive.");
  }
  const int contrast_methods =
      (options.contrast.empty() ? 0 : 1) +
      (options.contrast_vector.empty() ? 0 : 1) +
      (options.contrast_name.empty() ? 0 : 1);
  if (contrast_methods != 1) {
    throw Error(ExitCode::input_error,
                "Exactly one of --contrast, --contrast-vector, or "
                "--contrast-name is required.");
  }
  if (!options.design_matrix_path.empty() && !options.contrast.empty()) {
    throw Error(ExitCode::input_error,
                "--design-matrix requires --contrast-vector or "
                "--contrast-name, not --contrast.");
  }
  if (options.min_mu <= 0.0 || options.min_disp <= 0.0 ||
      options.max_disp <= 0.0 || options.max_disp < options.min_disp ||
      options.beta_tol <= 0.0 || options.alpha <= 0.0 || options.alpha >= 1.0) {
    throw Error(ExitCode::input_error,
                "--min-mu, --min-disp, --max-disp, --beta-tol and --alpha "
                "must be positive and ordered.");
  }
  if (options.min_replicates <= 0) {
    throw Error(ExitCode::input_error,
                "--min-replicates must be a positive integer.");
  }
  if (options.lfc_null < 0.0 &&
      (options.alt_hypothesis == ccdeseq2::AlternativeHypothesis::greater_abs ||
       options.alt_hypothesis == ccdeseq2::AlternativeHypothesis::less_abs)) {
    throw Error(ExitCode::input_error,
                "--lfc-null must be non-negative with greaterAbs or lessAbs.");
  }
  if (options.vst_blind_requested && options.vst_use_design_requested) {
    throw Error(ExitCode::input_error,
                "--vst-blind and --vst-use-design are mutually exclusive.");
  }
  if (options.compute_lfc_shrink && options.lfc_shrink_coef.empty()) {
    throw Error(ExitCode::input_error,
                "--lfc-shrink requires --lfc-shrink-coef <design-column>.");
  }
  if (!options.write_independent_filtering_path.empty() &&
      !options.independent_filter) {
    throw Error(ExitCode::input_error,
                "--write-independent-filtering requires --independent-filter true.");
  }

  // Likelihood-ratio test option validation.
  const bool is_lrt = options.test_kind == "LRT" || options.test_kind == "lrt";
  const bool is_wald = options.test_kind == "Wald" || options.test_kind == "wald";
  if (!is_lrt && !is_wald) {
    throw Error(ExitCode::input_error, "--test must be Wald or LRT.");
  }
  if (!is_lrt) {
    if (options.reduced_provided || options.reduced_design_matrix_provided) {
      throw Error(ExitCode::input_error,
                  "--reduced / --reduced-design-matrix require --test LRT.");
    }
  } else {
    if (options.reduced_provided && options.reduced_design_matrix_provided) {
      throw Error(
          ExitCode::input_error,
          "--reduced and --reduced-design-matrix are mutually exclusive.");
    }
    if (!options.reduced_provided && !options.reduced_design_matrix_provided) {
      throw Error(ExitCode::input_error,
                  "--test LRT requires --reduced <formula> or "
                  "--reduced-design-matrix <path>.");
    }
    if (!options.design_matrix_path.empty()) {
      // A precomputed full design must pair with a precomputed reduced design.
      if (!options.reduced_design_matrix_provided) {
        throw Error(ExitCode::input_error,
                    "--test LRT with --design-matrix requires "
                    "--reduced-design-matrix.");
      }
    } else {
      // A formula full design must pair with a formula reduced design.
      if (!options.reduced_provided) {
        throw Error(ExitCode::input_error,
                    "--test LRT with a formula design requires --reduced "
                    "<formula> (use --reduced-design-matrix only with "
                    "--design-matrix).");
      }
    }
    if (options.compute_lfc_shrink) {
      throw Error(ExitCode::input_error,
                  "--lfc-shrink is not supported with --test LRT.");
    }
    if (options.lfc_null_provided || options.alt_hypothesis_provided) {
      throw Error(ExitCode::input_error,
                  "--lfc-null and --alt-hypothesis are Wald-only and cannot be "
                  "combined with --test LRT.");
    }
  }
  return options;
}

[[nodiscard]] double parse_double_strict(const std::string& value,
                                         const std::filesystem::path& path) {
  const std::string trimmed = ccdeseq2::trim_copy(value);
  if (trimmed.empty()) {
    throw Error(ExitCode::input_error,
                "Empty numeric value in design matrix " + path.string() + ".");
  }
  char* end = nullptr;
  errno = 0;
  const double parsed = std::strtod(trimmed.c_str(), &end);
  if (errno == ERANGE || end != trimmed.c_str() + trimmed.size() ||
      !std::isfinite(parsed)) {
    throw Error(ExitCode::input_error,
                "Invalid numeric value in design matrix " + path.string() + ": '" +
                    trimmed + "'.");
  }
  return parsed;
}

[[nodiscard]] ccdeseq2::DesignMatrix read_design_matrix_for_samples(
    const std::filesystem::path& path,
    const std::vector<std::string>& ordered_samples) {
  const ccdeseq2::CsvTable table = ccdeseq2::read_csv_table(path);
  std::vector<std::string> column_names(table.header.begin() + 1,
                                        table.header.end());
  std::map<std::string, std::size_t> row_index;
  for (std::size_t i = 0; i < table.row_names.size(); ++i) {
    row_index.emplace(table.row_names[i], i);
  }

  std::vector<double> row_major;
  row_major.reserve(ordered_samples.size() * column_names.size());
  for (const auto& sample : ordered_samples) {
    const auto it = row_index.find(sample);
    if (it == row_index.end()) {
      throw Error(ExitCode::input_error,
                  "Design matrix is missing sample '" + sample + "'.");
    }
    for (const auto& field : table.rows[it->second]) {
      row_major.push_back(parse_double_strict(field, path));
    }
  }
  return ccdeseq2::DesignMatrix(ordered_samples, column_names, row_major, {});
}

[[nodiscard]] std::vector<double> contrast_vector_for_column(
    const ccdeseq2::DesignMatrix& design, const std::string& column_name) {
  const auto& columns = design.column_names();
  const auto it = std::find(columns.begin(), columns.end(), column_name);
  if (it == columns.end()) {
    throw Error(ExitCode::input_error,
                "Contrast column '" + column_name +
                    "' was not found in the design matrix. Use "
                    "--write-design-matrix to inspect available column names.");
  }
  std::vector<double> contrast(columns.size(), 0.0);
  contrast[static_cast<std::size_t>(std::distance(columns.begin(), it))] = 1.0;
  return contrast;
}

void write_mask_csv(const std::filesystem::path& path, const std::string& name,
                    const std::vector<std::string>& gene_names,
                    const ccdeseq2::ByteMask& mask) {
  std::vector<double> values(mask.size(), 0.0);
  for (std::size_t i = 0; i < mask.size(); ++i) {
    values[i] = mask[i] != 0 ? 1.0 : 0.0;
  }
  ccdeseq2::write_series_csv(path, name, gene_names, values, "gene_id");
}

int run_command(const std::vector<std::string>& args) {
  RunOptions options = parse_run_options(args);
  if (!options.alpha_provided &&
      options.compat_mode == ccdeseq2::CompatMode::deseq2_r) {
    options.alpha = 0.1;
  }
  ccdeseq2::ProfileReport profile(options.profile_cpu);
  profile.set_metadata(ccdeseq2::numeric_backend_metadata());
  ccdeseq2::ProfileReport* profile_ptr =
      (options.profile || options.profile_cpu || !options.profile_json_path.empty())
          ? &profile
          : nullptr;

  if (options.deterministic && !options.quiet) {
    std::cerr << "[info] --deterministic forces single-thread execution.\n";
  }

  ccdeseq2::CountMatrix counts;
  {
    ccdeseq2::ScopedProfileTimer timer(profile_ptr, "load_counts_ms");
    counts = ccdeseq2::read_count_matrix(
        options.counts_path, options.orientation,
        options.tximport_round ? ccdeseq2::CountParseMode::tximport_round
                               : ccdeseq2::CountParseMode::strict_integer);
  }

  ccdeseq2::MetadataTable metadata;
  {
    ccdeseq2::ScopedProfileTimer timer(profile_ptr, "load_metadata_ms");
    metadata = ccdeseq2::read_metadata_table(options.metadata_path);
  }

  ccdeseq2::DesignMatrix design;
  std::vector<double> contrast;
  {
    ccdeseq2::ScopedProfileTimer timer(profile_ptr, "design_matrix_ms");
    if (options.design_matrix_path.empty()) {
      design = ccdeseq2::build_design_matrix(
          metadata, counts.sample_names(), options.design_formula, options.ref_levels);
      if (!options.contrast_name.empty()) {
        contrast = contrast_vector_for_column(design, options.contrast_name);
      } else if (!options.contrast_vector.empty()) {
        contrast = ccdeseq2::parse_contrast_vector(options.contrast_vector);
      } else {
        contrast = design.contrast_vector(options.contrast[0], options.contrast[1],
                                          options.contrast[2]);
      }
    } else {
      design = read_design_matrix_for_samples(options.design_matrix_path,
                                              counts.sample_names());
      if (!options.contrast_name.empty()) {
        contrast = contrast_vector_for_column(design, options.contrast_name);
      } else {
        contrast = ccdeseq2::parse_contrast_vector(options.contrast_vector);
      }
      if (contrast.size() != design.column_count()) {
        throw Error(ExitCode::input_error,
                    "--contrast-vector length does not match design matrix columns.");
      }
    }
  }
  if (contrast.size() != design.column_count()) {
    throw Error(ExitCode::input_error,
                "Contrast vector length does not match design matrix columns.");
  }

  // Likelihood-ratio test: build the nested reduced design and validate that it
  // sits inside the full design (the rank difference becomes the LRT df). The
  // contrast above is reused purely for the displayed log2FoldChange.
  const bool is_lrt =
      options.test_kind == "LRT" || options.test_kind == "lrt";
  std::optional<ccdeseq2::DesignMatrix> reduced_design;
  std::size_t lrt_degrees_of_freedom = 0;
  if (is_lrt) {
    if (!options.reduced_design_matrix_path.empty()) {
      reduced_design = read_design_matrix_for_samples(
          options.reduced_design_matrix_path, counts.sample_names());
    } else {
      reduced_design = ccdeseq2::build_design_matrix(
          metadata, counts.sample_names(), options.reduced_formula,
          options.ref_levels);
    }
    const auto nested =
        ccdeseq2::validate_nested_designs(design, *reduced_design);
    lrt_degrees_of_freedom = nested.degrees_of_freedom;
  }

  const bool need_mom_outputs = !options.write_rough_dispersions_path.empty() ||
                                !options.write_moments_dispersions_path.empty() ||
                                !options.write_mom_dispersions_path.empty();
  const bool need_wald_outputs = !options.out_path.empty() ||
                                 !options.write_independent_filtering_path.empty();
  const bool need_lfc_shrink_outputs =
      options.compute_lfc_shrink || !options.write_shrunken_lfc_path.empty();
  const bool need_replacement_outputs =
      !options.write_replace_cooks_path.empty() ||
      !options.write_replaced_counts_path.empty() ||
      !options.write_replaced_path.empty() || !options.write_refitted_path.empty() ||
      !options.write_new_all_zeroes_path.empty();
  const bool need_cooks_outputs =
      !options.write_cooks_path.empty() ||
      !options.write_pvalue_cooks_outlier_path.empty() ||
      need_replacement_outputs;
  const bool need_cooks = need_cooks_outputs ||
                          (need_wald_outputs &&
                           (options.cooks_filter || options.refit_cooks));
  const bool need_vst_outputs =
      options.compute_vst || !options.write_vst_counts_path.empty() ||
      !options.write_vst_trend_coeffs_path.empty();
  const bool need_lfc_outputs = !options.write_lfc_path.empty() ||
                                !options.write_lfc_log2_path.empty() ||
                                !options.write_lfc_converged_path.empty() ||
                                !options.write_lfc_iterations_path.empty() ||
                                !options.write_lfc_fallback_path.empty() ||
                                !options.write_mu_lfc_path.empty() ||
                                !options.write_hat_diagonals_path.empty() ||
                                need_wald_outputs || need_cooks ||
                                need_lfc_shrink_outputs;
  const bool need_genewise_outputs =
      !options.write_genewise_dispersions_path.empty() ||
      !options.write_dispersion_iterations_path.empty() ||
      !options.write_dispersion_outliers_path.empty() ||
      !options.write_mu_hat_path.empty() ||
      !options.write_fitted_dispersions_path.empty() ||
      !options.write_map_dispersions_path.empty() ||
      !options.write_dispersions_path.empty() || need_lfc_outputs;

  ccdeseq2::DeseqPipelineResult pipeline;
  std::optional<ccdeseq2::NormalizedCounts> normalized_only;
  if (need_genewise_outputs || need_vst_outputs) {
    ccdeseq2::DeseqPipelineOptions pipeline_options;
    pipeline_options.compat_mode = options.compat_mode;
    pipeline_options.size_factor_fit_type = options.size_factor_fit_type;
    if (options.fit_type == "parametric") {
      pipeline_options.trend_kind = ccdeseq2::DispersionTrendKind::parametric;
    } else if (options.fit_type == "local") {
      pipeline_options.trend_kind = ccdeseq2::DispersionTrendKind::local;
    } else {
      pipeline_options.trend_kind = ccdeseq2::DispersionTrendKind::mean;
    }
    pipeline_options.min_mu = options.min_mu;
    pipeline_options.min_disp = options.min_disp;
    pipeline_options.max_disp = options.max_disp;
    pipeline_options.beta_tol = options.beta_tol;
    pipeline_options.threads = options.threads;
    pipeline_options.min_replicates = options.min_replicates;
    pipeline_options.deterministic = options.deterministic;
    pipeline_options.compute_trend =
        !options.write_fitted_dispersions_path.empty() ||
        !options.write_map_dispersions_path.empty() ||
        !options.write_dispersions_path.empty() || need_lfc_outputs;
    pipeline_options.compute_map =
        !options.write_map_dispersions_path.empty() ||
        !options.write_dispersions_path.empty() || need_lfc_outputs;
    pipeline_options.compute_lfc = need_lfc_outputs;
    pipeline_options.compute_cooks = need_cooks;
    pipeline_options.compute_replacement =
        (options.refit_cooks || need_replacement_outputs) && need_cooks;
    pipeline_options.compute_vst = need_vst_outputs;
    pipeline_options.vst_use_design = options.vst_use_design_requested;
    pipeline_options.vst_kind =
        options.vst_fit_type == "parametric"
            ? ccdeseq2::DispersionTrendKind::parametric
            : ccdeseq2::DispersionTrendKind::mean;
    pipeline_options.refit_cooks = options.refit_cooks;
    pipeline_options.compute_wald = need_wald_outputs;
    pipeline_options.compute_lfc_shrink = need_lfc_shrink_outputs;
    if (need_lfc_shrink_outputs) {
      pipeline_options.lfc_shrink_coeff_index =
          design_column_index(design, options.lfc_shrink_coef);
      pipeline_options.lfc_shrink_adapt = options.lfc_shrink_adapt;
    }
    pipeline_options.cooks_filter = options.cooks_filter;
    pipeline_options.wald_options.alpha = options.alpha;
    pipeline_options.wald_options.lfc_null_log2 = options.lfc_null;
    pipeline_options.wald_options.alternative = options.alt_hypothesis;
    pipeline_options.wald_options.independent_filter =
        options.independent_filter;
    if (is_lrt) {
      pipeline_options.test_kind = ccdeseq2::StatisticalTestKind::lrt;
      pipeline_options.lrt_degrees_of_freedom = lrt_degrees_of_freedom;
    }
    pipeline = ccdeseq2::run_deseq_pipeline(
        counts, design, contrast, pipeline_options, profile_ptr,
        reduced_design ? &*reduced_design : nullptr);
  } else {
    ccdeseq2::ScopedProfileTimer timer(profile_ptr, "size_factor_ms");
    normalized_only =
        ccdeseq2::pydeseq2::dds::fit_size_factors(
            counts, options.size_factor_fit_type);
  }

  const ccdeseq2::NormalizedCounts& normalized =
      (need_genewise_outputs || need_vst_outputs) ? pipeline.normalized
                                                  : *normalized_only;

  if (!options.write_design_matrix_path.empty()) {
    ccdeseq2::write_matrix_csv(options.write_design_matrix_path,
                               design.sample_names(), design.column_names(),
                               design.values_row_major(), "sample_id");
  }
  if (!options.write_size_factors_path.empty()) {
    ccdeseq2::write_series_csv(options.write_size_factors_path, "sizeFactor",
                               counts.sample_names(),
                               normalized.sample_wise_size_factors(),
                               "sample_id");
  }
  if (!options.write_normalized_counts_path.empty()) {
    ccdeseq2::write_matrix_csv(
        options.write_normalized_counts_path,
        normalized.normalized_counts.sample_names(),
        normalized.normalized_counts.gene_names(),
        ccdeseq2::count_matrix_to_row_major(normalized.normalized_counts),
        "sample_id");
  }
  if (!options.write_base_means_path.empty()) {
    ccdeseq2::write_series_csv(options.write_base_means_path, "baseMean",
                               counts.gene_names(), normalized.base_means,
                               "gene_id");
  }
  if (pipeline.vst_counts.has_value() && !options.write_vst_counts_path.empty()) {
    ccdeseq2::write_matrix_csv(
        options.write_vst_counts_path, pipeline.vst_counts->sample_names(),
        pipeline.vst_counts->gene_names(),
        ccdeseq2::count_matrix_to_row_major(*pipeline.vst_counts),
        "sample_id");
  }
  if (pipeline.vst_fit.has_value() &&
      !options.write_vst_trend_coeffs_path.empty()) {
    if (pipeline.vst_fit->kind == ccdeseq2::DispersionTrendKind::parametric) {
      ccdeseq2::write_series_csv(options.write_vst_trend_coeffs_path, "value",
                                 {"a0", "a1"},
                                 {pipeline.vst_fit->a0, pipeline.vst_fit->a1},
                                 "term");
    } else {
      ccdeseq2::write_series_csv(options.write_vst_trend_coeffs_path, "value",
                                 {"mean"}, {pipeline.vst_fit->mean}, "term");
    }
  }

  if (need_genewise_outputs) {
    if (!options.write_rough_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_rough_dispersions_path,
                                 "roughDispersion", counts.gene_names(),
                                 pipeline.genewise.mom.rough, "gene_id");
    }
    if (!options.write_moments_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_moments_dispersions_path,
                                 "momentsDispersion", counts.gene_names(),
                                 pipeline.genewise.mom.moments, "gene_id");
    }
    if (!options.write_mom_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_mom_dispersions_path,
                                 "MoMDispersion", counts.gene_names(),
                                 pipeline.genewise.mom.estimates, "gene_id");
    }
    if (!options.write_genewise_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_genewise_dispersions_path,
                                 "genewiseDispersion", counts.gene_names(),
                                 pipeline.genewise.genewise, "gene_id");
    }
    if (!options.write_dispersion_iterations_path.empty()) {
      ccdeseq2::write_series_csv(options.write_dispersion_iterations_path,
                                 "dispIter", counts.gene_names(),
                                 pipeline.genewise.iterations, "gene_id");
    }
    if (!options.write_mu_hat_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.write_mu_hat_path, pipeline.genewise.mu_hat.sample_names(),
          pipeline.genewise.mu_hat.gene_names(),
          ccdeseq2::count_matrix_to_row_major(pipeline.genewise.mu_hat),
          "sample_id");
    }
    if (pipeline.trend.has_value() &&
        !options.write_fitted_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_fitted_dispersions_path,
                                 "fittedDispersion", counts.gene_names(),
                                 pipeline.trend->fitted, "gene_id");
    }
    if (pipeline.map.has_value() && !options.write_map_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_map_dispersions_path,
                                 "MAPDispersion", counts.gene_names(),
                                 pipeline.map->map, "gene_id");
    }
    if (pipeline.map.has_value() &&
        !options.write_dispersion_outliers_path.empty()) {
      write_mask_csv(options.write_dispersion_outliers_path, "dispOutlier",
                     counts.gene_names(), pipeline.map->outlier);
    }
    if (pipeline.map.has_value() && !options.write_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_dispersions_path, "dispersion",
                                 counts.gene_names(),
                                 pipeline.map->dispersions, "gene_id");
    }
    if (pipeline.lfc.has_value() && !options.write_lfc_path.empty()) {
      ccdeseq2::write_matrix_csv(options.write_lfc_path, counts.gene_names(),
                                 design.column_names(),
                                 pipeline.lfc->lfc_row_major, "gene_id");
    }
    if (pipeline.lfc.has_value() && !options.write_lfc_log2_path.empty()) {
      std::vector<double> log2_lfc = pipeline.lfc->lfc_row_major;
      const double inverse_log2 = 1.0 / std::log(2.0);
      for (double& value : log2_lfc) {
        if (std::isfinite(value)) {
          value *= inverse_log2;
        }
      }
      ccdeseq2::write_matrix_csv(options.write_lfc_log2_path,
                                 counts.gene_names(), design.column_names(),
                                 log2_lfc, "gene_id");
    }
    if (pipeline.lfc.has_value() &&
        !options.write_lfc_converged_path.empty()) {
      write_mask_csv(options.write_lfc_converged_path, "converged",
                     counts.gene_names(), pipeline.lfc->converged);
    }
    if (pipeline.lfc.has_value() &&
        !options.write_lfc_iterations_path.empty()) {
      ccdeseq2::write_series_csv(options.write_lfc_iterations_path,
                                 "betaIter", counts.gene_names(),
                                 pipeline.lfc->iterations, "gene_id");
    }
    if (pipeline.lfc.has_value() &&
        !options.write_lfc_fallback_path.empty()) {
      write_mask_csv(options.write_lfc_fallback_path, "fallback",
                     counts.gene_names(), pipeline.lfc->fallback);
    }
    if (pipeline.lfc.has_value() && !options.write_mu_lfc_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.write_mu_lfc_path, pipeline.lfc->mu.sample_names(),
          pipeline.lfc->mu.gene_names(),
          ccdeseq2::count_matrix_to_row_major(pipeline.lfc->mu), "sample_id");
    }
    if (pipeline.lfc.has_value() && !options.write_hat_diagonals_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.write_hat_diagonals_path,
          pipeline.lfc->hat_diagonals.sample_names(),
          pipeline.lfc->hat_diagonals.gene_names(),
          ccdeseq2::count_matrix_to_row_major(pipeline.lfc->hat_diagonals),
          "sample_id");
    }
    if (pipeline.cooks.has_value() && !options.write_cooks_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.write_cooks_path, pipeline.cooks->cooks.sample_names(),
          pipeline.cooks->cooks.gene_names(),
          ccdeseq2::count_matrix_to_row_major(pipeline.cooks->cooks),
          "sample_id");
    }
    if (pipeline.replacement.has_value() &&
        !options.write_replace_cooks_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.write_replace_cooks_path,
          pipeline.replacement->replace_cooks.sample_names(),
          pipeline.replacement->replace_cooks.gene_names(),
          ccdeseq2::count_matrix_to_row_major(
              pipeline.replacement->replace_cooks),
          "sample_id");
    }
    if (pipeline.replacement.has_value() &&
        !options.write_replaced_counts_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.write_replaced_counts_path,
          pipeline.replacement->counts.sample_names(),
          pipeline.replacement->counts.gene_names(),
          ccdeseq2::count_matrix_to_row_major(pipeline.replacement->counts),
          "sample_id");
    }
    if (pipeline.replacement.has_value() && !options.write_replaced_path.empty()) {
      write_mask_csv(options.write_replaced_path, "replaced",
                     counts.gene_names(), pipeline.replacement->replaced);
    }
    if (pipeline.replacement.has_value() && !options.write_refitted_path.empty()) {
      write_mask_csv(options.write_refitted_path, "refitted",
                     counts.gene_names(), pipeline.replacement->refitted);
    }
    if (pipeline.replacement.has_value() &&
        !options.write_new_all_zeroes_path.empty()) {
      write_mask_csv(options.write_new_all_zeroes_path, "new_all_zeroes",
                     counts.gene_names(),
                     pipeline.replacement->new_all_zeroes);
    }
    if (pipeline.cooks.has_value() &&
        !options.write_pvalue_cooks_outlier_path.empty()) {
      std::vector<double> pvalue_cooks(
          pipeline.cooks->pvalue_cooks_outlier.size(), 0.0);
      for (std::size_t gene = 0; gene < pvalue_cooks.size(); ++gene) {
        pvalue_cooks[gene] =
            pipeline.cooks->pvalue_cooks_outlier[gene] ? 1.0 : 0.0;
      }
      ccdeseq2::write_series_csv(
          options.write_pvalue_cooks_outlier_path, "_pvalue_cooks_outlier",
          counts.gene_names(), pvalue_cooks, "gene_id");
    }
    if (pipeline.summary.has_value() && !options.out_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.out_path, counts.gene_names(),
          {"baseMean", "log2FoldChange", "lfcSE", "stat", "pvalue", "padj"},
          cli_wald_summary_to_row_major(*pipeline.summary), "gene_id");
    }
    if (pipeline.summary.has_value() &&
        pipeline.summary->independent_filtering.has_value() &&
        !options.write_independent_filtering_path.empty()) {
      write_independent_filtering_csv(
          options.write_independent_filtering_path,
          *pipeline.summary->independent_filtering);
    }
    if (pipeline.lfc_shrink.has_value() && pipeline.summary.has_value()) {
      emit_lfc_shrink_warnings(*pipeline.lfc_shrink, *pipeline.summary);
    }
    if (pipeline.lfc_shrink.has_value() && pipeline.summary.has_value() &&
        !options.write_shrunken_lfc_path.empty()) {
      ccdeseq2::write_matrix_csv(
          options.write_shrunken_lfc_path, counts.gene_names(),
          {"baseMean", "shrunkenLog2FoldChange", "shrunkenLfcSE", "pvalue",
           "padj", "converged"},
          cli_lfc_shrink_to_row_major(*pipeline.lfc_shrink,
                                      *pipeline.summary),
          "gene_id");
    }
  } else if (need_mom_outputs) {
    ccdeseq2::MoMDispersions dispersions;
    {
      ccdeseq2::ScopedProfileTimer timer(profile_ptr, "dispersion_gene_wise_ms");
      dispersions = ccdeseq2::pydeseq2::dds::fit_MoM_dispersions(
          counts, normalized, design, options.min_disp,
          std::max(options.max_disp,
                   static_cast<double>(counts.sample_count())));
    }
    if (!options.write_rough_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_rough_dispersions_path,
                                 "roughDispersion", counts.gene_names(),
                                 dispersions.rough, "gene_id");
    }
    if (!options.write_moments_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_moments_dispersions_path,
                                 "momentsDispersion", counts.gene_names(),
                                 dispersions.moments, "gene_id");
    }
    if (!options.write_mom_dispersions_path.empty()) {
      ccdeseq2::write_series_csv(options.write_mom_dispersions_path,
                                 "MoMDispersion", counts.gene_names(),
                                 dispersions.estimates, "gene_id");
    }
  }

  if (profile_ptr != nullptr) {
    profile.set_peak_memory_mib(ccdeseq2::peak_memory_mib());
    if ((options.profile || options.profile_cpu) && !options.quiet) {
      profile.write_text(std::cerr);
    }
    if (!options.profile_json_path.empty()) {
      profile.write_json(options.profile_json_path);
    }
  }

  if (!options.dry_run && options.out_path.empty() &&
      options.write_independent_filtering_path.empty() &&
      options.write_vst_counts_path.empty() &&
      options.write_vst_trend_coeffs_path.empty() &&
      options.write_shrunken_lfc_path.empty()) {
    throw Error(ExitCode::unsupported,
                "The full DESeq2 statistical pipeline is not implemented yet. "
                "Use --out for the current Wald summary path, --write-vst-counts "
                "for VST output, --write-shrunken-lfc for apeGLM shrinkage, "
                "or --dry-run with debug write options.");
  }

  if (!options.quiet) {
    std::cerr << "Validated " << counts.sample_count() << " samples and "
              << counts.gene_count() << " genes; design matrix has "
              << design.column_count() << " columns.\n";
  }
  return static_cast<int>(ExitCode::success);
}

int run_main(const std::vector<std::string>& args) {
  try {
    if (args.size() <= 1 || args[1] == "--help" || args[1] == "-h") {
      std::cout << usage();
      return static_cast<int>(ExitCode::success);
    }
    if (args[1] == "--version") {
      write_version(std::cout);
      return static_cast<int>(ExitCode::success);
    }
    if (args[1] == "--build-info") {
      write_build_info(std::cout);
      return static_cast<int>(ExitCode::success);
    }
    if (args[1] == "run") {
      return run_command(args);
    }
    throw Error(ExitCode::input_error, "Unknown command: " + args[1]);
  } catch (const Error& error) {
    std::cerr << "error: " << error.what() << '\n';
    return static_cast<int>(error.code());
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return static_cast<int>(ExitCode::numeric_error);
  }
}

#ifdef _WIN32
[[nodiscard]] std::string wide_to_utf8(const wchar_t* text) {
  if (text == nullptr) {
    return {};
  }
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), needed, nullptr, nullptr);
  return result;
}
#endif

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.push_back(wide_to_utf8(argv[i]));
  }
  return run_main(args);
}
#else
int main(int argc, char* argv[]) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 0; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }
  return run_main(args);
}
#endif
