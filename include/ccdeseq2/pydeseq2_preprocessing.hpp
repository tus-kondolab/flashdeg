#pragma once

#include <vector>

#include "ccdeseq2/normalization.hpp"
#include "ccdeseq2/table.hpp"

namespace ccdeseq2::pydeseq2::preprocessing {

[[nodiscard]] NormalizedCounts deseq2_norm(const CountMatrix& counts,
                                           SizeFactorFitType fit_type);

[[nodiscard]] std::vector<double> deseq2_norm_fit(
    const CountMatrix& counts, SizeFactorFitType fit_type);

[[nodiscard]] NormalizedCounts deseq2_norm_transform(
    const CountMatrix& counts, const std::vector<double>& size_factors);

}  // namespace ccdeseq2::pydeseq2::preprocessing
