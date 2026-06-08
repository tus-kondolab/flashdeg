#include "ccdeseq2/normalization.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <sstream>
#include <utility>

#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/numpy_compat.hpp"

namespace ccdeseq2 {
namespace {

[[nodiscard]] std::vector<double> logmeans_ratio(const CountMatrix& counts,
                                                 std::vector<std::uint8_t>& filtered) {
  std::vector<double> logmeans(counts.gene_count(), 0.0);
  filtered.assign(counts.gene_count(), 1);
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    double sum = 0.0;
    for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
      const double value = counts(sample, gene);
      if (value <= 0.0) {
        filtered[gene] = 0;
        logmeans[gene] = -std::numeric_limits<double>::infinity();
        break;
      }
      sum += std::log(value);
    }
    if (filtered[gene]) {
      logmeans[gene] = sum / static_cast<double>(counts.sample_count());
    }
  }
  return logmeans;
}

[[nodiscard]] std::vector<double> logmeans_poscounts(const CountMatrix& counts,
                                                     std::vector<std::uint8_t>& filtered) {
  std::vector<double> logmeans(counts.gene_count(), 0.0);
  filtered.assign(counts.gene_count(), 0);
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    double sum = 0.0;
    for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
      const double value = counts(sample, gene);
      if (value > 0.0) {
        sum += std::log(value);
      }
    }
    logmeans[gene] = sum / static_cast<double>(counts.sample_count());
    filtered[gene] = static_cast<std::uint8_t>(logmeans[gene] > 0.0 &&
                                               std::isfinite(logmeans[gene]));
  }
  return logmeans;
}

[[nodiscard]] std::vector<double> fit_ratio(const CountMatrix& counts) {
  std::vector<std::uint8_t> filtered;
  const std::vector<double> logmeans = logmeans_ratio(counts, filtered);
  if (std::none_of(filtered.begin(), filtered.end(), [](bool value) { return value; })) {
    throw Error(ExitCode::unsupported,
                "Every gene contains at least one zero. Iterative size factors are not "
                "implemented yet.");
  }

  std::vector<double> size_factors(counts.sample_count(), 1.0);
  for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
    std::vector<double> ratios;
    ratios.reserve(counts.gene_count());
    for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
      if (filtered[gene]) {
        ratios.push_back(std::log(counts(sample, gene)) - logmeans[gene]);
      }
    }
    size_factors[sample] = std::exp(median(std::move(ratios)));
  }
  return size_factors;
}

[[nodiscard]] std::vector<double> fit_poscounts(const CountMatrix& counts) {
  std::vector<std::uint8_t> filtered;
  const std::vector<double> logmeans = logmeans_poscounts(counts, filtered);
  if (std::none_of(filtered.begin(), filtered.end(), [](bool value) { return value; })) {
    throw Error(ExitCode::numeric_error, "No genes available for poscounts fitting.");
  }

  std::vector<double> size_factors(counts.sample_count(), 1.0);
  for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
    std::vector<double> ratios;
    ratios.reserve(counts.gene_count());
    for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
      if (filtered[gene] && counts(sample, gene) > 0.0) {
        ratios.push_back(std::log(counts(sample, gene)) - logmeans[gene]);
      }
    }
    if (ratios.empty()) {
      throw Error(ExitCode::numeric_error,
                  "A sample has no positive counts in poscounts fitting.");
    }
    size_factors[sample] = std::exp(median(std::move(ratios)));
  }

  double log_sum = 0.0;
  for (double value : size_factors) {
    if (value <= 0.0 || !std::isfinite(value)) {
      throw Error(ExitCode::numeric_error, "Invalid size factor produced.");
    }
    log_sum += std::log(value);
  }
  const double geometric_mean =
      std::exp(log_sum / static_cast<double>(size_factors.size()));
  for (double& value : size_factors) {
    value /= geometric_mean;
  }
  return size_factors;
}

}  // namespace

NormalizationFactors::NormalizationFactors(Kind kind, std::size_t samples,
                                           std::size_t genes,
                                           std::vector<double> values)
    : kind_(kind),
      samples_(samples),
      genes_(genes),
      values_(std::move(values)) {}

NormalizationFactors NormalizationFactors::sample_wise(
    std::vector<double> values) {
  return NormalizationFactors(Kind::sample_wise, values.size(), 0, std::move(values));
}

NormalizationFactors NormalizationFactors::matrix(
    std::size_t samples, std::size_t genes,
    std::vector<double> values_gene_major) {
  if (values_gene_major.size() != samples * genes) {
    throw Error(ExitCode::input_error,
                "Normalization factor matrix has inconsistent dimensions.");
  }
  return NormalizationFactors(Kind::matrix, samples, genes,
                              std::move(values_gene_major));
}

double NormalizationFactors::value(std::size_t sample_index,
                                   std::size_t gene_index) const {
  if (kind_ == Kind::sample_wise) {
    return values_.at(sample_index);
  }
  return values_.at(gene_index * samples_ + sample_index);
}

NormalizedCounts fit_size_factors(const CountMatrix& counts,
                                  SizeFactorFitType fit_type) {
  std::vector<double> size_factors =
      fit_type == SizeFactorFitType::ratio ? fit_ratio(counts) : fit_poscounts(counts);
  return normalize_counts_with_size_factors(counts, size_factors);
}

NormalizedCounts normalize_counts_with_size_factors(
    const CountMatrix& counts, const std::vector<double>& size_factors) {
  if (size_factors.size() != counts.sample_count()) {
    throw Error(ExitCode::input_error,
                "Size factor count does not match the number of samples.");
  }
  for (double value : size_factors) {
    if (value <= 0.0 || !std::isfinite(value)) {
      throw Error(ExitCode::numeric_error,
                  "Size factors must be positive finite values.");
    }
  }

  CountMatrix normalized(counts.sample_names(), counts.gene_names());
  std::vector<double> base_means(counts.gene_count(), 0.0);
  for (std::size_t gene = 0; gene < counts.gene_count(); ++gene) {
    double sum = 0.0;
    for (std::size_t sample = 0; sample < counts.sample_count(); ++sample) {
      const double value = counts(sample, gene) / size_factors[sample];
      normalized(sample, gene) = value;
      sum += value;
    }
    base_means[gene] = sum / static_cast<double>(counts.sample_count());
  }

  return NormalizedCounts{NormalizationFactors::sample_wise(size_factors),
                          std::move(normalized),
                          std::move(base_means)};
}

std::vector<double> count_matrix_to_row_major(const CountMatrix& matrix) {
  std::vector<double> values(matrix.sample_count() * matrix.gene_count(), 0.0);
  for (std::size_t sample = 0; sample < matrix.sample_count(); ++sample) {
    for (std::size_t gene = 0; gene < matrix.gene_count(); ++gene) {
      values[sample * matrix.gene_count() + gene] = matrix(sample, gene);
    }
  }
  return values;
}

}  // namespace ccdeseq2
