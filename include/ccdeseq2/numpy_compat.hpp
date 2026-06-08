#pragma once

#include <cstddef>
#include <vector>

namespace ccdeseq2 {

enum class EmptyInputPolicy {
  throw_error,
  return_nan,
};

[[nodiscard]] double median(std::vector<double> values);

[[nodiscard]] double trim_mean(
    std::vector<double> values, double proportion,
    EmptyInputPolicy empty_policy = EmptyInputPolicy::throw_error);

[[nodiscard]] double quantile_from_sorted(const std::vector<double>& sorted,
                                          double q);

[[nodiscard]] double quantile_linear(std::vector<double> values, double q);

[[nodiscard]] std::vector<double> linspace(double min, double max,
                                           std::size_t length);

[[nodiscard]] std::vector<double> linspace(double min, double max, int length);

[[nodiscard]] double nan_to_num(double value);

}  // namespace ccdeseq2
