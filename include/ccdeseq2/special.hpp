#pragma once

namespace ccdeseq2 {

[[nodiscard]] double gammaln(double x);
[[nodiscard]] double digamma(double x);
[[nodiscard]] double trigamma(double x);
[[nodiscard]] double normal_sf(double x);
[[nodiscard]] double normal_ppf_75();
[[nodiscard]] double f_distribution_cdf(double x, double numerator_df,
                                        double denominator_df);
[[nodiscard]] double f_distribution_quantile(double probability, double numerator_df,
                                             double denominator_df);

}  // namespace ccdeseq2
