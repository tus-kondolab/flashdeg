#include "ccdeseq2/special.hpp"

#include <cmath>
#include <limits>

#ifdef FLASHDEG_HAVE_BOOST_MATH
#include <boost/math/distributions/fisher_f.hpp>
#include <boost/math/distributions/normal.hpp>
#include <boost/math/special_functions/digamma.hpp>
#include <boost/math/special_functions/gamma.hpp>
#include <boost/math/special_functions/polygamma.hpp>
#endif

namespace ccdeseq2 {
namespace {

[[nodiscard]] double regularized_beta_cf(double a, double b, double x) {
  constexpr int kMaxIterations = 200;
  constexpr double kEps = 3e-14;
  constexpr double kFpMin = 1e-300;
  const double qab = a + b;
  const double qap = a + 1.0;
  const double qam = a - 1.0;
  double c = 1.0;
  double d = 1.0 - qab * x / qap;
  if (std::abs(d) < kFpMin) {
    d = kFpMin;
  }
  d = 1.0 / d;
  double h = d;
  for (int m = 1; m <= kMaxIterations; ++m) {
    const int m2 = 2 * m;
    double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
    d = 1.0 + aa * d;
    if (std::abs(d) < kFpMin) {
      d = kFpMin;
    }
    c = 1.0 + aa / c;
    if (std::abs(c) < kFpMin) {
      c = kFpMin;
    }
    d = 1.0 / d;
    h *= d * c;

    aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
    d = 1.0 + aa * d;
    if (std::abs(d) < kFpMin) {
      d = kFpMin;
    }
    c = 1.0 + aa / c;
    if (std::abs(c) < kFpMin) {
      c = kFpMin;
    }
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::abs(del - 1.0) < kEps) {
      break;
    }
  }
  return h;
}

[[nodiscard]] double regularized_beta(double a, double b, double x) {
  if (x <= 0.0) {
    return 0.0;
  }
  if (x >= 1.0) {
    return 1.0;
  }
  const double bt = std::exp(std::lgamma(a + b) - std::lgamma(a) -
                             std::lgamma(b) + a * std::log(x) +
                             b * std::log1p(-x));
  if (x < (a + 1.0) / (a + b + 2.0)) {
    return bt * regularized_beta_cf(a, b, x) / a;
  }
  return 1.0 - bt * regularized_beta_cf(b, a, 1.0 - x) / b;
}

[[nodiscard]] double regularized_gamma_p_series(double a, double x) {
  // Lower regularized incomplete gamma P(a, x) via series expansion
  // (Numerical Recipes, gser). Valid for x < a + 1.
  constexpr int kMaxIterations = 300;
  constexpr double kEps = 3e-14;
  if (x <= 0.0) {
    return 0.0;
  }
  double ap = a;
  double sum = 1.0 / a;
  double del = sum;
  for (int n = 0; n < kMaxIterations; ++n) {
    ap += 1.0;
    del *= x / ap;
    sum += del;
    if (std::abs(del) < std::abs(sum) * kEps) {
      break;
    }
  }
  return sum * std::exp(-x + a * std::log(x) - std::lgamma(a));
}

[[nodiscard]] double regularized_gamma_q_cf(double a, double x) {
  // Upper regularized incomplete gamma Q(a, x) via continued fraction
  // (Numerical Recipes, gcf). Valid for x >= a + 1.
  constexpr int kMaxIterations = 300;
  constexpr double kEps = 3e-14;
  constexpr double kFpMin = 1e-300;
  double b = x + 1.0 - a;
  double c = 1.0 / kFpMin;
  double d = 1.0 / b;
  double h = d;
  for (int i = 1; i <= kMaxIterations; ++i) {
    const double an = -static_cast<double>(i) * (static_cast<double>(i) - a);
    b += 2.0;
    d = an * d + b;
    if (std::abs(d) < kFpMin) {
      d = kFpMin;
    }
    c = b + an / c;
    if (std::abs(c) < kFpMin) {
      c = kFpMin;
    }
    d = 1.0 / d;
    const double del = d * c;
    h *= del;
    if (std::abs(del - 1.0) < kEps) {
      break;
    }
  }
  return std::exp(-x + a * std::log(x) - std::lgamma(a)) * h;
}

// Dependency-free upper regularized incomplete gamma Q(a, x), used as the
// chi-square survival fallback when Boost.Math is unavailable.
[[nodiscard]] double regularized_gamma_q(double a, double x) {
  if (!std::isfinite(a) || !std::isfinite(x) || a <= 0.0 || x < 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (x == 0.0) {
    return 1.0;
  }
  if (x < a + 1.0) {
    return 1.0 - regularized_gamma_p_series(a, x);
  }
  return regularized_gamma_q_cf(a, x);
}

}  // namespace

double gammaln(double x) {
#ifdef FLASHDEG_HAVE_BOOST_MATH
  return boost::math::lgamma(x);
#else
  return std::lgamma(x);
#endif
}

double digamma(double x) {
  if (x <= 0.0 || !std::isfinite(x)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
#ifdef FLASHDEG_HAVE_BOOST_MATH
  return boost::math::digamma(x);
#else
  double result = 0.0;
  while (x < 8.0) {
    result -= 1.0 / x;
    x += 1.0;
  }
  const double inv = 1.0 / x;
  const double inv2 = inv * inv;
  const double inv4 = inv2 * inv2;
  result += std::log(x) - 0.5 * inv - inv2 / 12.0 + inv4 / 120.0 -
            inv4 * inv2 / 252.0 + inv4 * inv4 / 240.0 -
            5.0 * inv4 * inv4 * inv2 / 660.0;
  return result;
#endif
}

double trigamma(double x) {
  if (x <= 0.0 || !std::isfinite(x)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
#ifdef FLASHDEG_HAVE_BOOST_MATH
  return boost::math::polygamma(1, x);
#else
  double result = 0.0;
  while (x < 8.0) {
    result += 1.0 / (x * x);
    x += 1.0;
  }
  const double inv = 1.0 / x;
  const double inv2 = inv * inv;
  result += inv + 0.5 * inv2 + (inv2 * inv) / 6.0 -
            (inv2 * inv2 * inv) / 30.0 +
            (inv2 * inv2 * inv2 * inv) / 42.0 -
            (inv2 * inv2 * inv2 * inv2 * inv) / 30.0;
  return result;
#endif
}

double normal_sf(double x) {
#ifdef FLASHDEG_HAVE_BOOST_MATH
  const boost::math::normal_distribution<double> normal;
  return boost::math::cdf(boost::math::complement(normal, x));
#else
  static constexpr double kInvSqrt2 = 0.70710678118654752440;
  return 0.5 * std::erfc(x * kInvSqrt2);
#endif
}

double normal_ppf_75() {
#ifdef FLASHDEG_HAVE_BOOST_MATH
  const boost::math::normal_distribution<double> normal;
  return boost::math::quantile(normal, 0.75);
#else
  return 0.6744897501960817;
#endif
}

double f_distribution_cdf(double x, double numerator_df, double denominator_df) {
  if (!std::isfinite(x) || !std::isfinite(numerator_df) ||
      !std::isfinite(denominator_df) || numerator_df <= 0.0 ||
      denominator_df <= 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (x <= 0.0) {
    return 0.0;
  }
#ifdef FLASHDEG_HAVE_BOOST_MATH
  const boost::math::fisher_f_distribution<double> distribution(numerator_df,
                                                                 denominator_df);
  return boost::math::cdf(distribution, x);
#else
  const double z =
      (numerator_df * x) / (numerator_df * x + denominator_df);
  return regularized_beta(0.5 * numerator_df, 0.5 * denominator_df, z);
#endif
}

double f_distribution_quantile(double probability, double numerator_df,
                               double denominator_df) {
  if (!std::isfinite(probability) || !std::isfinite(numerator_df) ||
      !std::isfinite(denominator_df) || numerator_df <= 0.0 ||
      denominator_df <= 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (probability <= 0.0) {
    return 0.0;
  }
  if (probability >= 1.0) {
    return std::numeric_limits<double>::infinity();
  }

#ifdef FLASHDEG_HAVE_BOOST_MATH
  const boost::math::fisher_f_distribution<double> distribution(numerator_df,
                                                                 denominator_df);
  return boost::math::quantile(distribution, probability);
#else
  double lo = 0.0;
  double hi = 1.0;
  while (f_distribution_cdf(hi, numerator_df, denominator_df) < probability &&
         hi < 1e12) {
    hi *= 2.0;
  }
  for (int i = 0; i < 100; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (f_distribution_cdf(mid, numerator_df, denominator_df) < probability) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
#endif
}

double chi_square_sf(double x, double degrees_of_freedom) {
  if (!std::isfinite(x) || !std::isfinite(degrees_of_freedom) ||
      degrees_of_freedom <= 0.0) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (x <= 0.0) {
    return 1.0;
  }
  // chi-square(k) survival = Q(k/2, x/2), the upper regularized incomplete gamma.
#ifdef FLASHDEG_HAVE_BOOST_MATH
  return boost::math::gamma_q(0.5 * degrees_of_freedom, 0.5 * x);
#else
  return regularized_gamma_q(0.5 * degrees_of_freedom, 0.5 * x);
#endif
}

}  // namespace ccdeseq2
