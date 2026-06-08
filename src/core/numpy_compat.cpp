#include "ccdeseq2/numpy_compat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "ccdeseq2/errors.hpp"

namespace ccdeseq2 {

double median(std::vector<double> values) {
  if (values.empty()) {
    throw Error(ExitCode::numeric_error, "Cannot compute median of an empty vector.");
  }
  const std::size_t mid = values.size() / 2;
  std::nth_element(values.begin(), values.begin() + mid, values.end());
  if (values.size() % 2 == 1) {
    return values[mid];
  }
  const double upper = values[mid];
  std::nth_element(values.begin(), values.begin() + mid - 1, values.end());
  return 0.5 * (values[mid - 1] + upper);
}

double trim_mean(std::vector<double> values, double proportion,
                 EmptyInputPolicy empty_policy) {
  auto empty_result = [&]() {
    if (empty_policy == EmptyInputPolicy::return_nan) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    throw Error(ExitCode::numeric_error,
                "Cannot compute trimmed mean of an empty vector.");
  };

  if (values.empty()) {
    return empty_result();
  }
  std::sort(values.begin(), values.end());
  const std::size_t ntrim =
      static_cast<std::size_t>(std::floor(proportion * values.size()));
  if (2 * ntrim >= values.size()) {
    return empty_result();
  }
  double sum = 0.0;
  for (std::size_t i = ntrim; i < values.size() - ntrim; ++i) {
    sum += values[i];
  }
  return sum / static_cast<double>(values.size() - 2 * ntrim);
}

double quantile_from_sorted(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (q <= 0.0) {
    return sorted.front();
  }
  if (q >= 1.0) {
    return sorted.back();
  }
  const double pos = q * static_cast<double>(sorted.size() - 1);
  const auto lower = static_cast<std::size_t>(std::floor(pos));
  const auto upper = static_cast<std::size_t>(std::ceil(pos));
  const double frac = pos - static_cast<double>(lower);
  return sorted[lower] * (1.0 - frac) + sorted[upper] * frac;
}

double quantile_linear(std::vector<double> values, double q) {
  std::sort(values.begin(), values.end());
  return quantile_from_sorted(values, q);
}

std::vector<double> linspace(double min, double max, std::size_t length) {
  std::vector<double> values(length, min);
  if (length <= 1) {
    return values;
  }
  const double step = (max - min) / static_cast<double>(length - 1);
  for (std::size_t i = 0; i < length; ++i) {
    values[i] = min + step * static_cast<double>(i);
  }
  return values;
}

std::vector<double> linspace(double min, double max, int length) {
  if (length <= 0) {
    return {};
  }
  return linspace(min, max, static_cast<std::size_t>(length));
}

double nan_to_num(double value) {
  if (std::isnan(value)) {
    return 0.0;
  }
  if (std::isinf(value)) {
    return std::copysign(std::numeric_limits<double>::max(), value);
  }
  return value;
}

}  // namespace ccdeseq2
