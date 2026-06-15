#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "ccdeseq2/csv.hpp"
#include "ccdeseq2/design.hpp"
#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/executor.hpp"
#include "ccdeseq2/linalg.hpp"
#include "ccdeseq2/nb.hpp"
#include "ccdeseq2/numeric_backend.hpp"
#include "ccdeseq2/normalization.hpp"
#include "ccdeseq2/numpy_compat.hpp"
#include "ccdeseq2/optimize.hpp"
#include "ccdeseq2/pipeline.hpp"
#include "ccdeseq2/profile.hpp"
#include "ccdeseq2/pydeseq2_dds.hpp"
#include "ccdeseq2/pydeseq2_ds.hpp"
#include "ccdeseq2/pydeseq2_grid_search.hpp"
#include "ccdeseq2/pydeseq2_preprocessing.hpp"
#include "ccdeseq2/pydeseq2_utils.hpp"
#include "ccdeseq2/special.hpp"

namespace {

namespace dds = ccdeseq2::pydeseq2::dds;
namespace ds = ccdeseq2::pydeseq2::ds;
namespace utils = ccdeseq2::pydeseq2::utils;

std::filesystem::path source_dir() {
#ifdef FLASHDEG_SOURCE_DIR
  return std::filesystem::path(FLASHDEG_SOURCE_DIR);
#else
  return std::filesystem::current_path();
#endif
}

std::filesystem::path pyde_reference_fixture_dir() {
  return source_dir() / "tests" / "fixtures" / "pyde_reference";
}

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void require_near(double actual, double expected, double tol,
                  const std::string& message) {
  if (std::abs(actual - expected) > tol) {
    throw std::runtime_error(message + ": actual=" + std::to_string(actual) +
                             " expected=" + std::to_string(expected));
  }
}

void require_relative_near(double actual, double expected, double rel_tol,
                           const std::string& message) {
  const double scale = std::max(std::abs(expected), 1e-12);
  if (std::abs(actual - expected) / scale > rel_tol) {
    throw std::runtime_error(message + ": actual=" + std::to_string(actual) +
                             " expected=" + std::to_string(expected));
  }
}

void require_relative_or_absolute_near(double actual, double expected,
                                       double rel_tol, double abs_tol,
                                       const std::string& message) {
  if (std::isnan(expected)) {
    require(std::isnan(actual), message + ": expected NaN");
    return;
  }
  const double abs_diff = std::abs(actual - expected);
  const double scale = std::max(std::abs(expected), 1e-12);
  if (abs_diff > abs_tol && abs_diff / scale > rel_tol) {
    throw std::runtime_error(message + ": actual=" + std::to_string(actual) +
                             " expected=" + std::to_string(expected));
  }
}

void test_numpy_compat_primitives() {
  require_near(ccdeseq2::median({3.0, 1.0, 2.0}), 2.0, 0.0,
               "numpy_compat median odd");
  require_near(ccdeseq2::median({4.0, 1.0, 2.0, 3.0}), 2.5, 0.0,
               "numpy_compat median even");
  require_near(ccdeseq2::trim_mean({1.0, 2.0, 100.0, 3.0}, 0.25), 2.5, 0.0,
               "numpy_compat trim_mean");
  require(std::isnan(ccdeseq2::trim_mean(
              {}, 0.25, ccdeseq2::EmptyInputPolicy::return_nan)),
          "numpy_compat trim_mean empty NaN");
  const auto xs = ccdeseq2::linspace(0.0, 1.0, 3);
  require(xs.size() == 3, "numpy_compat linspace size");
  require_near(xs[0], 0.0, 0.0, "numpy_compat linspace start");
  require_near(xs[1], 0.5, 0.0, "numpy_compat linspace mid");
  require_near(xs[2], 1.0, 0.0, "numpy_compat linspace end");
  require_near(ccdeseq2::quantile_linear({0.0, 10.0, 20.0}, 0.25), 5.0,
               0.0, "numpy_compat quantile linear");
  require_near(ccdeseq2::nan_to_num(std::numeric_limits<double>::quiet_NaN()),
               0.0, 0.0, "numpy_compat nan_to_num NaN");
  require_near(ccdeseq2::nan_to_num(std::numeric_limits<double>::infinity()),
               std::numeric_limits<double>::max(), 0.0,
               "numpy_compat nan_to_num inf");
}

void test_linalg_backend_primitives() {
  const std::vector<double> spd{4.0, 1.0, 1.0, 3.0};
  const auto chol = ccdeseq2::cholesky_decompose(spd, 2);
  require_near(chol[0], 2.0, 1e-14, "linalg cholesky diag 0");
  require_near(chol[2], 0.5, 1e-14, "linalg cholesky lower");
  require_near(chol[3], std::sqrt(2.75), 1e-14, "linalg cholesky diag 1");
  const auto solved = ccdeseq2::cholesky_solve(spd, {1.0, 2.0}, 2);
  require_near(solved[0], 1.0 / 11.0, 1e-14, "linalg cholesky solve x0");
  require_near(solved[1], 7.0 / 11.0, 1e-14, "linalg cholesky solve x1");
  require_near(ccdeseq2::positive_definite_logdet(spd, 2), std::log(11.0),
               1e-14, "linalg SPD logdet");

  const std::vector<double> x{
      1.0, 0.0,
      1.0, 1.0,
      1.0, 2.0,
  };
  const std::vector<double> y{1.0, 3.0, 5.0};
  const auto beta = ccdeseq2::least_squares(x, y, 3, 2);
  require_near(beta[0], 1.0, 1e-12, "linalg least squares intercept");
  require_near(beta[1], 2.0, 1e-12, "linalg least squares slope");
  require(ccdeseq2::matrix_rank(x, 3, 2) == 2, "linalg full rank");
  require(ccdeseq2::matrix_rank({1.0, 2.0, 2.0, 4.0}, 2, 2) == 1,
          "linalg rank deficient");
}

void test_optimizer_primitives() {
  ccdeseq2::GoldenSectionOptions options;
  options.max_iterations = 80;
  const auto result = ccdeseq2::golden_section_minimize(
      [](double x) { return (x - 2.0) * (x - 2.0) + 1.0; }, -5.0, 5.0,
      options);
  require(result.converged, "optimizer golden-section converged");
  require_near(result.argmin, 2.0, 1e-6, "optimizer golden-section argmin");
  require_near(result.minimum, 1.0, 1e-10,
               "optimizer golden-section minimum");
  require(std::string(ccdeseq2::optimizer_backend_name()).find("golden") !=
              std::string::npos,
          "optimizer backend name");

  const auto lbfgsb = ccdeseq2::minimize_l_bfgs_b(
      [](std::span<const double> x) {
        return (x[0] - 1.0) * (x[0] - 1.0) +
               2.0 * (x[1] + 2.0) * (x[1] + 2.0);
      },
      {4.0, 4.0}, {{-5.0, 5.0}, {-5.0, 5.0}});
  require(lbfgsb.converged, "optimizer L-BFGS-B converged");
  require_near(lbfgsb.x[0], 1.0, 1e-4, "optimizer L-BFGS-B x0");
  require_near(lbfgsb.x[1], -2.0, 1e-4, "optimizer L-BFGS-B x1");

  const auto scipy_lbfgsb = ccdeseq2::minimize_l_bfgs_b_scipy(
      [](std::span<const double> x) {
        return (x[0] - 1.0) * (x[0] - 1.0) +
               2.0 * (x[1] + 2.0) * (x[1] + 2.0);
      },
      {4.0, 4.0}, {{-5.0, 5.0}, {-5.0, 5.0}});
  require(scipy_lbfgsb.converged,
          "optimizer SciPy L-BFGS-B wrapper converged");
  require_near(scipy_lbfgsb.x[0], 1.0, 1e-4,
               "optimizer SciPy L-BFGS-B wrapper x0");
  require_near(scipy_lbfgsb.x[1], -2.0, 1e-4,
               "optimizer SciPy L-BFGS-B wrapper x1");

  const auto bounded = ccdeseq2::minimize_l_bfgs_b(
      [](std::span<const double> x) { return (x[0] - 3.0) * (x[0] - 3.0); },
      {0.0}, {{0.0, 2.0}});
  require(bounded.converged, "optimizer L-BFGS-B bound converged");
  require_near(bounded.x[0], 2.0, 1e-4, "optimizer L-BFGS-B bound active");
}

double csv_double(const ccdeseq2::CsvTable& table, std::size_t row,
                  std::size_t column) {
  if (table.rows[row][column].empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return std::stod(table.rows[row][column]);
}

ccdeseq2::CountMatrix repeat_genes(const ccdeseq2::CountMatrix& counts,
                                   std::size_t repeats) {
  std::vector<std::string> gene_names;
  gene_names.reserve(counts.gene_count() * repeats);
  for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
    for (const auto& gene : counts.gene_names()) {
      gene_names.push_back(gene + "_r" + std::to_string(repeat));
    }
  }

  ccdeseq2::CountMatrix repeated(counts.sample_names(), gene_names);
  for (std::size_t repeat = 0; repeat < repeats; ++repeat) {
    for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
      const std::size_t out_gene = repeat * counts.gene_count() + gene;
      for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
        repeated(sample, out_gene) = counts(sample, gene);
      }
    }
  }
  return repeated;
}

struct SingleFactorInputs {
  ccdeseq2::CountMatrix counts;
  ccdeseq2::DesignMatrix design;
};

struct PipelineRun {
  ccdeseq2::WaldSummary summary;
  std::optional<ccdeseq2::CookOutlierResult> cooks;
  std::optional<ccdeseq2::CookReplacementResult> replacement;
};

void compare_lfc_shrink_fixture(const ccdeseq2::LfcShrinkResult& actual,
                                const std::vector<std::string>& gene_names,
                                const std::filesystem::path& fixture_path,
                                double rel_tol, const std::string& label);

SingleFactorInputs load_single_factor_inputs() {
  const auto root = pyde_reference_fixture_dir();
  auto counts = ccdeseq2::read_count_matrix(
      root / "datasets" / "synthetic" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  const auto metadata = ccdeseq2::read_metadata_table(
      root / "datasets" / "synthetic" / "test_metadata.csv");
  auto design =
      ccdeseq2::build_design_matrix(metadata, counts.sample_names(), "~ condition",
                                    {});
  return {std::move(counts), std::move(design)};
}

void test_pydeseq2_facade_api() {
  const auto input = load_single_factor_inputs();
  const auto normalized = ccdeseq2::pydeseq2::preprocessing::deseq2_norm(
      input.counts, ccdeseq2::SizeFactorFitType::ratio);
  const auto size_factors =
      ccdeseq2::pydeseq2::preprocessing::deseq2_norm_fit(
          input.counts, ccdeseq2::SizeFactorFitType::ratio);
  require(size_factors.size() == input.counts.sample_count(),
          "pydeseq2 preprocessing size factor count");
  const auto transformed =
      ccdeseq2::pydeseq2::preprocessing::deseq2_norm_transform(
          input.counts, size_factors);
  require_near(transformed.normalized_counts(0, 0),
               normalized.normalized_counts(0, 0), 0.0,
               "pydeseq2 preprocessing transform");

  const double max_disp =
      std::max(10.0, static_cast<double>(input.counts.sample_count()));
  const auto mom = ccdeseq2::pydeseq2::dds::fit_MoM_dispersions(
      input.counts, normalized, input.design, 1e-8, max_disp);
  const auto genewise = ccdeseq2::pydeseq2::dds::fit_genewise_dispersions(
      input.counts, normalized, input.design, 0.5, 1e-8, max_disp);
  require(genewise.genewise.size() == input.counts.gene_count(),
          "pydeseq2 dds genewise size");
  require(genewise.mom.estimates.size() == mom.estimates.size(),
          "pydeseq2 dds MoM passthrough");
  const auto trend = ccdeseq2::pydeseq2::dds::fit_dispersion_trend(
      genewise.genewise, genewise.non_zero, normalized.base_means, 1e-8);
  const auto prior = dds::fit_dispersion_prior(
      genewise.genewise, trend.fitted, genewise.non_zero,
      input.counts.sample_count(), input.design.column_count(), 1e-8);
  const auto map = ccdeseq2::pydeseq2::dds::fit_MAP_dispersions(
      input.counts, input.design, genewise.mu_hat, genewise.genewise,
      trend.fitted, genewise.non_zero, 1e-8, max_disp, prior.prior_disp_var,
      prior.squared_logres);
  const auto lfc = ccdeseq2::pydeseq2::dds::fit_LFC(
      input.counts, normalized, input.design, map.dispersions,
      genewise.non_zero, 0.5, 1e-8);
  const auto contrast =
      input.design.contrast_vector("condition", "B", "A");
  ccdeseq2::WaldTestOptions options;
  options.independent_filter = false;
  const auto summary = ccdeseq2::pydeseq2::ds::run_wald_test(
      input.design, normalized, lfc, map.dispersions, genewise.non_zero,
      contrast, options);
  require(summary.pvalue.size() == input.counts.gene_count(),
          "pydeseq2 ds Wald summary size");

  const auto adjusted =
      ccdeseq2::pydeseq2::ds::p_value_adjustment({0.01, 0.02, 1.0});
  require(adjusted.size() == 3, "pydeseq2 ds p-value adjustment size");
  const auto filtered =
      ccdeseq2::pydeseq2::ds::cooks_filtering({0.1, 0.2}, {1, 0});
  require(std::isnan(filtered[0]), "pydeseq2 ds cooks filtering NaN");
  require_near(filtered[1], 0.2, 0.0, "pydeseq2 ds cooks filtering keep");
  bool mismatch_threw = false;
  try {
    (void)ccdeseq2::pydeseq2::ds::cooks_filtering({0.1}, {1, 0});
  } catch (const ccdeseq2::Error&) {
    mismatch_threw = true;
  }
  require(mismatch_threw, "pydeseq2 ds cooks filtering size mismatch");

  require_near(ccdeseq2::pydeseq2::utils::dispersion_trend(2.0, 0.5, 3.0),
               2.0, 0.0, "pydeseq2 utils dispersion_trend");
  const std::span<const double> counts_gene(input.counts.gene_data(0),
                                            input.counts.sample_count());
  const std::span<const double> mu_gene(lfc.mu.gene_data(0),
                                        lfc.mu.sample_count());
  require(std::isfinite(ccdeseq2::pydeseq2::utils::nb_nll(
              counts_gene, mu_gene, map.dispersions[0])),
          "pydeseq2 utils nb_nll finite");
  const auto lowess = ccdeseq2::pydeseq2::utils::lowess(
      {1.0, 2.0, 3.0}, {1.0, 2.0, 3.0}, 2.0 / 3.0, 1);
  require(lowess.size() == 3, "pydeseq2 utils lowess size");

  ccdeseq2::ThreadWorkspace workspace;
  const auto beta = ccdeseq2::pydeseq2::grid_search::grid_fit_beta(
      counts_gene, normalized.sample_wise_size_factors(), input.design,
      map.dispersions[0], 0.5, workspace);
  require(beta.size() == input.design.column_count(),
          "pydeseq2 grid_search beta size");
}

void test_vst_transform_known_values() {
  ccdeseq2::CountMatrix counts({"s1", "s2"}, {"g1", "g2"});
  counts(0, 0) = 10.0;
  counts(1, 0) = 30.0;
  counts(0, 1) = 100.0;
  counts(1, 1) = 250.0;
  const std::vector<double> size_factors{1.0, 2.0};

  ccdeseq2::VstFit parametric;
  parametric.kind = ccdeseq2::DispersionTrendKind::parametric;
  parametric.a0 = 0.2;
  parametric.a1 = 0.5;
  const auto transformed =
      ccdeseq2::pydeseq2::dds::vst_transform(counts, size_factors, parametric);
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
      const double normed = counts(sample, gene) / size_factors[sample];
      const double expected =
          std::log((1.0 + parametric.a1 + 2.0 * parametric.a0 * normed +
                    2.0 * std::sqrt(parametric.a0 * normed *
                                    (1.0 + parametric.a1 +
                                     parametric.a0 * normed))) /
                   (4.0 * parametric.a0)) /
          std::log(2.0);
      require_near(transformed(sample, gene), expected, 1e-14,
                   "VST parametric transform");
    }
  }

  ccdeseq2::VstFit mean;
  mean.kind = ccdeseq2::DispersionTrendKind::mean;
  mean.mean = 0.25;
  const auto mean_transformed =
      ccdeseq2::pydeseq2::dds::vst_transform(counts, size_factors, mean);
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
      const double normed = counts(sample, gene) / size_factors[sample];
      const double expected =
          (2.0 * std::asinh(std::sqrt(mean.mean * normed)) -
           std::log(mean.mean) - std::log(4.0)) /
          std::log(2.0);
      require_near(mean_transformed(sample, gene), expected, 1e-14,
                   "VST mean transform");
    }
  }
}

void test_single_factor_lfc_shrink_fixture() {
  const auto input = load_single_factor_inputs();
  const auto contrast = input.design.contrast_vector("condition", "B", "A");
  ccdeseq2::DeseqPipelineOptions options;
  options.refit_cooks = true;
  options.cooks_filter = true;
  options.compute_lfc_shrink = true;
  options.lfc_shrink_coeff_index = 1;
  options.lfc_shrink_adapt = true;
  const auto result =
      ccdeseq2::run_deseq_pipeline(input.counts, input.design, contrast, options);
  require(result.lfc_shrink.has_value(), "single-factor lfc_shrink result");
  compare_lfc_shrink_fixture(
      *result.lfc_shrink, input.counts.gene_names(),
      pyde_reference_fixture_dir() / "tests" / "data" / "single_factor" /
          "r_test_lfc_shrink_res.csv",
      0.02, "single-factor lfc_shrink");
}

void test_shrinkage_loss_formula() {
  ccdeseq2::DesignMatrix design(
      {"s1", "s2", "s3"}, {"Intercept", "condition[T.B]"},
      {1.0, 0.0, 1.0, 1.0, 1.0, 1.0}, {});
  const std::vector<double> beta{2.0, -0.4};
  const std::vector<double> counts{8.0, 21.0, 15.0};
  const std::vector<double> offset{0.1, -0.2, 0.05};
  const double size = 1.7;
  const double prior_no_shrink_scale = 15.0;
  const double prior_scale = 0.9;

  auto logaddexp = [](double a, double b) {
    const double hi = std::max(a, b);
    const double lo = std::min(a, b);
    return hi + std::log1p(std::exp(lo - hi));
  };

  double expected = beta[0] * beta[0] /
                    (2.0 * prior_no_shrink_scale * prior_no_shrink_scale);
  expected += std::log1p((beta[1] / prior_scale) * (beta[1] / prior_scale));
  const double log_size = std::log(size);
  for (std::size_t sample = 0; sample < counts.size(); ++sample) {
    const double xbeta = design(sample, 0) * beta[0] +
                         design(sample, 1) * beta[1];
    expected -= counts[sample] * xbeta -
                (counts[sample] + size) *
                    logaddexp(xbeta + offset[sample], log_size);
  }

  const double actual =
      utils::nbinomFn(beta, design, counts, size, offset,
                      prior_no_shrink_scale, prior_scale, 1);
  require_near(actual, expected, 1e-13,
               "shrinkage negative-binomial posterior loss");
}

void test_nbinom_glm_compat_mode_shrinkage_semantics() {
  ccdeseq2::DesignMatrix design(
      {"s1", "s2", "s3", "s4"}, {"Intercept", "condition[T.B]"},
      {1.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0}, {});
  const std::vector<double> counts{12.0, 16.0, 64.0, 82.0};
  const std::vector<double> offset{0.0, 0.0, 0.0, 0.0};

  const auto pydeseq2 = utils::nbinomGLM(
      design, counts, 4.0, offset, 15.0, 1.0, 1,
      ccdeseq2::CompatMode::pydeseq2);
  const auto deseq2_r = utils::nbinomGLM(
      design, counts, 4.0, offset, 15.0, 1.0, 1,
      ccdeseq2::CompatMode::deseq2_r);

  require(pydeseq2.converged, "pydeseq2 shrinkage MAP converged");
  require(deseq2_r.converged, "deseq2-r shrinkage MAP converged");
  require(pydeseq2.beta.size() == deseq2_r.beta.size(),
          "shrinkage MAP beta sizes match");
  double max_beta_diff = 0.0;
  for (std::size_t i = 0; i < pydeseq2.beta.size(); ++i) {
    max_beta_diff =
        std::max(max_beta_diff, std::abs(pydeseq2.beta[i] - deseq2_r.beta[i]));
  }
  require(max_beta_diff > 1e-5,
          "deseq2-r objective scale should affect shrinkage MAP stopping point");
  require(max_beta_diff < 1e-3,
          "deseq2-r shrinkage MAP should remain close to pydeseq2 MAP");

  const double se_pydeseq2 =
      std::sqrt(std::abs(pydeseq2.inv_hessian_row_major[3]));
  const double se_deseq2_r =
      std::sqrt(std::abs(deseq2_r.inv_hessian_row_major[3]));
  require(std::abs(se_pydeseq2 - se_deseq2_r) > 1e-6,
          "compat mode must affect shrinkage Hessian-derived SE");
}

ccdeseq2::MetadataTable replace_metadata_value(
    const ccdeseq2::MetadataTable& metadata, const std::string& sample_name,
    const std::string& column_name, const std::string& value) {
  std::vector<std::vector<std::string>> rows(
      metadata.sample_count(),
      std::vector<std::string>(metadata.column_count()));
  for (std::size_t sample = 0; sample < metadata.sample_count(); ++sample) {
    for (std::size_t col = 0; col < metadata.column_count(); ++col) {
      rows[sample][col] = metadata.value(sample, col);
    }
  }
  rows[metadata.sample_index(sample_name)][metadata.column_index(column_name)] = value;
  return ccdeseq2::MetadataTable(metadata.sample_names(), metadata.column_names(),
                                 std::move(rows));
}

PipelineRun run_pipeline_for_test(ccdeseq2::CountMatrix counts,
                                  const ccdeseq2::DesignMatrix& design,
                                  const std::vector<double>& contrast,
                                  bool refit_cooks = true,
                                  bool cooks_filter = true) {
  ccdeseq2::DeseqPipelineOptions options;
  options.refit_cooks = refit_cooks;
  options.cooks_filter = cooks_filter;
  options.wald_options.independent_filter = true;
  const auto result =
      ccdeseq2::run_deseq_pipeline(counts, design, contrast, options);
  PipelineRun run;
  run.summary = *result.summary;
  run.cooks = result.cooks;
  run.replacement = result.replacement;
  return run;
}

void compare_wald_fixture(const ccdeseq2::WaldSummary& actual,
                          const std::vector<std::string>& gene_names,
                          const std::filesystem::path& fixture_path,
                          double rel_tol, const std::string& label) {
  const auto expected = ccdeseq2::read_csv_table(fixture_path);
  require(expected.row_names.size() == gene_names.size(), label + " row count");
  for (std::size_t gene = 0; gene < expected.row_names.size(); ++gene) {
    require(expected.row_names[gene] == gene_names[gene], label + " gene order");
    require_relative_or_absolute_near(
        actual.base_mean[gene], csv_double(expected, gene, 0), rel_tol, 1e-10,
        label + " baseMean");
    require_relative_or_absolute_near(
        actual.log2_fold_change[gene], csv_double(expected, gene, 1), rel_tol,
        1e-10, label + " log2FoldChange");
    require_relative_or_absolute_near(
        actual.pvalue[gene], csv_double(expected, gene, 4), rel_tol, 1e-10,
        label + " pvalue");
    require_relative_or_absolute_near(
        actual.padj[gene], csv_double(expected, gene, 5), rel_tol, 1e-10,
        label + " padj");
  }
}

void compare_lfc_shrink_fixture(const ccdeseq2::LfcShrinkResult& actual,
                                const std::vector<std::string>& gene_names,
                                const std::filesystem::path& fixture_path,
                                double rel_tol, const std::string& label) {
  const auto expected = ccdeseq2::read_csv_table(fixture_path);
  require(expected.row_names.size() == gene_names.size(), label + " row count");
  for (std::size_t gene = 0; gene < expected.row_names.size(); ++gene) {
    require(expected.row_names[gene] == gene_names[gene],
            label + " gene order");
    require_relative_or_absolute_near(
        actual.log2_fold_change[gene], csv_double(expected, gene, 1), rel_tol,
        1e-10, label + " shrunken log2FoldChange");
    require(std::isfinite(actual.lfc_se[gene]), label + " shrunken lfcSE finite");
  }
}

void test_csv_parser() {
  const auto fields = ccdeseq2::parse_csv_line("\"gene,1\", 12 ,\"a\"\"b\"\r");
  require(fields.size() == 3, "CSV parser field count");
  require(fields[0] == "gene,1", "CSV parser quoted comma");
  require(fields[1] == "12", "CSV parser trim");
  require(fields[2] == "a\"b", "CSV parser escaped quote");

  const auto tsv_fields =
      ccdeseq2::parse_csv_line("\"gene\t1\"\t12\t\"a\"\"b\"\r", '\t');
  require(tsv_fields.size() == 3, "TSV parser field count");
  require(tsv_fields[0] == "gene\t1", "TSV parser quoted tab");
  require(tsv_fields[1] == "12", "TSV parser trim");
  require(tsv_fields[2] == "a\"b", "TSV parser escaped quote");

  require(ccdeseq2::detect_delimiter_from_extension("counts.tsv") == '\t',
          "TSV extension delimiter");
  require(ccdeseq2::detect_delimiter_from_extension("counts.TAB") == '\t',
          "TAB extension delimiter is case-insensitive");
  require(ccdeseq2::detect_delimiter_from_extension("counts.csv") == ',',
          "CSV extension delimiter");

  const auto tsv_path =
      std::filesystem::temp_directory_path() / "FLASHDEG_parser_test.TSV";
  {
    std::ofstream out(tsv_path, std::ios::binary);
    out << "gene_id\ts1\ts2\n";
    out << "g1\t1\t2\n";
    out << "g2\t3\t4\n";
  }
  const auto table = ccdeseq2::read_csv_table(tsv_path);
  require(table.header.size() == 3, "TSV table header width");
  require(table.header[1] == "s1", "TSV table header delimiter");
  require(table.row_names == std::vector<std::string>({"g1", "g2"}),
          "TSV table row names");
  require(table.rows[1][1] == "4", "TSV table value");
  std::filesystem::remove(tsv_path);
}

void test_tximport_round_count_parser() {
  const auto dir = std::filesystem::temp_directory_path();
  const auto counts_path = dir / "FLASHDEG_tximport_round_counts.csv";
  {
    std::ofstream out(counts_path, std::ios::binary);
    out << "gene_id,s1,s2,s3,s4,s5,s6,s7,s8\n";
    out << "g1,0.5,1.5,2.5,3.5,4.5,4.499,4.501,1e2\n";
    out << "g2,1.2,1.8,2.0,0,12,13,14,15\n";
  }

  bool strict_threw = false;
  try {
    (void)ccdeseq2::read_count_matrix(
        counts_path, ccdeseq2::CountOrientation::features_as_rows);
  } catch (const ccdeseq2::Error&) {
    strict_threw = true;
  }
  require(strict_threw, "fractional counts rejected by default");

  const auto rounded = ccdeseq2::read_count_matrix(
      counts_path, ccdeseq2::CountOrientation::features_as_rows,
      ccdeseq2::CountParseMode::tximport_round);
  const std::vector<double> expected_g1{0.0, 2.0, 2.0, 4.0,
                                        4.0, 4.0, 5.0, 100.0};
  for (std::size_t sample = 0; sample < expected_g1.size(); ++sample) {
    require_near(rounded(sample, 0), expected_g1[sample], 0.0,
                 "tximport-round R-compatible half-to-even rounding");
  }
  require_near(rounded(0, 1), 1.0, 0.0, "tximport-round 1.2");
  require_near(rounded(1, 1), 2.0, 0.0, "tximport-round 1.8");
  require_near(rounded(2, 1), 2.0, 0.0, "tximport-round integer decimal");

  const auto cols_path = dir / "FLASHDEG_tximport_round_cols.csv";
  {
    std::ofstream out(cols_path, std::ios::binary);
    out << "sample_id,g1,g2\n";
    out << "s1,0.5,1.5\n";
    out << "s2,2.5,3.5\n";
  }
  const auto rounded_cols = ccdeseq2::read_count_matrix(
      cols_path, ccdeseq2::CountOrientation::features_as_cols,
      ccdeseq2::CountParseMode::tximport_round);
  require_near(rounded_cols(0, 0), 0.0, 0.0,
               "tximport-round features-as-cols g1 s1");
  require_near(rounded_cols(1, 0), 2.0, 0.0,
               "tximport-round features-as-cols g1 s2");
  require_near(rounded_cols(0, 1), 2.0, 0.0,
               "tximport-round features-as-cols g2 s1");
  require_near(rounded_cols(1, 1), 4.0, 0.0,
               "tximport-round features-as-cols g2 s2");

  const auto tiny_path = dir / "FLASHDEG_tximport_round_tiny.csv";
  {
    std::ofstream out(tiny_path, std::ios::binary);
    out << "gene_id,s1\n";
    out << "g1,1e-310\n";
  }
  const auto tiny = ccdeseq2::read_count_matrix(
      tiny_path, ccdeseq2::CountOrientation::features_as_rows,
      ccdeseq2::CountParseMode::tximport_round);
  require_near(tiny(0, 0), 0.0, 0.0, "tximport-round tiny count rounds to zero");

  const std::vector<std::string> invalid_values{
      "NaN", "Inf", "-1", "", "0x10", "+1.5"};
  for (std::size_t i = 0; i < invalid_values.size(); ++i) {
    const auto bad_path =
        dir / ("FLASHDEG_tximport_round_bad_" + std::to_string(i) + ".csv");
    {
      std::ofstream out(bad_path, std::ios::binary);
      out << "gene_id,s1\n";
      out << "g1," << invalid_values[i] << "\n";
    }
    bool threw = false;
    try {
      (void)ccdeseq2::read_count_matrix(
          bad_path, ccdeseq2::CountOrientation::features_as_rows,
          ccdeseq2::CountParseMode::tximport_round);
    } catch (const ccdeseq2::Error&) {
      threw = true;
    }
    require(threw, "tximport-round invalid count rejected");
    std::filesystem::remove(bad_path);
  }

  std::filesystem::remove(counts_path);
  std::filesystem::remove(cols_path);
  std::filesystem::remove(tiny_path);
}

void test_trigamma_known_values() {
  constexpr double pi = 3.14159265358979323846;
  require_near(ccdeseq2::gammaln(1.0), 0.0, 1e-15, "gammaln(1)");
  require_near(ccdeseq2::gammaln(0.5), 0.5 * std::log(pi), 1e-15,
               "gammaln(0.5)");
  constexpr double euler_gamma = 0.57721566490153286060;
  require_near(ccdeseq2::digamma(1.0), -euler_gamma, 1e-10,
               "digamma(1)");
  require_near(ccdeseq2::digamma(0.5), -euler_gamma - 2.0 * std::log(2.0),
               1e-10, "digamma(0.5)");
  require_near(ccdeseq2::trigamma(0.5), pi * pi / 2.0, 1e-10,
               "trigamma(0.5)");
  require_near(ccdeseq2::trigamma(1.0), pi * pi / 6.0, 1e-10,
               "trigamma(1)");
  require(std::isnan(ccdeseq2::trigamma(-1.0)), "trigamma negative NaN");
  require_near(ccdeseq2::normal_sf(0.0), 0.5, 1e-15, "normal sf zero");
  require_near(ccdeseq2::normal_sf(1.959963984540054), 0.025, 1e-14,
               "normal sf 1.96");
  require_near(ccdeseq2::normal_ppf_75(), 0.6744897501960817, 1e-15,
               "normal ppf 0.75");
  require_near(ccdeseq2::f_distribution_cdf(1.0, 1.0, 1.0), 0.5, 1e-14,
               "F CDF df 1,1 at 1");
  require_near(ccdeseq2::f_distribution_cdf(1.0, 2.0, 2.0), 0.5, 1e-14,
               "F CDF df 2,2 at 1");
  require_relative_near(ccdeseq2::f_distribution_quantile(0.99, 2.0, 98.0),
                        4.827296266, 5e-4,
                        "F quantile 0.99 df 2,98 vs scipy");
  require_relative_near(ccdeseq2::f_distribution_quantile(0.99, 2.0, 60.0),
                        4.977385027, 1e-4,
                        "F quantile 0.99 df 2,60 vs scipy");
  require_relative_near(ccdeseq2::f_distribution_quantile(0.99, 5.0, 50.0),
                        3.407636572, 1e-4,
                        "F quantile 0.99 df 5,50 vs scipy");
}

void test_nb_nll_derivative_alpha() {
  const std::vector<double> counts{1.0, 3.0, 7.0, 12.0};
  const std::vector<double> mu{2.25, 3.5, 6.75, 9.0};
  const double alpha = 0.31;
  const double eps = 1e-6;
  const double numerical =
      (ccdeseq2::pydeseq2::utils::nb_nll(counts, mu, alpha + eps) -
       ccdeseq2::pydeseq2::utils::nb_nll(counts, mu, alpha - eps)) /
      (2.0 * eps);
  const double analytical =
      ccdeseq2::pydeseq2::utils::dnb_nll(counts, mu, alpha);
  require_relative_near(analytical, numerical, 1e-7,
                        "PyDESeq2 dnb_nll derivative alpha");
}

void test_bh_adjustment() {
  const auto adjusted = ds::p_value_adjustment(
      {0.01, 0.01, 0.03, 0.04, std::numeric_limits<double>::quiet_NaN(), 1.0});
  require_near(adjusted[0], 0.025, 1e-15, "BH tie first");
  require_near(adjusted[1], 0.025, 1e-15, "BH tie second");
  require_near(adjusted[2], 0.05, 1e-15, "BH cummin first");
  require_near(adjusted[3], 0.05, 1e-15, "BH cummin second");
  require(std::isnan(adjusted[4]), "BH NaN preserved");
  require_near(adjusted[5], 1.0, 1e-15, "BH clipped");
}

void test_lowess_and_independent_filter() {
  const std::vector<double> features{0.0, 1.0, 2.0, 3.0, 4.0};
  const std::vector<double> targets{1.0, 3.0, 5.0, 7.0, 9.0};
  const auto smooth = utils::lowess(features, targets, 0.8, 3);
  for (std::size_t i = 0; i < targets.size(); ++i) {
    require_near(smooth[i], targets[i], 1e-12,
                 "LOWESS perfect-fit s==0 path");
  }

  std::vector<double> base_mean(30, 0.0);
  std::vector<double> pvalues(30, 0.9);
  for (std::size_t i = 0; i < base_mean.size(); ++i) {
    base_mean[i] = static_cast<double>(i + 1);
    if (i < 20) {
      pvalues[i] = 1e-6;
    }
  }
  const auto adjusted = ds::independent_filtering(base_mean, pvalues, 0.05);
  const auto rejected = std::count_if(adjusted.begin(), adjusted.end(),
                                      [](double value) {
                                        return !std::isnan(value) && value < 0.05;
                                      });
  require(rejected > 10, "independent filtering LOWESS branch exercised");
}

// Apply DESeq2 R's independent-filtering theta-selection rule to a numRej
// curve smoothed by lowess_r_compat, returning the selected index.
[[nodiscard]] std::size_t select_filter_theta(
    const std::vector<double>& theta, const std::vector<double>& num_rej) {
  const auto smooth =
      ccdeseq2::pydeseq2::utils::lowess_r_compat(theta, num_rej, 1.0 / 5.0, 3);
  double residual_sq = 0.0;
  std::size_t residual_n = 0;
  double max_fit = smooth.empty() ? 0.0 : smooth[0];
  for (std::size_t i = 0; i < num_rej.size(); ++i) {
    max_fit = std::max(max_fit, smooth[i]);
    if (num_rej[i] > 0.0) {
      const double r = num_rej[i] - smooth[i];
      residual_sq += r * r;
      ++residual_n;
    }
  }
  const double rmse = residual_n == 0 ? 0.0 : std::sqrt(residual_sq / residual_n);
  const auto first_above = [&](double value) -> std::size_t {
    for (std::size_t i = 0; i < num_rej.size(); ++i) {
      if (num_rej[i] > value) {
        return i;
      }
    }
    return num_rej.size();
  };
  std::size_t sel = first_above(max_fit - rmse);
  if (sel == num_rej.size()) sel = first_above(0.9 * max_fit);
  if (sel == num_rej.size()) sel = first_above(0.8 * max_fit);
  if (sel == num_rej.size()) sel = 0;
  return sel;
}

// Regression guard for the DESeq2-R-compatible lowess used in independent
// filtering. The two numRej curves are reconstructed from real benchmark
// results: TCGA-BRCA (n=1256) where DESeq2 R selects theta index 0, and a
// 4,500-sample GTEx-like dataset where DESeq2 R selects index 2. The earlier
// PyDESeq2-derived lowess (iter=4 + r_compat bandwidth patch) blew up to
// ~2^16 on the GTEx curve and selected index 0 instead. lowess_r_compat is a
// faithful port of R's stats::lowess and must reproduce R's selection on
// both. See tests/fixtures/lowess/PROVENANCE.md.
void test_lowess_r_compat_selection() {
  const auto root = source_dir() / "tests" / "fixtures" / "lowess";
  struct Case {
    const char* file;
    std::size_t expected;
  };
  for (const Case& c : {Case{"tcga_brca_numrej.csv", 0},
                        Case{"large4500_numrej.csv", 2}}) {
    const auto table = ccdeseq2::read_csv_table(root / c.file);
    std::vector<double> theta;
    std::vector<double> num_rej;
    theta.reserve(table.rows.size());
    num_rej.reserve(table.rows.size());
    for (std::size_t i = 0; i < table.rows.size(); ++i) {
      theta.push_back(std::stod(table.row_names[i]));
      num_rej.push_back(std::stod(table.rows[i][0]));
    }
    const std::size_t sel = select_filter_theta(theta, num_rej);
    require(sel == c.expected,
            std::string("lowess_r_compat theta selection for ") + c.file +
                " expected " + std::to_string(c.expected) + " got " +
                std::to_string(sel));
  }
}

// Regression guard for fit_gamma_glm_identity_irls step halving (R glm.fit
// validmu() + start <- (start + coefold)/2 loop). Before the step-halving
// fix, the first WLS step on this 9,931-gene active set produced
// (a0, a1) = (-0.022, 6.41); the original code bailed on next[0] <= 0 and
// fit_parametric_dispersion_trend silently fell back to local trend,
// diverging from DESeq2 R on dispersions by 10-30% per gene and flipping
// 16 borderline DEG calls on the 10,000-gene smoke synthetic. R's stats::
// glm.fit handles the same iterate by halving the step toward old coefs
// until validmu(mu) holds, converging to (a0, a1) = (0.002141, 2.854).
// See docs/parametric_trend_fit_step_halving_fix.md.
void test_parametric_trend_fit_step_halving() {
  const auto path = source_dir() / "tests" / "fixtures" / "trend_fit" /
                    "smoke_active_set.csv";
  const auto table = ccdeseq2::read_csv_table(path);
  std::vector<double> base_means;
  std::vector<double> genewise;
  base_means.reserve(table.rows.size());
  genewise.reserve(table.rows.size());
  for (const auto& row : table.rows) {
    base_means.push_back(std::stod(row[0]));
    genewise.push_back(std::stod(row[1]));
  }
  const ccdeseq2::ByteMask non_zero(base_means.size(),
                                    static_cast<std::uint8_t>(1));
  const auto fit = dds::fit_parametric_dispersion_trend(
      genewise, non_zero, base_means, 1.0e-8,
      ccdeseq2::CompatMode::deseq2_r);

  require(fit.kind == ccdeseq2::DispersionTrendKind::parametric,
          "smoke active set: parametric trend must be selected, not local "
          "(local fallback is the pre-fix bug signature)");
  require(fit.converged, "smoke active set: parametric trend must converge");
  // R native glm(Gamma(identity)) on the same active set converges to
  // (a0 = 0.002141, a1 = 2.853513). Allow ~10% slack so the test tolerates
  // tiny IRLS convergence-criterion differences across platforms / future
  // tweaks; the pre-fix code returned (a0 = 0.1, a1 = 1.0) which is more
  // than 30x off on a1.
  require(fit.a0 > 0.0 && fit.a0 < 0.01,
          "a0 (intercept) within tolerance of R's 0.002141");
  require(fit.a1 > 2.5 && fit.a1 < 3.2,
          "a1 (slope) within tolerance of R's 2.854");
}

void test_wald_nonconverged_adjustment() {
  ccdeseq2::CountMatrix mu({"s1", "s2"}, {"g1", "g2"});
  mu(0, 0) = 10.0;
  mu(1, 0) = 10.0;
  mu(0, 1) = 20.0;
  mu(1, 1) = 20.0;
  ccdeseq2::CountMatrix normed({"s1", "s2"}, {"g1", "g2"});
  ccdeseq2::NormalizedCounts normalized{
      ccdeseq2::NormalizationFactors::sample_wise({1.0, 1.0}),
      normed,
      {10.0, 20.0}};
  ccdeseq2::LFCFit lfc;
  lfc.lfc_row_major = {0.0, 0.0};
  lfc.mu = mu;
  lfc.converged = {0, 0};
  ccdeseq2::DesignMatrix design({"s1", "s2"}, {"Intercept"}, {1.0, 1.0}, {});
  ccdeseq2::WaldTestOptions options;
  options.independent_filter = false;
  const auto summary = ds::summary(
      design, normalized, lfc, {0.1, 0.1}, {1, 1}, {1.0}, options);
  require_near(summary.pvalue[0], 1.0, 0.0, "nonconverged pvalue one");
  require_near(summary.padj[0], 1.0, 0.0, "nonconverged pvalue adjusted");
  require_near(summary.padj[1], 1.0, 0.0, "nonconverged second adjusted");

  ccdeseq2::ByteMask cooks_outlier{1, 0};
  options.cooks_outlier = &cooks_outlier;
  const auto filtered = ds::summary(
      design, normalized, lfc, {0.1, 0.1}, {1, 1}, {1.0}, options);
  require(std::isnan(filtered.pvalue[0]), "Cook-filtered pvalue NaN");
  require(std::isnan(filtered.padj[0]), "Cook-filtered padj NaN");
  require_near(filtered.pvalue[1], 1.0, 0.0,
               "unfiltered nonconverged pvalue one");
  require_near(filtered.padj[1], 1.0, 0.0,
               "unfiltered nonconverged padj one");
}

void test_wald_min_mu_floor_bounds_se() {
  const std::vector<std::string> samples{"s1", "s2", "s3",
                                         "s4", "s5", "s6"};
  ccdeseq2::CountMatrix mu(samples, {"g1"});
  ccdeseq2::CountMatrix normed(samples, {"g1"});
  for (std::size_t sample = 0; sample < samples.size(); ++sample) {
    mu(sample, 0) = 1.0e-6;
    normed(sample, 0) = 1.0;
  }

  ccdeseq2::NormalizedCounts normalized{
      ccdeseq2::NormalizationFactors::sample_wise(
          std::vector<double>(samples.size(), 1.0)),
      normed,
      {1.0}};
  ccdeseq2::LFCFit lfc;
  lfc.lfc_row_major = {0.0, 0.0};
  lfc.mu = mu;
  lfc.converged = {1};
  ccdeseq2::DesignMatrix design(
      samples, {"Intercept", "condition[T.B]"},
      {1.0, 0.0,
       1.0, 0.0,
       1.0, 0.0,
       1.0, 1.0,
       1.0, 1.0,
       1.0, 1.0},
      {});

  ccdeseq2::WaldTestOptions options;
  options.independent_filter = false;
  options.min_mu = 0.0;
  const auto unfloored = ds::summary(design, normalized, lfc, {1.0}, {1},
                                     {0.0, 1.0}, options);

  options.min_mu = 0.5;
  const auto floored = ds::summary(design, normalized, lfc, {1.0}, {1},
                                   {0.0, 1.0}, options);

  require(std::isfinite(floored.lfc_se[0]),
          "Wald min_mu floor keeps lfcSE finite");
  require(floored.lfc_se[0] < 20.0,
          "Wald min_mu floor bounds lfcSE");
  require(unfloored.lfc_se[0] > floored.lfc_se[0] * 100.0,
          "Wald min_mu floor reduces degenerate lfcSE");
}

void test_cooks_replicate_mask() {
  ccdeseq2::DesignMatrix design(
      {"s1", "s2", "s3", "s4", "s5", "s6"},
      {"Intercept", "group_code"},
      {1.0, 0.0, 1.0, 0.0, 1.0, 0.0,
       1.0, 1.0, 1.0, 1.0, 1.0, 2.0},
      {});
  const auto mask = utils::n_or_more_replicates(design, 3);
  require(mask == ccdeseq2::ByteMask({1, 1, 1, 0, 0, 0}),
          "Cook replicate mask");

  ccdeseq2::CountMatrix zero_normed({"s1", "s2", "s3"}, {"g1"});
  const auto alpha = utils::robust_method_of_moments_disp(
      zero_normed,
      ccdeseq2::DesignMatrix({"s1", "s2", "s3"}, {"Intercept"},
                             {1.0, 1.0, 1.0}, {}),
      {1});
  require(std::isnan(alpha[0]), "Cook robust MoM NaN propagation");

  ccdeseq2::CountMatrix counts(
      {"s1", "s2", "s3", "s4", "s5", "s6"}, {"g1", "g2"});
  ccdeseq2::CountMatrix cooks(counts.sample_names(), counts.gene_names());
  counts(0, 0) = 10.0;
  counts(1, 0) = 12.0;
  counts(2, 0) = 11.0;
  counts(3, 0) = 13.0;
  counts(4, 0) = 9.0;
  counts(5, 0) = 8.0;
  counts(0, 1) = 10.0;
  counts(1, 1) = 20.0;
  counts(2, 1) = 30.0;
  counts(3, 1) = 40.0;
  counts(4, 1) = 5.0;
  counts(5, 1) = 6.0;
  cooks.fill(0.0);
  counts(2, 0) = 100000.0;
  cooks(2, 0) = 1e6;
  cooks(0, 1) = 1e6;
  const auto outliers = dds::cooks_outlier(counts, design, cooks);
  require(outliers == ccdeseq2::ByteMask({1, 0}),
          "Cook outlier count-rank rule");

  const auto normalized = ccdeseq2::normalize_counts_with_size_factors(
      counts, std::vector<double>(counts.sample_count(), 1.0));
  const auto replacement =
      dds::replace_outliers(counts, normalized, design, cooks, 3);
  require(replacement.replaced == ccdeseq2::ByteMask({1, 1}),
          "Cook replacement flags all cutoff genes");
  require(replacement.refitted == ccdeseq2::ByteMask({1, 1}),
          "Cook replacement refit flags");
  require_near(replacement.counts(2, 0), 11.0, 0.0,
               "Cook replacement trimmed count");
  require_near(replacement.replace_cooks(0, 0), 0.0, 0.0,
               "Cook replacement replace_cooks zeroed");
}

void test_count_orientations() {
  const auto dir = std::filesystem::temp_directory_path();
  const auto rows_path = dir / "FLASHDEG_counts_rows.csv";
  const auto cols_path = dir / "FLASHDEG_counts_cols.csv";
  {
    std::ofstream out(rows_path, std::ios::binary);
    out << "\"\",s1,s2\n";
    out << "g1,1,2\n";
    out << "g2,3,4\n";
  }
  {
    std::ofstream out(cols_path, std::ios::binary);
    out << "\"\",g1,g2\n";
    out << "s1,1,3\n";
    out << "s2,2,4\n";
  }
  const auto rows = ccdeseq2::read_count_matrix(
      rows_path, ccdeseq2::CountOrientation::features_as_rows);
  const auto cols = ccdeseq2::read_count_matrix(
      cols_path, ccdeseq2::CountOrientation::features_as_cols);
  require(rows.sample_names() == cols.sample_names(),
          "features-as-cols sample order");
  require(rows.gene_names() == cols.gene_names(), "features-as-cols gene order");
  for (std::size_t sample = 0; sample < rows.sample_count(); ++sample) {
    for (std::size_t gene = 0; gene < rows.gene_count(); ++gene) {
      require_near(rows(sample, gene), cols(sample, gene), 0.0,
                   "features-as-cols value");
    }
  }
  std::filesystem::remove(rows_path);
  std::filesystem::remove(cols_path);
}

void test_output_writers_create_parent_directories() {
  const auto root =
      std::filesystem::temp_directory_path() / "FLASHDEG_output_parent_test";
  std::filesystem::remove_all(root);

  const auto csv_path = root / "csv" / "nested" / "series.csv";
  ccdeseq2::write_series_csv(csv_path, "value", {"g1", "g2"}, {1.0, 2.0},
                             "gene_id");
  require(std::filesystem::exists(csv_path),
          "CSV writer creates parent directories");
  const auto csv = ccdeseq2::read_csv_table(csv_path);
  require(csv.header[0] == "gene_id", "CSV nested output row-name header");
  require(csv.row_names.size() == 2, "CSV nested output row count");

  const auto profile_path = root / "profile" / "nested" / "profile.json";
  ccdeseq2::ProfileReport profile(false);
  profile.add_wall_time("step_ms", 1.0, 0.0);
  profile.write_json(profile_path);
  require(std::filesystem::exists(profile_path),
          "profile writer creates parent directories");

  std::filesystem::remove_all(root);
}

void test_counts_design_and_size_factors() {
  const auto root = pyde_reference_fixture_dir();
  const auto counts_path = root / "datasets" / "synthetic" /
                           "test_counts.csv";
  const auto metadata_path = root / "datasets" / "synthetic" /
                             "test_metadata.csv";
  const auto sf_path = root / "tests" / "data" / "single_factor" /
                       "r_test_size_factors.csv";
  const auto sf_poscounts_path =
      root / "tests" / "data" / "single_factor" /
      "r_test_size_factors_poscount.csv";

  const auto counts = ccdeseq2::read_count_matrix(
      counts_path, ccdeseq2::CountOrientation::features_as_rows);
  require(counts.sample_count() == 100, "synthetic sample count");
  require(counts.gene_count() > 0, "synthetic gene count");
  require(counts.sample_names()[0] == "sample1", "first sample name");
  require(counts.gene_names()[0] == "gene1", "first gene name");
  require_near(counts(0, 0), 12.0, 0.0, "count sample1/gene1");
  require(counts.gene_data(1) == counts.gene_data(0) + counts.sample_count(),
          "gene-major contiguous layout");

  const auto metadata = ccdeseq2::read_metadata_table(metadata_path);
  const auto design =
      ccdeseq2::build_design_matrix(metadata, counts.sample_names(), "~ condition",
                                    {});
  require(design.column_count() == 2, "single-factor design column count");
  require(design.column_names()[0] == "Intercept", "intercept column");
  require(design.column_names()[1] == "condition[T.B]", "condition column");
  require_near(design(0, 0), 1.0, 0.0, "intercept value");
  require_near(design(0, 1), 0.0, 0.0, "sample1 condition value");
  const auto contrast = design.contrast_vector("condition", "B", "A");
  require(contrast.size() == 2, "contrast length");
  require_near(contrast[0], 0.0, 0.0, "contrast intercept");
  require_near(contrast[1], 1.0, 0.0, "contrast condition");

  const auto design_ref_b =
      ccdeseq2::build_design_matrix(metadata, counts.sample_names(), "~ condition",
                                    {{"condition", "B"}});
  require(design_ref_b.column_names()[1] == "condition[T.A]",
          "ref-level changed dummy column");
  const auto contrast_ref_b =
      design_ref_b.contrast_vector("condition", "B", "A");
  require_near(contrast_ref_b[1], -1.0, 0.0, "ref-level contrast sign");

  const ccdeseq2::MetadataTable nan_metadata({"s1", "s2"}, {"condition"},
                                             {{"nan"}, {"A"}});
  const auto nan_design = ccdeseq2::build_design_matrix(
      nan_metadata, std::vector<std::string>{"s1", "s2"}, "~ condition", {});
  require(nan_design.column_names()[1] == "condition[T.nan]",
          "non-finite-looking level remains categorical");

  const auto normalized =
      dds::fit_size_factors(counts, ccdeseq2::SizeFactorFitType::ratio);
  const auto expected = ccdeseq2::read_csv_table(sf_path);
  require(expected.row_names.size() ==
              normalized.sample_wise_size_factors().size(),
          "size factor expected row count");
  for (std::size_t i = 0; i < expected.row_names.size(); ++i) {
    const double expected_value = std::stod(expected.rows[i][0]);
    require_near(normalized.sample_wise_size_factors()[i], expected_value, 1e-10,
                 "size factor fixture mismatch");
  }
  require_near(normalized.normalized_counts(0, 0),
               counts(0, 0) / normalized.sample_wise_size_factors()[0], 1e-12,
               "normalized count value");
  double gene1_sum = 0.0;
  for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
    gene1_sum += counts(sample, 0) / normalized.sample_wise_size_factors()[sample];
  }
  require_near(normalized.base_means[0],
               gene1_sum / static_cast<double>(counts.sample_count()), 1e-12,
               "base mean value");

  const auto normalized_poscounts =
      dds::fit_size_factors(counts, ccdeseq2::SizeFactorFitType::poscounts);
  const auto expected_poscounts = ccdeseq2::read_csv_table(sf_poscounts_path);
  require(expected_poscounts.row_names.size() ==
              normalized_poscounts.sample_wise_size_factors().size(),
          "poscounts expected row count");
  for (std::size_t i = 0; i < expected_poscounts.row_names.size(); ++i) {
    const double expected_value = std::stod(expected_poscounts.rows[i][0]);
    require_near(normalized_poscounts.sample_wise_size_factors()[i],
                 expected_value, 1e-10, "poscounts size factor fixture mismatch");
  }

  const auto row_major =
      ccdeseq2::count_matrix_to_row_major(normalized.normalized_counts);
  require_near(row_major[0], normalized.normalized_counts(0, 0), 0.0,
               "normalized row-major conversion");
  require_near(row_major[counts.gene_count()], normalized.normalized_counts(1, 0),
               0.0, "normalized row-major second sample");
}

void test_design_interaction() {
  const ccdeseq2::MetadataTable metadata(
      {"s1", "s2", "s3", "s4"}, {"A", "B", "x", "y"},
      {{"a1", "b1", "1", "10"},
       {"a2", "b1", "2", "20"},
       {"a1", "b2", "3", "30"},
       {"a2", "b2", "4", "40"}});
  const std::vector<std::string> samples{"s1", "s2", "s3", "s4"};
  const std::map<std::string, std::string> refs{{"A", "a1"}, {"B", "b1"}};

  const auto design = ccdeseq2::build_design_matrix(
      metadata, samples, "~ A + B + A:B", refs);
  const std::vector<std::string> expected_columns{
      "Intercept", "A[T.a2]", "B[T.b2]", "A[T.a2]:B[T.b2]"};
  require(design.column_names() == expected_columns,
          "interaction design column names");
  const std::vector<double> expected_values{
      1.0, 0.0, 0.0, 0.0,
      1.0, 1.0, 0.0, 0.0,
      1.0, 0.0, 1.0, 0.0,
      1.0, 1.0, 1.0, 1.0};
  require(design.values_row_major() == expected_values,
          "interaction design values");

  const auto shorthand = ccdeseq2::build_design_matrix(
      metadata, samples, "~ A * B", refs);
  require(shorthand.column_names() == design.column_names(),
          "interaction shorthand columns");
  require(shorthand.values_row_major() == design.values_row_major(),
          "interaction shorthand values");

  const auto additive = ccdeseq2::build_design_matrix(
      metadata, samples, "~ A + B", refs);
  require(additive.column_names() ==
              std::vector<std::string>({"Intercept", "A[T.a2]", "B[T.b2]"}),
          "additive design column names");
  require(additive.values_row_major() ==
              std::vector<double>({1.0, 0.0, 0.0,
                                   1.0, 1.0, 0.0,
                                   1.0, 0.0, 1.0,
                                   1.0, 1.0, 1.0}),
          "additive design values");

  const auto numeric_categorical = ccdeseq2::build_design_matrix(
      metadata, samples, "~ x + A + x:A", refs);
  require(numeric_categorical.column_names() ==
              std::vector<std::string>(
                  {"Intercept", "x", "A[T.a2]", "x:A[T.a2]"}),
          "numeric categorical interaction columns");
  require(numeric_categorical.values_row_major() ==
              std::vector<double>({1.0, 1.0, 0.0, 0.0,
                                   1.0, 2.0, 1.0, 2.0,
                                   1.0, 3.0, 0.0, 0.0,
                                   1.0, 4.0, 1.0, 4.0}),
          "numeric categorical interaction values");

  const auto numeric_numeric = ccdeseq2::build_design_matrix(
      metadata, samples, "~ x + y + x:y", refs);
  require(numeric_numeric.column_names() ==
              std::vector<std::string>({"Intercept", "x", "y", "x:y"}),
          "numeric numeric interaction columns");
  require(numeric_numeric.values_row_major() ==
              std::vector<double>({1.0, 1.0, 10.0, 10.0,
                                   1.0, 2.0, 20.0, 40.0,
                                   1.0, 3.0, 30.0, 90.0,
                                   1.0, 4.0, 40.0, 160.0}),
          "numeric numeric interaction values");

  const auto expect_unsupported = [&](std::string_view formula,
                                      const std::string& label) {
    bool threw = false;
    try {
      (void)ccdeseq2::build_design_matrix(metadata, samples, formula, refs);
    } catch (const ccdeseq2::Error& error) {
      threw = error.code() == ccdeseq2::ExitCode::unsupported;
    }
    require(threw, label);
  };
  expect_unsupported("~ A:B:C", "three-way colon interaction rejected");
  expect_unsupported("~ A * B * C", "three-way star interaction rejected");
  expect_unsupported("~ A:B", "interaction-only design rejected");
}

void test_executor_blocks() {
  const std::size_t threads = ccdeseq2::effective_thread_count(1);
  require(threads == 1, "threads=1 is sequential");
  const std::size_t block_size = ccdeseq2::default_gene_block_size(1000, 4);
  require(block_size >= 64, "default block lower bound");
  const auto blocks = ccdeseq2::make_gene_blocks(130, 64);
  require(blocks.size() == 3, "block count");
  require(blocks[0].begin == 0 && blocks[0].end == 64, "first block");
  require(blocks[2].begin == 128 && blocks[2].end == 130, "last block");
}

void test_profile_json_schema() {
  const auto path =
      std::filesystem::temp_directory_path() / "FLASHDEG_profile_test.json";
  ccdeseq2::ProfileReport profile(true);
  profile.set_metadata(ccdeseq2::numeric_backend_metadata());
  profile.add_wall_time("load_counts_ms", 1.25, 2.5);
  profile.set_peak_memory_mib(12.5);
  profile.write_json(path);

  std::ifstream in(path, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  require(text.find("\"schema_version\": 1") != std::string::npos,
          "profile schema_version");
  require(text.find("\"metadata\"") != std::string::npos,
          "profile metadata object");
  require(text.find("\"linear_algebra_backend\"") != std::string::npos,
          "profile linear algebra backend");
  require(text.find("\"special_function_backend\"") != std::string::npos,
          "profile special backend");
  require(text.find("\"optimizer_backend\"") != std::string::npos,
          "profile optimizer backend");
  require(text.find("\"cpu_ms\"") != std::string::npos, "profile cpu_ms");
  require(text.find("\"peak_memory_mib\"") != std::string::npos,
          "profile peak memory MiB field");
  require(text.find("\"peak_memory_mb\"") == std::string::npos,
          "profile old peak memory field absent");
  in.close();
  std::filesystem::remove(path);
}

void test_mom_dispersions() {
  ccdeseq2::CountMatrix counts({"s1", "s2", "s3", "s4"}, {"g1", "g2"});
  counts(0, 0) = 10.0;
  counts(1, 0) = 12.0;
  counts(2, 0) = 20.0;
  counts(3, 0) = 22.0;
  counts(0, 1) = 1.0;
  counts(1, 1) = 20.0;
  counts(2, 1) = 1.0;
  counts(3, 1) = 20.0;

  ccdeseq2::DesignMatrix design(
      {"s1", "s2", "s3", "s4"}, {"Intercept", "condition[T.B]"},
      {1.0, 0.0, 1.0, 0.0, 1.0, 1.0, 1.0, 1.0}, {});
  ccdeseq2::NormalizedCounts normalized{
      ccdeseq2::NormalizationFactors::sample_wise({1.0, 1.0, 1.0, 1.0}),
      counts,
      {16.0, 10.5}};

  const auto rough = utils::fit_rough_dispersions(counts, design);
  require_near(rough[0], 0.0, 1e-12, "rough dispersion g1");
  require_near(rough[1], 1.4467120181405896, 1e-12, "rough dispersion g2");

  const auto moments =
      utils::fit_moments_dispersions(counts, {1.0, 1.0, 1.0, 1.0});
  require_near(moments[0], 0.07291666666666667, 1e-12,
               "moments dispersion g1");
  require_near(moments[1], 0.9962207105064248, 1e-12,
               "moments dispersion g2");

  ccdeseq2::CountMatrix inf_counts({"s1", "s2"}, {"g1"});
  inf_counts(0, 0) = 1.0;
  inf_counts(1, 0) = -1.0;
  const auto inf_moments =
      utils::fit_moments_dispersions(inf_counts, {1.0, 1.0});
  require_near(inf_moments[0], std::numeric_limits<double>::max(), 0.0,
               "moments dispersion positive infinity nan_to_num");

  const auto mom =
      dds::fit_MoM_dispersions(counts, normalized, design, 1e-8, 10.0);
  require(mom.non_zero[0] && mom.non_zero[1], "MoM non-zero flags");
  require_near(mom.estimates[0], 1e-8, 0.0, "MoM clipped g1");
  require_near(mom.estimates[1], moments[1], 1e-12, "MoM estimate g2");

  const auto mu = utils::fit_lin_mu(counts, normalized, design, 0.5);
  require_near(mu(0, 0), 11.0, 1e-12, "lin mu g1 A");
  require_near(mu(2, 0), 21.0, 1e-12, "lin mu g1 B");

  const auto genewise =
      dds::fit_genewise_dispersions(counts, normalized, design, 0.5,
                                              1e-8, 10.0);
  require(std::isfinite(genewise.genewise[0]), "genewise dispersion g1 finite");
  require(genewise.genewise[0] >= 1e-8 && genewise.genewise[0] <= 10.0,
          "genewise dispersion g1 clipped");

  const auto trend = dds::fit_mean_dispersion_trend(
      genewise.genewise, genewise.non_zero, normalized.base_means, 1e-8);
  require(std::isfinite(trend.mean), "mean dispersion trend finite");
  require_near(trend.fitted[0], trend.mean, 0.0, "mean trend fitted");

  const auto parametric = dds::fit_parametric_dispersion_trend(
      genewise.genewise, genewise.non_zero, normalized.base_means, 1e-8);
  require(std::isfinite(parametric.fitted[0]),
          "parametric dispersion trend finite");

  const auto local = dds::fit_local_dispersion_trend(
      genewise.genewise, genewise.non_zero, normalized.base_means, 1e-8);
  require(local.kind == ccdeseq2::DispersionTrendKind::local,
          "local dispersion trend kind");
  require(std::isfinite(local.fitted[0]), "local dispersion trend finite");

  const std::vector<double> threshold_disps{9.0e-8, 1.0e-7, 2.0e-7};
  const ccdeseq2::ByteMask threshold_non_zero{1, 1, 1};
  const std::vector<double> threshold_means{10.0, 20.0, 30.0};
  const auto threshold_local = dds::fit_local_dispersion_trend(
      threshold_disps, threshold_non_zero, threshold_means, 1.0e-8);
  require(threshold_local.local_log_means.size() == 2,
          "local dispersion trend uses DESeq2 minDisp*10 inclusion rule");

  const auto prior = dds::fit_dispersion_prior(
      genewise.genewise, parametric.fitted, genewise.non_zero, counts.sample_count(),
      design.column_count(), 1e-8);
  require(prior.prior_disp_var >= 0.25, "dispersion prior lower bound");

  const auto map = dds::fit_MAP_dispersions(
      counts, design, genewise.mu_hat, genewise.genewise, parametric.fitted,
      genewise.non_zero, 1e-8, 10.0, prior.prior_disp_var,
      prior.squared_logres);
  require(std::isfinite(map.dispersions[0]), "MAP final dispersion finite");
}

void test_single_factor_dispersion_fixture() {
  const auto root = pyde_reference_fixture_dir();
  const auto input = load_single_factor_inputs();
  const auto normalized =
      dds::fit_size_factors(input.counts, ccdeseq2::SizeFactorFitType::ratio);
  const double max_disp =
      std::max(10.0, static_cast<double>(input.counts.sample_count()));
  const auto genewise = dds::fit_genewise_dispersions(
      input.counts, normalized, input.design, 0.5, 1e-8, max_disp);
  const auto trend = dds::fit_parametric_dispersion_trend(
      genewise.genewise, genewise.non_zero, normalized.base_means, 1e-8);
  const auto prior = dds::fit_dispersion_prior(
      genewise.genewise, trend.fitted, genewise.non_zero,
      input.counts.sample_count(), input.design.column_count(), 1e-8);
  const auto map = dds::fit_MAP_dispersions(
      input.counts, input.design, genewise.mu_hat, genewise.genewise, trend.fitted,
      genewise.non_zero, 1e-8, max_disp, prior.prior_disp_var,
      prior.squared_logres);

  const auto expected = ccdeseq2::read_csv_table(
      root / "tests" / "data" / "single_factor" /
      "r_test_dispersions.csv");
  require(expected.row_names.size() == map.dispersions.size(),
          "dispersion fixture row count");
  for (std::size_t gene = 0; gene < expected.row_names.size(); ++gene) {
    require(expected.row_names[gene] == input.counts.gene_names()[gene],
            "dispersion fixture gene order");
    const double expected_value = std::stod(expected.rows[gene][0]);
    const double rel = std::abs(map.dispersions[gene] - expected_value) /
                       std::max(std::abs(expected_value), 1e-12);
    require(rel <= 0.002, "single-factor dispersion fixture mismatch");
  }

  const auto lfc = dds::fit_LFC(
      input.counts, normalized, input.design, map.dispersions, genewise.non_zero,
      0.5, 1e-8);
  const auto cooks = dds::calculate_cooks_outliers(
      input.counts, normalized, input.design, lfc, genewise.non_zero);
  require(cooks.cooks.sample_count() == input.counts.sample_count(),
          "Cook distance sample count");
  require(cooks.cooks.gene_count() == input.counts.gene_count(),
          "Cook distance gene count");
  require(cooks.pvalue_cooks_outlier.size() == input.counts.gene_count(),
          "Cook p-value outlier mask length");
  require(std::none_of(cooks.pvalue_cooks_outlier.begin(),
                       cooks.pvalue_cooks_outlier.end(),
                       [](std::uint8_t value) { return value != 0; }),
          "single-factor synthetic has no Cook p-value outliers");
  const auto contrast =
      input.design.contrast_vector("condition", "B", "A");
  const auto log2_lfc = ds::contrast_log2_fold_change(lfc, contrast);
  auto compare_wald_fixture =
      [&](const ccdeseq2::WaldSummary& wald,
          const std::filesystem::path& fixture_path, bool abs_stat,
          bool compare_all_pvalues, bool compare_padj,
          const std::string& label) {
        const auto expected_res = ccdeseq2::read_csv_table(fixture_path);
        require(expected_res.row_names.size() == log2_lfc.size(),
                label + " row count");
        for (std::size_t gene = 0; gene < expected_res.row_names.size(); ++gene) {
          require(expected_res.row_names[gene] == input.counts.gene_names()[gene],
                  label + " gene order");
          require_relative_or_absolute_near(
              wald.base_mean[gene], csv_double(expected_res, gene, 0), 1e-6,
              1e-10, label + " baseMean fixture mismatch");
          require_relative_or_absolute_near(
              wald.log2_fold_change[gene], csv_double(expected_res, gene, 1),
              0.02, 1e-10, label + " log2 LFC fixture mismatch");
          require_relative_or_absolute_near(
              wald.lfc_se[gene], csv_double(expected_res, gene, 2), 0.03,
              1e-10, label + " lfcSE fixture mismatch");
          const double actual_stat =
              abs_stat ? std::abs(wald.statistic[gene]) : wald.statistic[gene];
          require_relative_or_absolute_near(
              actual_stat, csv_double(expected_res, gene, 3), 0.04, 1e-10,
              label + " statistic fixture mismatch");
          const double expected_stat = csv_double(expected_res, gene, 3);
          if (compare_all_pvalues || expected_stat != 0.0) {
            require_relative_or_absolute_near(
                wald.pvalue[gene], csv_double(expected_res, gene, 4), 0.08,
                1e-10, label + " pvalue fixture mismatch");
          }
          if (compare_padj) {
            require_relative_or_absolute_near(
                wald.padj[gene], csv_double(expected_res, gene, 5), 0.08, 1e-10,
                label + " padj fixture mismatch");
          }
        }
      };

  ccdeseq2::WaldTestOptions wald_options;
  wald_options.independent_filter = false;
  const auto wald_no_filter = ds::summary(
      input.design, normalized, lfc, map.dispersions, genewise.non_zero,
      contrast, wald_options);
  const auto expected_res = ccdeseq2::read_csv_table(
      root / "tests" / "data" / "single_factor" /
      "r_test_res_no_independent_filtering.csv");
  require(expected_res.row_names.size() == log2_lfc.size(),
          "LFC fixture row count");
  for (std::size_t gene = 0; gene < expected_res.row_names.size(); ++gene) {
    require(expected_res.row_names[gene] == input.counts.gene_names()[gene],
            "LFC fixture gene order");
    const double expected_value = std::stod(expected_res.rows[gene][1]);
    require_relative_near(log2_lfc[gene], expected_value, 0.02,
                          "single-factor log2 LFC fixture mismatch");
  }

  compare_wald_fixture(
      wald_no_filter,
      root / "tests" / "data" / "single_factor" /
          "r_test_res_no_independent_filtering.csv",
      false, true, true, "Wald no-independent-filter");

  wald_options.independent_filter = true;
  const auto wald_filter = ds::summary(
      input.design, normalized, lfc, map.dispersions, genewise.non_zero,
      contrast, wald_options);
  compare_wald_fixture(
      wald_filter,
      root / "tests" / "data" / "single_factor" / "r_test_res.csv",
      false, true, true, "Wald independent-filter");

  struct AltFixture {
    ccdeseq2::AlternativeHypothesis hypothesis;
    double lfc_null = 0.5;
    const char* file_suffix = "";
    bool abs_stat = false;
  };
  const std::vector<AltFixture> alt_fixtures{
      {ccdeseq2::AlternativeHypothesis::less_abs, 0.5, "lessAbs", true},
      {ccdeseq2::AlternativeHypothesis::greater_abs, 0.5, "greaterAbs", false},
      {ccdeseq2::AlternativeHypothesis::less, -0.5, "less", false},
      {ccdeseq2::AlternativeHypothesis::greater, 0.5, "greater", false},
  };
  for (const auto& fixture : alt_fixtures) {
    ccdeseq2::WaldTestOptions alt_options;
    alt_options.independent_filter = true;
    alt_options.alternative = fixture.hypothesis;
    alt_options.lfc_null_log2 = fixture.lfc_null;
    const auto alt_wald = ds::summary(
        input.design, normalized, lfc, map.dispersions, genewise.non_zero,
        contrast, alt_options);
    compare_wald_fixture(
        alt_wald,
        root / "tests" / "data" / "single_factor" /
            ("r_test_res_" + std::string(fixture.file_suffix) + ".csv"),
        fixture.abs_stat, false, false,
        std::string("Wald alt ") + fixture.file_suffix);
  }
}

void test_cook_refit_pipeline_end_to_end() {
  auto input = load_single_factor_inputs();
  input.counts(0, 0) = 100000000.0;

  ccdeseq2::DeseqPipelineOptions no_refit_options;
  no_refit_options.refit_cooks = false;
  no_refit_options.compute_replacement = false;
  no_refit_options.compute_wald = false;
  const auto no_refit = ccdeseq2::run_deseq_pipeline(
      input.counts, input.design,
      input.design.contrast_vector("condition", "B", "A"), no_refit_options);
  require(no_refit.cooks.has_value() &&
              no_refit.cooks->pvalue_cooks_outlier[0] != 0,
          "injected Cook outlier detected before refit");

  ccdeseq2::DeseqPipelineOptions refit_options;
  refit_options.compute_wald = false;
  const auto refit = ccdeseq2::run_deseq_pipeline(
      input.counts, input.design,
      input.design.contrast_vector("condition", "B", "A"), refit_options);
  require(refit.replacement.has_value() && refit.replacement->replaced[0] != 0,
          "injected outlier replacement flag");
  require(refit.replacement->refitted[0] != 0,
          "injected outlier refit flag");
  require(refit.replacement->counts(0, 0) < input.counts(0, 0),
          "injected outlier count replaced downward");
  require(refit.lfc.has_value() && no_refit.lfc.has_value(),
          "Cook refit LFC results are present");
  require(refit.map.has_value() && no_refit.map.has_value(),
          "Cook refit MAP results are present");
  require(std::abs(refit.genewise.mom.estimates[0] -
                   no_refit.genewise.mom.estimates[0]) > 1e-6,
          "Cook refit copies MoM dispersion back to parent");
  require(std::abs(refit.genewise.mu_hat(0, 0) -
                   no_refit.genewise.mu_hat(0, 0)) > 1e-3,
          "Cook refit copies mu_hat back to parent");
  require(std::abs(refit.map->map[0] - no_refit.map->map[0]) > 1e-6,
          "Cook refit copies MAP dispersion back to parent");
  require(std::abs(refit.lfc->lfc_row_major[1] -
                   no_refit.lfc->lfc_row_major[1]) > 1e-3,
          "Cook refit changes injected-gene LFC");
  require(refit.cooks.has_value() &&
              refit.cooks->pvalue_cooks_outlier[0] == 0,
          "refitted gene is removed from Cook p-value filtering");
}

void test_multi_factor_fixture() {
  const auto root = pyde_reference_fixture_dir();
  const auto counts = ccdeseq2::read_count_matrix(
      root / "datasets" / "synthetic" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  const auto metadata = ccdeseq2::read_metadata_table(
      root / "datasets" / "synthetic" / "test_metadata.csv");
  const auto design =
      ccdeseq2::build_design_matrix(metadata, counts.sample_names(),
                                    "~ group + condition", {});
  const auto contrast = design.contrast_vector("condition", "B", "A");
  const auto run = run_pipeline_for_test(counts, design, contrast);
  compare_wald_fixture(
      run.summary, counts.gene_names(),
      root / "tests" / "data" / "multi_factor" / "r_test_res.csv",
      0.04, "multi-factor");
}

void test_multi_factor_outlier_fixture() {
  const auto root = pyde_reference_fixture_dir();
  auto counts = ccdeseq2::read_count_matrix(
      root / "datasets" / "synthetic" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  auto metadata = ccdeseq2::read_metadata_table(
      root / "datasets" / "synthetic" / "test_metadata.csv");
  // Mirrors PyDESeq2 test_multifactor_deseq(with_outliers=True).
  counts(counts.sample_index("sample1"), counts.gene_index("gene1")) = 2000.0;
  counts(counts.sample_index("sample11"), counts.gene_index("gene7")) = 1000.0;
  metadata = replace_metadata_value(metadata, "sample1", "condition", "C");
  const auto design =
      ccdeseq2::build_design_matrix(metadata, counts.sample_names(),
                                    "~ group + condition", {});
  const auto contrast = design.contrast_vector("condition", "B", "A");
  const auto run = run_pipeline_for_test(counts, design, contrast);
  compare_wald_fixture(
      run.summary, counts.gene_names(),
      root / "tests" / "data" / "multi_factor" /
          "r_test_res_outliers.csv",
      0.04, "multi-factor outlier");
}

void test_continuous_fixture(bool with_outliers) {
  const auto root = pyde_reference_fixture_dir();
  auto counts = ccdeseq2::read_count_matrix(
      root / "tests" / "data" / "continuous" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  auto metadata = ccdeseq2::read_metadata_table(
      root / "tests" / "data" / "continuous" /
      "test_metadata.csv");
  if (with_outliers) {
    // Mirrors PyDESeq2 test_continuous_deseq(with_outliers=True).
    counts(counts.sample_index("sample1"), counts.gene_index("gene1")) = 2000.0;
    counts(counts.sample_index("sample11"), counts.gene_index("gene7")) = 1000.0;
    metadata = replace_metadata_value(metadata, "sample1", "condition", "C");
  }
  const auto design =
      ccdeseq2::build_design_matrix(metadata, counts.sample_names(),
                                    "~ group + condition + measurement", {});
  std::vector<double> contrast(design.column_count(), 0.0);
  contrast.back() = 1.0;
  const auto run = run_pipeline_for_test(counts, design, contrast);
  compare_wald_fixture(
      run.summary, counts.gene_names(),
      root / "tests" / "data" / "continuous" /
          (with_outliers ? "r_test_res_outliers.csv" : "r_test_res.csv"),
      0.04, with_outliers ? "continuous outlier" : "continuous");
}

void test_wide_fixture() {
  const auto root = pyde_reference_fixture_dir();
  const auto counts = ccdeseq2::read_count_matrix(
      root / "tests" / "data" / "wide" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  const auto metadata = ccdeseq2::read_metadata_table(
      root / "tests" / "data" / "wide" / "test_metadata.csv");
  const auto design =
      ccdeseq2::build_design_matrix(metadata, counts.sample_names(),
                                    "~ group + condition", {});
  const auto contrast = design.contrast_vector("condition", "B", "A");
  const auto run = run_pipeline_for_test(counts, design, contrast);
  compare_wald_fixture(
      run.summary, counts.gene_names(),
      root / "tests" / "data" / "wide" / "r_test_res.csv", 0.02,
      "wide");
}

void test_dispersion_parallel_consistency() {
  const auto input = load_single_factor_inputs();
  const auto wide_counts = repeat_genes(input.counts, 7);
  const auto wide_normalized =
      dds::fit_size_factors(wide_counts, ccdeseq2::SizeFactorFitType::ratio);
  const double wide_max_disp =
      std::max(10.0, static_cast<double>(wide_counts.sample_count()));
  const auto wide_genewise = dds::fit_genewise_dispersions(
      wide_counts, wide_normalized, input.design, 0.5, 1e-8, wide_max_disp, 1,
      true);
  const auto wide_genewise_deterministic =
      dds::fit_genewise_dispersions(
          wide_counts, wide_normalized, input.design, 0.5, 1e-8, wide_max_disp, 4,
          true);
  const auto wide_genewise_parallel = dds::fit_genewise_dispersions(
      wide_counts, wide_normalized, input.design, 0.5, 1e-8, wide_max_disp, 4,
      false);
  const auto wide_trend = dds::fit_parametric_dispersion_trend(
      wide_genewise.genewise, wide_genewise.non_zero, wide_normalized.base_means,
      1e-8);
  const auto wide_prior = dds::fit_dispersion_prior(
      wide_genewise.genewise, wide_trend.fitted, wide_genewise.non_zero,
      wide_counts.sample_count(), input.design.column_count(), 1e-8);
  const auto wide_map = dds::fit_MAP_dispersions(
      wide_counts, input.design, wide_genewise.mu_hat, wide_genewise.genewise,
      wide_trend.fitted, wide_genewise.non_zero, 1e-8, wide_max_disp,
      wide_prior.prior_disp_var, wide_prior.squared_logres, 1, true);
  const auto wide_map_deterministic = dds::fit_MAP_dispersions(
      wide_counts, input.design, wide_genewise.mu_hat, wide_genewise.genewise,
      wide_trend.fitted, wide_genewise.non_zero, 1e-8, wide_max_disp,
      wide_prior.prior_disp_var, wide_prior.squared_logres, 4, true);
  const auto wide_map_parallel = dds::fit_MAP_dispersions(
      wide_counts, input.design, wide_genewise.mu_hat, wide_genewise.genewise,
      wide_trend.fitted, wide_genewise.non_zero, 1e-8, wide_max_disp,
      wide_prior.prior_disp_var, wide_prior.squared_logres, 4, false);

  for (std::size_t gene = 0; gene < wide_counts.gene_count(); ++gene) {
    require_near(wide_genewise_deterministic.genewise[gene],
                 wide_genewise.genewise[gene], 0.0,
                 "deterministic genewise dispersion mismatch");
    require_relative_near(wide_genewise_parallel.genewise[gene],
                          wide_genewise.genewise[gene], 1e-12,
                          "parallel genewise dispersion mismatch");
    require_near(wide_map_deterministic.dispersions[gene],
                 wide_map.dispersions[gene], 0.0,
                 "deterministic MAP dispersion mismatch");
    require_relative_near(wide_map_parallel.dispersions[gene],
                          wide_map.dispersions[gene], 1e-12,
                          "parallel MAP dispersion mismatch");
  }
}

void test_intercept_only_design_formula() {
  const ccdeseq2::MetadataTable metadata(
      {"s1", "s2", "s3", "s4"}, {"batch", "condition"},
      {{"b1", "ctrl"},
       {"b1", "trt"},
       {"b2", "ctrl"},
       {"b2", "trt"}});
  const std::vector<std::string> samples{"s1", "s2", "s3", "s4"};

  // "~ 1" and "~1" are intercept-only designs.
  for (const std::string& formula : std::vector<std::string>{"~ 1", "~1"}) {
    const auto design =
        ccdeseq2::build_design_matrix(metadata, samples, formula, {});
    require(design.column_count() == 1, "intercept-only has one column");
    require(design.column_names()[0] == "Intercept",
            "intercept-only column is named Intercept");
    for (std::size_t s = 0; s < samples.size(); ++s) {
      require_near(design(s, 0), 1.0, 0.0, "intercept-only values are 1");
    }
  }

  // An explicit "1" term is redundant: "~ 1 + batch" == "~ batch + 1" == "~ batch".
  const auto base =
      ccdeseq2::build_design_matrix(metadata, samples, "~ batch", {});
  for (const std::string& formula :
       std::vector<std::string>{"~ 1 + batch", "~ batch + 1"}) {
    const auto design =
        ccdeseq2::build_design_matrix(metadata, samples, formula, {});
    require(design.column_names() == base.column_names(),
            "explicit intercept term keeps the same columns");
    require(design.values_row_major() == base.values_row_major(),
            "explicit intercept term keeps the same values");
  }

  // "~ 0" / "~ -1" (no intercept) remain unsupported.
  for (const std::string& formula : std::vector<std::string>{"~ 0", "~ -1"}) {
    bool threw = false;
    try {
      (void)ccdeseq2::build_design_matrix(metadata, samples, formula, {});
    } catch (const ccdeseq2::Error& error) {
      threw = error.code() == ccdeseq2::ExitCode::unsupported;
    }
    require(threw, "no-intercept formulas remain unsupported");
  }
}

void test_chi_square_sf() {
  using ccdeseq2::chi_square_sf;

  // Endpoints and invalid arguments.
  require_near(chi_square_sf(0.0, 1.0), 1.0, 0.0, "chi-square sf at x=0 is 1");
  require_near(chi_square_sf(-3.0, 5.0), 1.0, 0.0, "chi-square sf for x<0 is 1");
  require(std::isnan(chi_square_sf(1.0, 0.0)), "chi-square df=0 -> NaN");
  require(std::isnan(chi_square_sf(1.0, -2.0)), "chi-square df<0 -> NaN");
  require(std::isnan(chi_square_sf(std::numeric_limits<double>::infinity(), 1.0)),
          "chi-square non-finite x -> NaN");

  // df=2 closed form: sf(x) = exp(-x/2).
  for (const double x : {0.5, 1.0, 2.0, 5.0, 10.0, 25.0}) {
    require_relative_or_absolute_near(chi_square_sf(x, 2.0), std::exp(-0.5 * x),
                                      1e-11, 1e-300,
                                      "chi-square df=2 closed form");
  }
  // df=4 closed form: sf(x) = exp(-x/2) * (1 + x/2).
  for (const double x : {0.5, 1.0, 2.0, 5.0, 10.0}) {
    require_relative_or_absolute_near(chi_square_sf(x, 4.0),
                                      std::exp(-0.5 * x) * (1.0 + 0.5 * x),
                                      1e-11, 1e-300,
                                      "chi-square df=4 closed form");
  }
  // df=1: sf(x) = 2 * normal_sf(sqrt(x)), since chi-square(1) = Z^2.
  for (const double x : {0.25, 1.0, 3.841458820694124, 6.0}) {
    require_relative_or_absolute_near(chi_square_sf(x, 1.0),
                                      2.0 * ccdeseq2::normal_sf(std::sqrt(x)),
                                      1e-10, 1e-12,
                                      "chi-square df=1 via normal sf");
  }
  // Known upper 5% quantiles.
  require_near(chi_square_sf(3.841458820694124, 1.0), 0.05, 1e-9, "chi df1 95%");
  require_near(chi_square_sf(5.991464547107979, 2.0), 0.05, 1e-9, "chi df2 95%");
  require_near(chi_square_sf(18.307038053275146, 10.0), 0.05, 1e-7, "chi df10 95%");

  // Monotone decreasing; tiny in the extreme upper tail.
  require(chi_square_sf(40.0, 50.0) > chi_square_sf(80.0, 50.0),
          "chi-square sf decreases in x");
  const double tiny = chi_square_sf(1.0e6, 10.0);
  require(tiny >= 0.0 && tiny < 1e-12, "chi-square sf tiny for huge x");
}

void test_nested_design_validation() {
  const ccdeseq2::MetadataTable metadata(
      {"s1", "s2", "s3", "s4", "s5", "s6"}, {"batch", "condition"},
      {{"b1", "ctrl"},
       {"b1", "trt"},
       {"b2", "ctrl"},
       {"b2", "trt"},
       {"b1", "ctrl"},
       {"b2", "trt"}});
  const std::vector<std::string> samples{"s1", "s2", "s3", "s4", "s5", "s6"};

  const auto full_bc = ccdeseq2::build_design_matrix(
      metadata, samples, "~ batch + condition", {});
  const auto reduced_b =
      ccdeseq2::build_design_matrix(metadata, samples, "~ batch", {});
  const auto reduced_1 =
      ccdeseq2::build_design_matrix(metadata, samples, "~ 1", {});
  const auto full_c =
      ccdeseq2::build_design_matrix(metadata, samples, "~ condition", {});

  // ~batch+condition vs ~batch: nested, df = 1.
  const auto v1 = ccdeseq2::validate_nested_designs(full_bc, reduced_b);
  require(v1.degrees_of_freedom == 1, "df for dropping condition is 1");
  require(v1.full_rank == 3 && v1.reduced_rank == 2, "nested ranks");

  // ~condition vs ~1: nested, df = 1.
  const auto v2 = ccdeseq2::validate_nested_designs(full_c, reduced_1);
  require(v2.degrees_of_freedom == 1, "df for condition vs intercept is 1");

  const auto expect_error = [](const ccdeseq2::DesignMatrix& full,
                               const ccdeseq2::DesignMatrix& reduced,
                               const std::string& label) {
    bool threw = false;
    try {
      (void)ccdeseq2::validate_nested_designs(full, reduced);
    } catch (const ccdeseq2::Error& error) {
      threw = error.code() == ccdeseq2::ExitCode::input_error;
    }
    require(threw, label);
  };

  // df == 0: identical designs.
  expect_error(full_bc, full_bc, "identical designs -> df==0 error");
  // non-nested: ~condition vs ~batch (batch not in span of {1, condition}).
  expect_error(full_c, reduced_b, "non-nested reduced -> error");
  // sample mismatch.
  const ccdeseq2::MetadataTable metadata_small(
      {"s1", "s2", "s3"}, {"condition"}, {{"ctrl"}, {"trt"}, {"ctrl"}});
  const auto small_c = ccdeseq2::build_design_matrix(
      metadata_small, {"s1", "s2", "s3"}, "~ condition", {});
  expect_error(full_c, small_c, "sample mismatch -> error");
}

void test_lrt_summary_basic() {
  const auto root = pyde_reference_fixture_dir();
  auto counts = ccdeseq2::read_count_matrix(
      root / "datasets" / "synthetic" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  const auto metadata = ccdeseq2::read_metadata_table(
      root / "datasets" / "synthetic" / "test_metadata.csv");
  const auto full = ccdeseq2::build_design_matrix(
      metadata, counts.sample_names(), "~ condition", {});
  const auto reduced = ccdeseq2::build_design_matrix(
      metadata, counts.sample_names(), "~ 1", {});
  const auto nested = ccdeseq2::validate_nested_designs(full, reduced);

  // Dispersions are fit on the FULL design; full and reduced LFC then share
  // those dispersions (no Cook refit in this focused unit test).
  const auto normalized =
      dds::fit_size_factors(counts, ccdeseq2::SizeFactorFitType::ratio);
  const double max_disp =
      std::max(10.0, static_cast<double>(counts.sample_count()));
  const auto genewise = dds::fit_genewise_dispersions(
      counts, normalized, full, 0.5, 1e-8, max_disp, 1, true);
  const auto trend = dds::fit_dispersion_trend(
      genewise.genewise, genewise.non_zero, normalized.base_means, 1e-8,
      ccdeseq2::DispersionTrendKind::parametric);
  const auto prior = dds::fit_dispersion_prior(
      genewise.genewise, trend.fitted, genewise.non_zero, counts.sample_count(),
      full.column_count(), 1e-8);
  const auto map = dds::fit_MAP_dispersions(
      counts, full, genewise.mu_hat, genewise.genewise, trend.fitted,
      genewise.non_zero, 1e-8, max_disp, prior.prior_disp_var,
      prior.squared_logres, 1, true);
  const auto full_lfc = dds::fit_LFC(counts, normalized, full, map.dispersions,
                                     genewise.non_zero, 0.5, 1e-8, 1, true);
  const auto reduced_lfc =
      dds::fit_LFC(counts, normalized, reduced, map.dispersions,
                   genewise.non_zero, 0.5, 1e-8, 1, true);

  const auto contrast = full.contrast_vector("condition", "B", "A");
  ccdeseq2::LrtTestOptions opts;
  opts.degrees_of_freedom = nested.degrees_of_freedom;
  opts.min_mu = 0.5;
  opts.independent_filter = true;
  const auto summary = ds::summary_lrt(counts, full, normalized, full_lfc,
                                       reduced_lfc, map.dispersions,
                                       genewise.non_zero, contrast, opts);

  const double df = static_cast<double>(nested.degrees_of_freedom);
  double min_raw_stat = 0.0;
  std::size_t finite_pvalues = 0;
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    if (!genewise.non_zero[gene]) {
      require(std::isnan(summary.pvalue[gene]),
              "all-zero gene has NaN LRT pvalue");
      continue;
    }
    if (!full_lfc.converged[gene] || !reduced_lfc.converged[gene]) {
      continue;  // NA is allowed for untestable genes.
    }
    // Independently recompute the raw LRT statistic from the fitted means.
    const std::span<const double> y(counts.gene_data(gene),
                                    counts.sample_count());
    const std::span<const double> mu_full(full_lfc.mu.gene_data(gene),
                                          counts.sample_count());
    const std::span<const double> mu_red(reduced_lfc.mu.gene_data(gene),
                                         counts.sample_count());
    const double raw =
        2.0 * (ccdeseq2::negative_binomial_nll(y, mu_red, map.dispersions[gene]) -
               ccdeseq2::negative_binomial_nll(y, mu_full, map.dispersions[gene]));
    if (std::isfinite(raw)) {
      min_raw_stat = std::min(min_raw_stat, raw);
    }
    if (std::isfinite(summary.pvalue[gene])) {
      ++finite_pvalues;
      // The reported statistic is the raw 2*(NLL_red - NLL_full) with no
      // clamping, matching DESeq2 even for small negatives.
      require_relative_or_absolute_near(
          summary.statistic[gene], raw, 1e-9, 1e-9,
          "LRT statistic equals 2*(NLL_red - NLL_full)");
      require_relative_or_absolute_near(
          summary.pvalue[gene],
          ccdeseq2::chi_square_sf(summary.statistic[gene], df), 1e-12, 1e-15,
          "LRT pvalue == chi_square_sf(stat, df)");
    }
  }
  require(finite_pvalues > 0, "LRT produced finite pvalues");
  require(min_raw_stat > -1e-6,
          "raw LRT statistics are non-negative within tolerance");

  // Mask handling: cooks -> NaN, new_all_zeroes -> pvalue=1 override.
  std::size_t g_cook = counts.gene_count();
  std::size_t g_zero = counts.gene_count();
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    if (!genewise.non_zero[gene]) {
      continue;
    }
    if (g_cook == counts.gene_count()) {
      g_cook = gene;
    } else if (g_zero == counts.gene_count()) {
      g_zero = gene;
      break;
    }
  }
  require(g_cook < counts.gene_count() && g_zero < counts.gene_count(),
          "found two non-zero genes for mask test");
  ccdeseq2::ByteMask cooks_mask(counts.gene_count(), 0);
  ccdeseq2::ByteMask new_all_zero_mask(counts.gene_count(), 0);
  cooks_mask[g_cook] = 1;
  new_all_zero_mask[g_zero] = 1;
  ccdeseq2::LrtTestOptions masked = opts;
  masked.cooks_outlier = &cooks_mask;
  masked.new_all_zeroes = &new_all_zero_mask;
  const auto masked_summary =
      ds::summary_lrt(counts, full, normalized, full_lfc, reduced_lfc,
                      map.dispersions, genewise.non_zero, contrast, masked);
  require(std::isnan(masked_summary.pvalue[g_cook]),
          "cooks-outlier gene has NaN LRT pvalue");
  require(masked_summary.pvalue[g_zero] == 1.0,
          "new-all-zero gene has LRT pvalue 1");
  require(masked_summary.statistic[g_zero] == 0.0,
          "new-all-zero gene has LRT statistic 0");
  require(masked_summary.lfc_se[g_zero] == 0.0,
          "new-all-zero gene has LRT lfcSE 0");
}

void test_lrt_pipeline_end_to_end() {
  const auto root = pyde_reference_fixture_dir();
  auto counts = ccdeseq2::read_count_matrix(
      root / "datasets" / "synthetic" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  const auto metadata = ccdeseq2::read_metadata_table(
      root / "datasets" / "synthetic" / "test_metadata.csv");
  const auto full = ccdeseq2::build_design_matrix(
      metadata, counts.sample_names(), "~ condition", {});
  const auto reduced = ccdeseq2::build_design_matrix(
      metadata, counts.sample_names(), "~ 1", {});
  const auto nested = ccdeseq2::validate_nested_designs(full, reduced);
  const auto contrast = full.contrast_vector("condition", "B", "A");

  ccdeseq2::DeseqPipelineOptions options;
  options.test_kind = ccdeseq2::StatisticalTestKind::lrt;
  options.lrt_degrees_of_freedom = nested.degrees_of_freedom;
  options.wald_options.independent_filter = true;
  const auto result = ccdeseq2::run_deseq_pipeline(counts, full, contrast,
                                                   options, nullptr, &reduced);
  require(result.summary.has_value(), "LRT pipeline produced a summary");
  require(result.reduced_lfc.has_value(), "LRT pipeline fit a reduced model");

  const double df = static_cast<double>(nested.degrees_of_freedom);
  std::size_t finite = 0;
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    const double pv = result.summary->pvalue[gene];
    if (std::isfinite(pv)) {
      ++finite;
      // statistic is raw (small negatives kept to match DESeq2); the pvalue is
      // its chi-square survival, which is 1 for any non-positive statistic.
      require_relative_or_absolute_near(
          pv, ccdeseq2::chi_square_sf(result.summary->statistic[gene], df),
          1e-12, 1e-15, "pipeline LRT pvalue == chi_square_sf(stat, df)");
    }
  }
  require(finite > 0, "pipeline LRT produced finite pvalues");

  // A Wald run on the same design must be unchanged in shape and must differ in
  // its test statistics (sanity that the LRT path is actually doing the LRT).
  ccdeseq2::DeseqPipelineOptions wald_only;
  wald_only.wald_options.independent_filter = true;
  const auto wald =
      ccdeseq2::run_deseq_pipeline(counts, full, contrast, wald_only);
  require(!wald.reduced_lfc.has_value(), "Wald run does not fit a reduced model");
  bool differs = false;
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    const double a = result.summary->pvalue[gene];
    const double b = wald.summary->pvalue[gene];
    if (std::isfinite(a) && std::isfinite(b) && std::abs(a - b) > 1e-6) {
      differs = true;
    }
    // baseMean is identical (same full-model normalization).
    require_relative_or_absolute_near(result.summary->base_mean[gene],
                                      wald.summary->base_mean[gene], 1e-12,
                                      1e-12, "LRT and Wald share baseMean");
  }
  require(differs, "LRT and Wald pvalues differ on some gene");
}

void test_lrt_pipeline_rejections() {
  const auto root = pyde_reference_fixture_dir();
  auto counts = ccdeseq2::read_count_matrix(
      root / "datasets" / "synthetic" / "test_counts.csv",
      ccdeseq2::CountOrientation::features_as_rows);
  const auto metadata = ccdeseq2::read_metadata_table(
      root / "datasets" / "synthetic" / "test_metadata.csv");
  const auto full = ccdeseq2::build_design_matrix(
      metadata, counts.sample_names(), "~ condition", {});
  const auto reduced = ccdeseq2::build_design_matrix(
      metadata, counts.sample_names(), "~ 1", {});
  const auto contrast = full.contrast_vector("condition", "B", "A");

  const auto expect_throw = [&](const ccdeseq2::DeseqPipelineOptions& opts,
                                const ccdeseq2::DesignMatrix* red,
                                const std::string& label) {
    bool threw = false;
    try {
      (void)ccdeseq2::run_deseq_pipeline(counts, full, contrast, opts, nullptr,
                                         red);
    } catch (const ccdeseq2::Error&) {
      threw = true;
    }
    require(threw, label);
  };

  // LRT requested without a reduced design.
  ccdeseq2::DeseqPipelineOptions no_reduced;
  no_reduced.test_kind = ccdeseq2::StatisticalTestKind::lrt;
  no_reduced.lrt_degrees_of_freedom = 1;
  expect_throw(no_reduced, nullptr, "LRT without reduced design throws");

  // LRT with degrees of freedom left at 0.
  ccdeseq2::DeseqPipelineOptions zero_df;
  zero_df.test_kind = ccdeseq2::StatisticalTestKind::lrt;
  zero_df.lrt_degrees_of_freedom = 0;
  expect_throw(zero_df, &reduced, "LRT with df==0 throws");

  // LRT combined with LFC shrinkage.
  ccdeseq2::DeseqPipelineOptions with_shrink;
  with_shrink.test_kind = ccdeseq2::StatisticalTestKind::lrt;
  with_shrink.lrt_degrees_of_freedom = 1;
  with_shrink.compute_lfc_shrink = true;
  expect_throw(with_shrink, &reduced, "LRT with lfc-shrink throws");
}

void test_lrt_raw_negative_statistic() {
  // Construct full/reduced LFC fits where the full-model mean is a deliberately
  // poor fit and the reduced-model mean is a good fit, so the raw statistic
  // 2*(NLL_reduced - NLL_full) is negative. summary_lrt must keep the raw
  // negative statistic (no clamp to 0) with pvalue == 1, matching DESeq2 and
  // guarding against re-introducing a clamp.
  const std::vector<std::string> samples{"s1", "s2", "s3", "s4"};
  const std::vector<std::string> genes{"g1"};
  ccdeseq2::CountMatrix counts(samples, genes);
  const double y[4] = {10.0, 12.0, 9.0, 11.0};
  for (std::size_t s = 0; s < samples.size(); ++s) counts(s, 0) = y[s];
  const auto normalized =
      dds::fit_size_factors(counts, ccdeseq2::SizeFactorFitType::ratio);

  // Intercept + x design (2 columns); factors are unused by summary_lrt.
  const ccdeseq2::DesignMatrix full_design(
      samples, {"Intercept", "x"}, {1.0, 0.0, 1.0, 1.0, 1.0, 0.0, 1.0, 1.0}, {});

  const auto make_lfc = [&](double mu_value, double beta1) {
    ccdeseq2::LFCFit fit;
    fit.lfc_row_major = {0.0, beta1};
    fit.mu = ccdeseq2::CountMatrix(samples, genes);
    fit.hat_diagonals = ccdeseq2::CountMatrix(samples, genes);
    for (std::size_t s = 0; s < samples.size(); ++s) {
      fit.mu(s, 0) = mu_value;
      fit.hat_diagonals(s, 0) = 0.0;
    }
    fit.converged = ccdeseq2::ByteMask{1};
    fit.iterations = {1.0};
    fit.fallback = ccdeseq2::ByteMask{0};
    return fit;
  };
  const auto full_lfc = make_lfc(30.0, 0.5);     // poor fit (mu >> counts)
  const auto reduced_lfc = make_lfc(10.5, 0.0);  // good fit (mu ~ counts)

  const std::vector<double> dispersions{0.1};
  const ccdeseq2::ByteMask non_zero{1};
  const std::vector<double> contrast{0.0, 1.0};
  ccdeseq2::LrtTestOptions opts;
  opts.degrees_of_freedom = 1;
  opts.independent_filter = false;

  const auto summary =
      ds::summary_lrt(counts, full_design, normalized, full_lfc, reduced_lfc,
                      dispersions, non_zero, contrast, opts);
  require(summary.statistic[0] < 0.0,
          "raw negative LRT statistic is preserved (not clamped to 0)");
  require(summary.pvalue[0] == 1.0,
          "negative LRT statistic yields pvalue 1");
}

}  // namespace

int main() {
  try {
    test_numpy_compat_primitives();
    test_linalg_backend_primitives();
    test_optimizer_primitives();
    test_pydeseq2_facade_api();
    test_vst_transform_known_values();
    test_single_factor_lfc_shrink_fixture();
    test_shrinkage_loss_formula();
    test_nbinom_glm_compat_mode_shrinkage_semantics();
    test_csv_parser();
    test_tximport_round_count_parser();
    test_trigamma_known_values();
    test_nb_nll_derivative_alpha();
    test_bh_adjustment();
    test_lowess_and_independent_filter();
    test_lowess_r_compat_selection();
    test_parametric_trend_fit_step_halving();
    test_wald_nonconverged_adjustment();
    test_wald_min_mu_floor_bounds_se();
    test_cooks_replicate_mask();
    test_count_orientations();
    test_output_writers_create_parent_directories();
    test_counts_design_and_size_factors();
    test_design_interaction();
    test_intercept_only_design_formula();
    test_chi_square_sf();
    test_nested_design_validation();
    test_lrt_summary_basic();
    test_lrt_pipeline_end_to_end();
    test_lrt_pipeline_rejections();
    test_lrt_raw_negative_statistic();
    test_executor_blocks();
    test_profile_json_schema();
    test_mom_dispersions();
    test_single_factor_dispersion_fixture();
    test_cook_refit_pipeline_end_to_end();
    test_multi_factor_fixture();
    test_multi_factor_outlier_fixture();
    test_continuous_fixture(false);
    test_continuous_fixture(true);
    test_wide_fixture();
    test_dispersion_parallel_consistency();
  } catch (const std::exception& error) {
    std::cerr << "test failure: " << error.what() << '\n';
    return 1;
  }
  std::cout << "FLASHDEG_tests passed\n";
  return 0;
}
