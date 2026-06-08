#include "ccdeseq2/pipeline.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

#include "ccdeseq2/pydeseq2_dds.hpp"
#include "ccdeseq2/pydeseq2_ds.hpp"

namespace ccdeseq2 {
namespace {

namespace dds = ccdeseq2::pydeseq2::dds;
namespace ds = ccdeseq2::pydeseq2::ds;

constexpr double kLog2 = 0.69314718055994530942;

[[nodiscard]] bool any_mask(const ByteMask& mask) {
  return std::any_of(mask.begin(), mask.end(),
                     [](std::uint8_t value) { return value != 0; });
}

[[nodiscard]] std::vector<std::size_t> mask_indices(const ByteMask& mask) {
  std::vector<std::size_t> indices;
  for (std::size_t i = 0; i < mask.size(); ++i) {
    if (mask[i] != 0) {
      indices.push_back(i);
    }
  }
  return indices;
}

[[nodiscard]] CountMatrix subset_genes(
    const CountMatrix& matrix, const std::vector<std::size_t>& gene_indices) {
  std::vector<std::string> gene_names;
  gene_names.reserve(gene_indices.size());
  for (const std::size_t gene : gene_indices) {
    gene_names.push_back(matrix.gene_names()[gene]);
  }
  CountMatrix subset(matrix.sample_names(), std::move(gene_names));
  for (std::size_t out_gene = 0; out_gene < gene_indices.size(); ++out_gene) {
    const std::size_t in_gene = gene_indices[out_gene];
    for (std::size_t sample = 0; sample < matrix.sample_count(); ++sample) {
      subset(sample, out_gene) = matrix(sample, in_gene);
    }
  }
  return subset;
}

void copy_refit_results(const std::vector<std::size_t>& gene_indices,
                        const NormalizedCounts& sub_normalized,
                        const GeneWiseDispersions& sub_genewise,
                        const std::vector<double>& sub_fitted,
                        const MAPDispersions& sub_map, const LFCFit& sub_lfc,
                        NormalizedCounts& normalized,
                        GeneWiseDispersions& genewise,
                        DispersionTrendFit& trend, MAPDispersions& map,
                        LFCFit& lfc) {
  const std::size_t p = sub_lfc.lfc_row_major.size() / gene_indices.size();
  for (std::size_t sub_gene = 0; sub_gene < gene_indices.size(); ++sub_gene) {
    const std::size_t gene = gene_indices[sub_gene];
    normalized.base_means[gene] = sub_normalized.base_means[sub_gene];
    for (std::size_t sample = 0;
         sample < normalized.normalized_counts.sample_count(); ++sample) {
      genewise.mu_hat(sample, gene) = sub_genewise.mu_hat(sample, sub_gene);
      lfc.mu(sample, gene) = sub_lfc.mu(sample, sub_gene);
      lfc.hat_diagonals(sample, gene) =
          sub_lfc.hat_diagonals(sample, sub_gene);
    }
    genewise.non_zero[gene] = sub_genewise.non_zero[sub_gene];
    genewise.mom.non_zero[gene] = sub_genewise.mom.non_zero[sub_gene];
    genewise.mom.rough[gene] = sub_genewise.mom.rough[sub_gene];
    genewise.mom.moments[gene] = sub_genewise.mom.moments[sub_gene];
    genewise.mom.estimates[gene] = sub_genewise.mom.estimates[sub_gene];
    genewise.genewise[gene] = sub_genewise.genewise[sub_gene];
    genewise.converged[gene] = sub_genewise.converged[sub_gene];
    if (genewise.iterations.size() == normalized.base_means.size() &&
        sub_genewise.iterations.size() == gene_indices.size()) {
      genewise.iterations[gene] = sub_genewise.iterations[sub_gene];
    }
    if (trend.fitted.size() == normalized.base_means.size()) {
      trend.fitted[gene] = sub_fitted[sub_gene];
    }
    map.map[gene] = sub_map.map[sub_gene];
    map.dispersions[gene] = sub_map.dispersions[sub_gene];
    map.outlier[gene] = sub_map.outlier[sub_gene];
    map.converged[gene] = sub_map.converged[sub_gene];
    lfc.converged[gene] = sub_lfc.converged[sub_gene];
    if (lfc.iterations.size() == normalized.base_means.size() &&
        sub_lfc.iterations.size() == gene_indices.size()) {
      lfc.iterations[gene] = sub_lfc.iterations[sub_gene];
    }
    if (lfc.fallback.size() == normalized.base_means.size() &&
        sub_lfc.fallback.size() == gene_indices.size()) {
      lfc.fallback[gene] = sub_lfc.fallback[sub_gene];
    }
    for (std::size_t col = 0; col < p; ++col) {
      lfc.lfc_row_major[gene * p + col] =
          sub_lfc.lfc_row_major[sub_gene * p + col];
    }
  }
}

void apply_new_all_zeroes(const ByteMask& new_all_zeroes,
                          const DesignMatrix& design_matrix,
                          NormalizedCounts& normalized, LFCFit& lfc) {
  if (!any_mask(new_all_zeroes)) {
    return;
  }
  const std::size_t p = design_matrix.column_count();
  for (std::size_t gene = 0; gene < new_all_zeroes.size(); ++gene) {
    if (new_all_zeroes[gene] == 0) {
      continue;
    }
    normalized.base_means[gene] = 0.0;
    for (std::size_t col = 0; col < p; ++col) {
      lfc.lfc_row_major[gene * p + col] = 0.0;
    }
  }
}

void clear_new_all_zeroes_from_cooks(const ByteMask& new_all_zeroes,
                                     ByteMask& cooks_outlier) {
  const std::size_t n = std::min(new_all_zeroes.size(), cooks_outlier.size());
  for (std::size_t gene = 0; gene < n; ++gene) {
    if (new_all_zeroes[gene] != 0) {
      cooks_outlier[gene] = 0;
    }
  }
}

}  // namespace

DeseqPipelineResult run_deseq_pipeline(const CountMatrix& counts,
                                       const DesignMatrix& design_matrix,
                                       const std::vector<double>& contrast,
                                       const DeseqPipelineOptions& options,
                                       ProfileReport* profile) {
  DeseqPipelineResult result;
  result.effective_max_disp =
      std::max(options.max_disp, static_cast<double>(counts.sample_count()));

  const bool compute_wald = options.compute_wald || options.compute_lfc_shrink;
  const bool compute_lfc = options.compute_lfc || options.compute_cooks ||
                           options.compute_replacement || compute_wald;
  const bool compute_map = options.compute_map || compute_lfc;
  const bool compute_trend = options.compute_trend || compute_map;
  const bool compute_cooks = options.compute_cooks ||
      options.compute_replacement || (compute_wald && options.cooks_filter);
  const bool compute_replacement =
      options.compute_replacement || options.refit_cooks;

  {
    ScopedProfileTimer timer(profile, "size_factor_ms");
    result.normalized =
        dds::fit_size_factors(counts, options.size_factor_fit_type);
  }

  if (options.compute_vst) {
    ScopedProfileTimer timer(profile, "vst_ms");
    auto transformed = dds::vst(
        counts, result.normalized, design_matrix, options.vst_use_design,
        options.vst_kind, options.min_mu, options.min_disp,
        result.effective_max_disp, options.beta_tol, options.threads,
        options.deterministic);
    result.vst_counts = std::move(transformed.first);
    result.vst_fit = std::move(transformed.second);
  }

  {
    ScopedProfileTimer timer(profile, "dispersion_gene_wise_ms");
    result.genewise = dds::fit_genewise_dispersions(
        counts, result.normalized, design_matrix, options.min_mu,
        options.min_disp, result.effective_max_disp, options.threads,
        options.deterministic, options.beta_tol, options.compat_mode);
  }

  if (!compute_trend) {
    return result;
  }

  {
    ScopedProfileTimer timer(profile, "dispersion_trend_ms");
    result.trend = dds::fit_dispersion_trend(
        result.genewise.genewise, result.genewise.non_zero,
        result.normalized.base_means, options.min_disp, options.trend_kind,
        options.compat_mode);
  }

  if (!compute_map) {
    return result;
  }

  {
    ScopedProfileTimer timer(profile, "dispersion_prior_ms");
    result.prior = dds::fit_dispersion_prior(
        result.genewise.genewise, result.trend->fitted,
        result.genewise.non_zero, counts.sample_count(),
        design_matrix.column_count(), options.min_disp);
  }
  {
    ScopedProfileTimer timer(profile, "dispersion_map_ms");
    result.map = dds::fit_MAP_dispersions(
        counts, design_matrix, result.genewise.mu_hat, result.genewise.genewise,
        result.trend->fitted, result.genewise.non_zero, options.min_disp,
        result.effective_max_disp, result.prior->prior_disp_var,
        result.prior->squared_logres, options.threads, options.deterministic,
        options.compat_mode);
  }

  if (!compute_lfc) {
    return result;
  }

  {
    ScopedProfileTimer timer(profile, "glm_fit_ms");
    result.lfc = dds::fit_LFC(
        counts, result.normalized, design_matrix, result.map->dispersions,
        result.genewise.non_zero, options.min_mu, options.beta_tol,
        options.threads, options.deterministic, options.compat_mode);
  }

  if (compute_cooks) {
    ScopedProfileTimer timer(profile, "cooks_ms");
    result.cooks = dds::calculate_cooks_outliers(
        counts, result.normalized, design_matrix, *result.lfc,
        result.genewise.non_zero, options.threads, options.deterministic);
  }

  if (compute_replacement && result.cooks.has_value()) {
    ScopedProfileTimer timer(profile, "cook_refit_ms");
    result.replacement = dds::replace_outliers(
        counts, result.normalized, design_matrix, result.cooks->cooks,
        options.min_replicates, options.threads, options.deterministic);

    if (options.refit_cooks) {
      const std::vector<std::size_t> refit_genes =
          mask_indices(result.replacement->refitted);
      if (!refit_genes.empty()) {
        const CountMatrix sub_counts =
            subset_genes(result.replacement->counts, refit_genes);
        const NormalizedCounts sub_normalized =
            normalize_counts_with_size_factors(
                sub_counts, result.normalized.sample_wise_size_factors());
        const GeneWiseDispersions sub_genewise =
            dds::fit_genewise_dispersions(
                sub_counts, sub_normalized, design_matrix, options.min_mu,
                options.min_disp, result.effective_max_disp, options.threads,
                options.deterministic, options.beta_tol, options.compat_mode);
        const std::vector<double> sub_fitted = dds::fitted_dispersions_from_trend(
            *result.trend, sub_normalized.base_means, sub_genewise.non_zero);
        const MAPDispersions sub_map = dds::fit_MAP_dispersions(
            sub_counts, design_matrix, sub_genewise.mu_hat,
            sub_genewise.genewise, sub_fitted, sub_genewise.non_zero,
            options.min_disp, result.effective_max_disp,
            result.prior->prior_disp_var, result.prior->squared_logres,
            options.threads, options.deterministic, options.compat_mode);
        const LFCFit sub_lfc = dds::fit_LFC(
            sub_counts, sub_normalized, design_matrix, sub_map.dispersions,
            sub_genewise.non_zero, options.min_mu, options.beta_tol,
            options.threads, options.deterministic, options.compat_mode);
        copy_refit_results(refit_genes, sub_normalized, sub_genewise,
                           sub_fitted, sub_map, sub_lfc, result.normalized,
                           result.genewise, *result.trend, *result.map,
                           *result.lfc);
      }

      apply_new_all_zeroes(result.replacement->new_all_zeroes, design_matrix,
                           result.normalized, *result.lfc);
      if (any_mask(result.replacement->refitted)) {
        // After refit, PyDESeq2 evaluates Cook p-value filtering against the
        // replace_cooks layer where replaceable samples were zeroed.
        result.cooks->pvalue_cooks_outlier = dds::cooks_outlier(
            counts, design_matrix, result.cooks->cooks,
            &result.replacement->replace_cooks, options.threads,
            options.deterministic);
      }
      // DESeq2 applies the new-all-zero override after Cook filtering, so
      // these genes end with pvalue=1 instead of Cook-filtered NA.
      clear_new_all_zeroes_from_cooks(result.replacement->new_all_zeroes,
                                      result.cooks->pvalue_cooks_outlier);
    }
  }

  if (compute_wald) {
    ScopedProfileTimer timer(profile, "wald_test_ms");
    WaldTestOptions wald_options = options.wald_options;
    if (options.compat_mode == CompatMode::deseq2_r) {
      wald_options.ridge_factor = kDefaultRidgeFactor / (kLog2 * kLog2);
      wald_options.min_mu = options.min_mu;
    }
    wald_options.compat_mode = options.compat_mode;
    wald_options.requested_threads = options.threads;
    wald_options.deterministic = options.deterministic;
    if (options.cooks_filter && result.cooks.has_value()) {
      wald_options.cooks_outlier = &result.cooks->pvalue_cooks_outlier;
    }
    if (options.refit_cooks && result.replacement.has_value()) {
      wald_options.new_all_zeroes = &result.replacement->new_all_zeroes;
    }
    result.summary = ds::summary(
        design_matrix, result.normalized, *result.lfc, result.map->dispersions,
        result.genewise.non_zero, contrast, wald_options);
  }

  if (options.compute_lfc_shrink) {
    ScopedProfileTimer timer(profile, "lfc_shrink_ms");
    result.lfc_shrink = ds::lfc_shrink(
        counts, design_matrix, result.normalized, *result.lfc, *result.summary,
        result.map->dispersions, result.genewise.non_zero,
        options.lfc_shrink_coeff_index, options.lfc_shrink_adapt,
        options.threads, options.deterministic, options.compat_mode);
  }

  return result;
}

}  // namespace ccdeseq2
