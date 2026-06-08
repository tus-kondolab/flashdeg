#include "ccdeseq2/optimize.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

#ifdef FLASHDEG_HAVE_SCIPY_LBFGSB
extern "C" {
void setulb(int n, int m, double* x, double* l, double* u, int* nbd,
            double* f, double* g, double factr, double pgtol, double* wa,
            int* iwa, int* task, int* lsave, int* isave, double* dsave,
            int maxls, int* ln_task);
}
#endif

namespace ccdeseq2 {
namespace {

[[nodiscard]] double dot(const std::vector<double>& a,
                         const std::vector<double>& b) {
  double value = 0.0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    value += a[i] * b[i];
  }
  return value;
}

[[nodiscard]] double max_abs(const std::vector<double>& values) {
  double value = 0.0;
  for (const double x : values) {
    value = std::max(value, std::abs(x));
  }
  return value;
}

[[nodiscard]] double project_value(double value, const Bound& bound) {
  return std::clamp(value, bound.lower, bound.upper);
}

void project_in_place(std::vector<double>& x, const std::vector<Bound>& bounds) {
  for (std::size_t i = 0; i < x.size(); ++i) {
    x[i] = project_value(x[i], bounds[i]);
  }
}

[[nodiscard]] std::vector<double> projected_gradient(
    const std::vector<double>& x, const std::vector<double>& gradient,
    const std::vector<Bound>& bounds) {
  constexpr double kActiveTolerance = 1e-12;
  std::vector<double> projected = gradient;
  for (std::size_t i = 0; i < projected.size(); ++i) {
    const bool at_lower = x[i] <= bounds[i].lower + kActiveTolerance;
    const bool at_upper = x[i] >= bounds[i].upper - kActiveTolerance;
    if ((at_lower && gradient[i] > 0.0) || (at_upper && gradient[i] < 0.0)) {
      projected[i] = 0.0;
    }
  }
  return projected;
}

void finite_difference_gradient(const ObjectiveFunction& objective,
                                const std::vector<Bound>& bounds,
                                double rel_step,
                                const std::vector<double>& x,
                                std::vector<double>& gradient) {
  gradient.assign(x.size(), 0.0);
  std::vector<double> trial = x;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const double h = rel_step * std::max(1.0, std::abs(x[i]));
    const double plus = project_value(x[i] + h, bounds[i]);
    const double minus = project_value(x[i] - h, bounds[i]);
    if (plus == minus) {
      gradient[i] = 0.0;
      continue;
    }
    if (plus != x[i] && minus != x[i]) {
      trial[i] = plus;
      const double f_plus = objective(trial);
      trial[i] = minus;
      const double f_minus = objective(trial);
      gradient[i] = (f_plus - f_minus) / (plus - minus);
    } else if (plus != x[i]) {
      trial[i] = plus;
      const double f_plus = objective(trial);
      trial[i] = x[i];
      const double f = objective(trial);
      gradient[i] = (f_plus - f) / (plus - x[i]);
    } else {
      trial[i] = minus;
      const double f_minus = objective(trial);
      trial[i] = x[i];
      const double f = objective(trial);
      gradient[i] = (f - f_minus) / (x[i] - minus);
    }
    trial[i] = x[i];
  }
}

void evaluate_gradient(const ObjectiveFunction& objective,
                       const GradientFunction& gradient_function,
                       const std::vector<Bound>& bounds,
                       const LbfgsbOptions& options,
                       const std::vector<double>& x,
                       std::vector<double>& gradient) {
  if (gradient_function) {
    gradient_function(x, gradient);
  } else {
    finite_difference_gradient(objective, bounds,
                               options.finite_difference_rel_step, x,
                               gradient);
  }
}

[[nodiscard]] std::vector<double> lbfgs_direction(
    const std::vector<double>& gradient,
    const std::vector<std::vector<double>>& s_history,
    const std::vector<std::vector<double>>& y_history) {
  std::vector<double> q = gradient;
  std::vector<double> alpha(s_history.size(), 0.0);
  std::vector<double> rho(s_history.size(), 0.0);
  for (std::size_t i = s_history.size(); i-- > 0;) {
    const double ys = dot(y_history[i], s_history[i]);
    if (ys <= 0.0) {
      continue;
    }
    rho[i] = 1.0 / ys;
    alpha[i] = rho[i] * dot(s_history[i], q);
    for (std::size_t j = 0; j < q.size(); ++j) {
      q[j] -= alpha[i] * y_history[i][j];
    }
  }

  double gamma = 1.0;
  if (!s_history.empty()) {
    const auto& s = s_history.back();
    const auto& y = y_history.back();
    const double yy = dot(y, y);
    const double sy = dot(s, y);
    if (yy > 0.0 && sy > 0.0) {
      gamma = sy / yy;
    }
  }

  std::vector<double> direction(q.size(), 0.0);
  for (std::size_t i = 0; i < q.size(); ++i) {
    direction[i] = gamma * q[i];
  }

  for (std::size_t i = 0; i < s_history.size(); ++i) {
    if (rho[i] <= 0.0) {
      continue;
    }
    const double beta = rho[i] * dot(y_history[i], direction);
    for (std::size_t j = 0; j < direction.size(); ++j) {
      direction[j] += s_history[i][j] * (alpha[i] - beta);
    }
  }

  for (double& value : direction) {
    value = -value;
  }
  return direction;
}

[[nodiscard]] bool relative_ftol_converged(double previous, double current,
                                           double ftol) {
  const double scale =
      std::max({1.0, std::abs(previous), std::abs(current)});
  return std::abs(previous - current) <= ftol * scale;
}

#ifdef FLASHDEG_HAVE_SCIPY_LBFGSB
constexpr int kScipyTaskStart = 0;
constexpr int kScipyTaskNewX = 1;
constexpr int kScipyTaskRestart = 2;
constexpr int kScipyTaskFG = 3;
constexpr int kScipyTaskConvergence = 4;

[[nodiscard]] int scipy_bound_code(const Bound& bound) {
  const bool has_lower = std::isfinite(bound.lower);
  const bool has_upper = std::isfinite(bound.upper);
  if (has_lower && has_upper) {
    return 2;
  }
  if (has_lower) {
    return 1;
  }
  if (has_upper) {
    return 3;
  }
  return 0;
}
#endif

}  // namespace

LbfgsbResult minimize_l_bfgs_b(const ObjectiveFunction& objective,
                               std::vector<double> initial,
                               const std::vector<Bound>& bounds,
                               const LbfgsbOptions& options,
                               const GradientFunction& gradient_function) {
  LbfgsbResult result;
  if (initial.empty() || initial.size() != bounds.size()) {
    result.message = "invalid dimensions";
    return result;
  }

  std::vector<double> x = std::move(initial);
  project_in_place(x, bounds);
  double value = objective(x);
  if (!std::isfinite(value)) {
    result.x = x;
    result.value = value;
    result.message = "initial objective is not finite";
    return result;
  }

  std::vector<double> gradient;
  evaluate_gradient(objective, gradient_function, bounds, options, x, gradient);
  if (gradient.size() != x.size() ||
      !std::all_of(gradient.begin(), gradient.end(),
                   [](double g) { return std::isfinite(g); })) {
    result.x = x;
    result.value = value;
    result.message = "initial gradient is not finite";
    return result;
  }

  std::vector<std::vector<double>> s_history;
  std::vector<std::vector<double>> y_history;
  for (int iter = 0; iter < options.max_iterations; ++iter) {
    result.iterations = iter + 1;
    std::vector<double> pg = projected_gradient(x, gradient, bounds);
    if (max_abs(pg) <= options.gtol) {
      result.converged = true;
      result.message = "projected gradient tolerance reached";
      break;
    }

    std::vector<double> direction = lbfgs_direction(pg, s_history, y_history);
    for (std::size_t i = 0; i < direction.size(); ++i) {
      if (pg[i] == 0.0) {
        direction[i] = 0.0;
      }
    }
    if (dot(direction, gradient) >= 0.0 || max_abs(direction) == 0.0) {
      for (std::size_t i = 0; i < direction.size(); ++i) {
        direction[i] = -pg[i];
      }
    }

    std::vector<double> candidate = x;
    std::vector<double> displacement(x.size(), 0.0);
    double candidate_value = std::numeric_limits<double>::infinity();
    bool accepted = false;
    double step = 1.0;
    constexpr double kArmijo = 1e-4;
    for (int line_step = 0; line_step < options.max_line_search_steps;
         ++line_step) {
      for (std::size_t i = 0; i < x.size(); ++i) {
        candidate[i] = project_value(x[i] + step * direction[i], bounds[i]);
        displacement[i] = candidate[i] - x[i];
      }
      if (max_abs(displacement) == 0.0) {
        step *= 0.5;
        continue;
      }
      const double directional_derivative = dot(gradient, displacement);
      if (directional_derivative >= 0.0) {
        step *= 0.5;
        continue;
      }
      candidate_value = objective(candidate);
      if (std::isfinite(candidate_value) &&
          candidate_value <= value + kArmijo * directional_derivative) {
        accepted = true;
        break;
      }
      step *= 0.5;
    }

    if (!accepted) {
      result.message = "line search failed";
      break;
    }

    std::vector<double> candidate_gradient;
    evaluate_gradient(objective, gradient_function, bounds, options, candidate,
                      candidate_gradient);
    if (candidate_gradient.size() != x.size() ||
        !std::all_of(candidate_gradient.begin(), candidate_gradient.end(),
                     [](double g) { return std::isfinite(g); })) {
      result.message = "gradient is not finite";
      break;
    }

    std::vector<double> s(x.size(), 0.0);
    std::vector<double> y(x.size(), 0.0);
    for (std::size_t i = 0; i < x.size(); ++i) {
      s[i] = candidate[i] - x[i];
      y[i] = candidate_gradient[i] - gradient[i];
    }
    const double ys = dot(y, s);
    if (ys > 1e-12 * std::max(1.0, max_abs(s)) * std::max(1.0, max_abs(y))) {
      if (static_cast<int>(s_history.size()) == options.history_size) {
        s_history.erase(s_history.begin());
        y_history.erase(y_history.begin());
      }
      s_history.push_back(s);
      y_history.push_back(y);
    }

    const double previous_value = value;
    x = std::move(candidate);
    value = candidate_value;
    gradient = std::move(candidate_gradient);
    if (relative_ftol_converged(previous_value, value, options.ftol)) {
      result.converged = true;
      result.message = "relative function tolerance reached";
      break;
    }
  }

  result.x = std::move(x);
  result.gradient = std::move(gradient);
  result.value = value;
  if (!result.converged && result.message.empty()) {
    result.message = "maximum iterations reached";
  }
  return result;
}

LbfgsbResult minimize_l_bfgs_b_scipy(
    const ObjectiveFunction& objective, std::vector<double> initial,
    const std::vector<Bound>& bounds, const LbfgsbOptions& options,
    const GradientFunction& gradient_function) {
#ifndef FLASHDEG_HAVE_SCIPY_LBFGSB
  return minimize_l_bfgs_b(objective, std::move(initial), bounds, options,
                           gradient_function);
#else
  LbfgsbResult result;
  if (initial.empty() || initial.size() != bounds.size() ||
      options.history_size <= 0) {
    result.message = "invalid dimensions";
    return result;
  }

  std::vector<double> x = std::move(initial);
  project_in_place(x, bounds);
  const int n = static_cast<int>(x.size());
  const int m = options.history_size;

  std::vector<double> lower(static_cast<std::size_t>(n), 0.0);
  std::vector<double> upper(static_cast<std::size_t>(n), 0.0);
  std::vector<int> nbd(static_cast<std::size_t>(n), 0);
  for (int i = 0; i < n; ++i) {
    lower[static_cast<std::size_t>(i)] = bounds[static_cast<std::size_t>(i)].lower;
    upper[static_cast<std::size_t>(i)] = bounds[static_cast<std::size_t>(i)].upper;
    nbd[static_cast<std::size_t>(i)] =
        scipy_bound_code(bounds[static_cast<std::size_t>(i)]);
  }

  const std::size_t workspace_size =
      static_cast<std::size_t>(2 * m * n + 5 * n + 11 * m * m + 8 * m);
  std::vector<double> wa(workspace_size, 0.0);
  std::vector<int> iwa(static_cast<std::size_t>(3 * n), 0);
  std::vector<double> gradient(static_cast<std::size_t>(n), 0.0);
  double value = 0.0;

  const double eps = std::numeric_limits<double>::epsilon();
  const double factr = options.ftol / eps;
  const double pgtol = options.gtol;
  int task[2] = {kScipyTaskStart, 0};
  int lsave[4] = {};
  int isave[44] = {};
  double dsave[29] = {};
  int ln_task[2] = {0, 0};

  for (int call = 0; call < options.max_iterations; ++call) {
    setulb(n, m, x.data(), lower.data(), upper.data(), nbd.data(), &value,
           gradient.data(), factr, pgtol, wa.data(), iwa.data(), task, lsave,
           isave, dsave, options.max_line_search_steps, ln_task);

    if (task[0] == kScipyTaskFG) {
      value = objective(x);
      if (!std::isfinite(value)) {
        result.message = "objective is not finite";
        break;
      }
      evaluate_gradient(objective, gradient_function, bounds, options, x,
                        gradient);
      if (gradient.size() != x.size() ||
          !std::all_of(gradient.begin(), gradient.end(),
                       [](double g) { return std::isfinite(g); })) {
        result.message = "gradient is not finite";
        break;
      }
      continue;
    }
    if (task[0] == kScipyTaskNewX || task[0] == kScipyTaskRestart) {
      result.iterations = isave[29];
      continue;
    }
    if (task[0] == kScipyTaskConvergence) {
      result.converged = true;
      result.message = "convergence";
    } else {
      result.message = "scipy lbfgsb task=" + std::to_string(task[0]) +
                       " msg=" + std::to_string(task[1]);
    }
    result.iterations = isave[29];
    break;
  }

  result.x = std::move(x);
  result.gradient = std::move(gradient);
  result.value = value;
  if (!result.converged && result.message.empty()) {
    result.message = "maximum reverse-communication calls reached";
  }
  return result;
#endif
}

const char* optimizer_backend_name() {
#ifdef FLASHDEG_HAVE_SCIPY_LBFGSB
  return "scipy-lbfgsb+grid-golden+newton";
#else
  return "local-lbfgsb+grid-golden+newton";
#endif
}

}  // namespace ccdeseq2
