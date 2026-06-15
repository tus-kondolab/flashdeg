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
// Upper-tail (survival) probability of a chi-square distribution: P(X > x) for
// X ~ chi-square(degrees_of_freedom). Returns 1 for x <= 0 and NaN for invalid
// arguments (non-finite x/df or df <= 0). Used by the likelihood-ratio test.
[[nodiscard]] double chi_square_sf(double x, double degrees_of_freedom);

}  // namespace ccdeseq2
