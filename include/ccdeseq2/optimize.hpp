#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace ccdeseq2 {

struct Bound {
  double lower = -INFINITY;
  double upper = INFINITY;
};

struct LbfgsbOptions {
  int max_iterations = 15000;
  int max_line_search_steps = 50;
  int history_size = 10;
  double ftol = 2.220446049250313e-9;
  double gtol = 1e-5;
  double finite_difference_rel_step = 1e-8;
};

struct LbfgsbResult {
  std::vector<double> x;
  std::vector<double> gradient;
  double value = INFINITY;
  int iterations = 0;
  bool converged = false;
  std::string message;
};

using ObjectiveFunction = std::function<double(std::span<const double>)>;
using GradientFunction =
    std::function<void(std::span<const double>, std::vector<double>&)>;

struct GoldenSectionOptions {
  int max_iterations = 50;
  double relative_tolerance = 1e-12;
};

struct ScalarMinimizeResult {
  double argmin = 0.0;
  double minimum = 0.0;
  int iterations = 0;
  bool converged = false;
};

template <typename Loss>
[[nodiscard]] ScalarMinimizeResult golden_section_minimize(
    Loss&& loss, double lower, double upper,
    GoldenSectionOptions options = {}) {
  constexpr double kGolden = 0.6180339887498948482;
  double lo = lower;
  double hi = upper;
  double x1 = hi - kGolden * (hi - lo);
  double x2 = lo + kGolden * (hi - lo);
  double f1 = loss(x1);
  double f2 = loss(x2);

  ScalarMinimizeResult result;
  for (int iter = 0; iter < options.max_iterations; ++iter) {
    result.iterations = iter + 1;
    const double scale = std::max(std::max(std::abs(lo), std::abs(hi)), 1.0);
    if (hi - lo <= options.relative_tolerance * scale) {
      result.converged = true;
      break;
    }
    if (f1 > f2) {
      lo = x1;
      x1 = x2;
      f1 = f2;
      x2 = lo + kGolden * (hi - lo);
      f2 = loss(x2);
    } else {
      hi = x2;
      x2 = x1;
      f2 = f1;
      x1 = hi - kGolden * (hi - lo);
      f1 = loss(x1);
    }
  }

  if (f1 <= f2) {
    result.argmin = x1;
    result.minimum = f1;
  } else {
    result.argmin = x2;
    result.minimum = f2;
  }
  if (!result.converged) {
    const double scale = std::max(std::max(std::abs(lo), std::abs(hi)), 1.0);
    result.converged = hi - lo <= options.relative_tolerance * scale;
  }
  return result;
}

[[nodiscard]] LbfgsbResult minimize_l_bfgs_b(
    const ObjectiveFunction& objective, std::vector<double> initial,
    const std::vector<Bound>& bounds, const LbfgsbOptions& options = {},
    const GradientFunction& gradient = {});

[[nodiscard]] LbfgsbResult minimize_l_bfgs_b_scipy(
    const ObjectiveFunction& objective, std::vector<double> initial,
    const std::vector<Bound>& bounds, const LbfgsbOptions& options = {},
    const GradientFunction& gradient = {});

[[nodiscard]] const char* optimizer_backend_name();

}  // namespace ccdeseq2
