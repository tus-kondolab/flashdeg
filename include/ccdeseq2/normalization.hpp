#pragma once

#include <cstddef>
#include <vector>

#include "ccdeseq2/table.hpp"

namespace ccdeseq2 {

enum class SizeFactorFitType {
  ratio,
  poscounts,
};

class NormalizationFactors {
 public:
  enum class Kind {
    sample_wise,
    matrix,
  };

  NormalizationFactors() = default;

  static NormalizationFactors sample_wise(std::vector<double> values);
  static NormalizationFactors matrix(std::size_t samples, std::size_t genes,
                                     std::vector<double> values_gene_major);

  [[nodiscard]] Kind kind() const noexcept { return kind_; }
  [[nodiscard]] std::size_t sample_count() const noexcept { return samples_; }
  [[nodiscard]] std::size_t gene_count() const noexcept { return genes_; }
  [[nodiscard]] const std::vector<double>& values() const noexcept {
    return values_;
  }
  [[nodiscard]] double value(std::size_t sample_index,
                             std::size_t gene_index) const;

 private:
  NormalizationFactors(Kind kind, std::size_t samples, std::size_t genes,
                       std::vector<double> values);

  Kind kind_ = Kind::sample_wise;
  std::size_t samples_ = 0;
  std::size_t genes_ = 0;
  std::vector<double> values_;
};

struct NormalizedCounts {
  NormalizationFactors normalization_factors;
  CountMatrix normalized_counts;
  std::vector<double> base_means;

  [[nodiscard]] const std::vector<double>& sample_wise_size_factors() const {
    return normalization_factors.values();
  }
};

[[nodiscard]] NormalizedCounts fit_size_factors(const CountMatrix& counts,
                                                SizeFactorFitType fit_type);

[[nodiscard]] NormalizedCounts normalize_counts_with_size_factors(
    const CountMatrix& counts, const std::vector<double>& size_factors);

[[nodiscard]] std::vector<double> count_matrix_to_row_major(
    const CountMatrix& matrix);

}  // namespace ccdeseq2
