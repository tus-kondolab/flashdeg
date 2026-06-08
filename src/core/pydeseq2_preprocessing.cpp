#include "ccdeseq2/pydeseq2_preprocessing.hpp"

namespace ccdeseq2::pydeseq2::preprocessing {

NormalizedCounts deseq2_norm(const CountMatrix& counts,
                             SizeFactorFitType fit_type) {
  return ccdeseq2::fit_size_factors(counts, fit_type);
}

std::vector<double> deseq2_norm_fit(const CountMatrix& counts,
                                    SizeFactorFitType fit_type) {
  return ccdeseq2::fit_size_factors(counts, fit_type)
      .sample_wise_size_factors();
}

NormalizedCounts deseq2_norm_transform(
    const CountMatrix& counts, const std::vector<double>& size_factors) {
  return ccdeseq2::normalize_counts_with_size_factors(counts, size_factors);
}

}  // namespace ccdeseq2::pydeseq2::preprocessing
