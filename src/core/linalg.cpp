#include "ccdeseq2/linalg.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>

#include "ccdeseq2/errors.hpp"

#ifdef FLASHDEG_HAVE_EIGEN
#include <Eigen/Dense>
#endif

namespace ccdeseq2 {
namespace {

#ifndef FLASHDEG_HAVE_EIGEN
[[nodiscard]] std::size_t fallback_matrix_rank(std::vector<double> matrix,
                                               std::size_t rows,
                                               std::size_t cols,
                                               double tolerance) {
  if (matrix.size() != rows * cols) {
    throw Error(ExitCode::numeric_error,
                "Rank input matrix has inconsistent dimensions.");
  }
  if (tolerance < 0.0) {
    double max_abs = 0.0;
    for (double value : matrix) {
      max_abs = std::max(max_abs, std::abs(value));
    }
    tolerance = std::numeric_limits<double>::epsilon() *
                static_cast<double>(std::max(rows, cols)) * max_abs;
  }

  std::size_t rank = 0;
  for (std::size_t col = 0; col < cols && rank < rows; ++col) {
    std::size_t pivot = rank;
    double pivot_abs = std::abs(matrix[pivot * cols + col]);
    for (std::size_t row = rank + 1; row < rows; ++row) {
      const double candidate = std::abs(matrix[row * cols + col]);
      if (candidate > pivot_abs) {
        pivot = row;
        pivot_abs = candidate;
      }
    }
    if (pivot_abs <= tolerance) {
      continue;
    }
    if (pivot != rank) {
      for (std::size_t j = col; j < cols; ++j) {
        std::swap(matrix[rank * cols + j], matrix[pivot * cols + j]);
      }
    }
    const double diag = matrix[rank * cols + col];
    for (std::size_t row = rank + 1; row < rows; ++row) {
      const double factor = matrix[row * cols + col] / diag;
      for (std::size_t j = col; j < cols; ++j) {
        matrix[row * cols + j] -= factor * matrix[rank * cols + j];
      }
    }
    ++rank;
  }
  return rank;
}
#endif

}  // namespace

std::vector<double> cholesky_decompose(
    const std::vector<double>& matrix_row_major, std::size_t n) {
  std::vector<double> lower;
  cholesky_decompose_into(matrix_row_major, n, lower);
  return lower;
}

void cholesky_decompose_into(const std::vector<double>& matrix_row_major,
                             std::size_t n,
                             std::vector<double>& lower_row_major) {
  if (matrix_row_major.size() != n * n) {
    throw Error(ExitCode::numeric_error,
                "Cholesky input matrix has inconsistent dimensions.");
  }

#ifdef FLASHDEG_HAVE_EIGEN
  using RowMajorMatrix =
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  const Eigen::Map<const RowMajorMatrix> matrix(matrix_row_major.data(), n, n);
  const Eigen::LLT<Eigen::MatrixXd> llt(matrix);
  if (llt.info() != Eigen::Success) {
    throw Error(ExitCode::numeric_error,
                "Cholesky decomposition failed; matrix is not positive definite.");
  }
  lower_row_major.assign(n * n, 0.0);
  const auto lower = llt.matrixL();
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j <= i; ++j) {
      lower_row_major[i * n + j] =
          lower(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(j));
    }
  }
#else
  lower_row_major.assign(n * n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j <= i; ++j) {
      double sum = matrix_row_major[i * n + j];
      for (std::size_t k = 0; k < j; ++k) {
        sum -= lower_row_major[i * n + k] * lower_row_major[j * n + k];
      }
      if (i == j) {
        if (sum <= 0.0 || !std::isfinite(sum)) {
          throw Error(ExitCode::numeric_error,
                      "Cholesky decomposition failed; matrix is not positive "
                      "definite.");
        }
        lower_row_major[i * n + j] = std::sqrt(sum);
      } else {
        lower_row_major[i * n + j] = sum / lower_row_major[j * n + j];
      }
    }
  }
#endif
}

std::vector<double> cholesky_solve_from_factor(
    const std::vector<double>& lower_row_major, const std::vector<double>& rhs,
    std::size_t n) {
  std::vector<double> solution;
  std::vector<double> temp;
  cholesky_solve_from_factor_into(lower_row_major, rhs, n, solution, temp);
  return solution;
}

void cholesky_solve_from_factor_into(const std::vector<double>& lower_row_major,
                                     const std::vector<double>& rhs,
                                     std::size_t n,
                                     std::vector<double>& solution,
                                     std::vector<double>& temp) {
  if (lower_row_major.size() != n * n || rhs.size() != n) {
    throw Error(ExitCode::numeric_error,
                "Cholesky solve inputs have inconsistent dimensions.");
  }
  if (&rhs == &solution || &rhs == &temp || &solution == &temp) {
    throw Error(ExitCode::numeric_error,
                "Cholesky solve buffers must not alias each other.");
  }

#ifdef FLASHDEG_HAVE_EIGEN
  using RowMajorMatrix =
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  const Eigen::Map<const RowMajorMatrix> lower(lower_row_major.data(), n, n);
  const Eigen::Map<const Eigen::VectorXd> b(rhs.data(),
                                           static_cast<Eigen::Index>(n));
  Eigen::VectorXd y =
      lower.triangularView<Eigen::Lower>().solve(b);
  Eigen::VectorXd x =
      lower.transpose().triangularView<Eigen::Upper>().solve(y);
  solution.assign(x.data(), x.data() + x.size());
  temp.assign(y.data(), y.data() + y.size());
#else
  temp.assign(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    double sum = rhs[i];
    for (std::size_t k = 0; k < i; ++k) {
      sum -= lower_row_major[i * n + k] * temp[k];
    }
    temp[i] = sum / lower_row_major[i * n + i];
  }

  solution.assign(n, 0.0);
  for (std::size_t reverse = 0; reverse < n; ++reverse) {
    const std::size_t i = n - 1 - reverse;
    double sum = temp[i];
    for (std::size_t k = i + 1; k < n; ++k) {
      sum -= lower_row_major[k * n + i] * solution[k];
    }
    solution[i] = sum / lower_row_major[i * n + i];
  }
#endif
}

std::vector<double> cholesky_solve(
    const std::vector<double>& matrix_row_major, const std::vector<double>& rhs,
    std::size_t n) {
  return cholesky_solve_from_factor(cholesky_decompose(matrix_row_major, n), rhs,
                                    n);
}

double positive_definite_logdet(const std::vector<double>& matrix_row_major,
                                std::size_t n) {
  if (matrix_row_major.size() != n * n) {
    throw Error(ExitCode::numeric_error,
                "Log-determinant input matrix has inconsistent dimensions.");
  }

#ifdef FLASHDEG_HAVE_EIGEN
  using RowMajorMatrix =
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  const Eigen::Map<const RowMajorMatrix> matrix(matrix_row_major.data(), n, n);
  const Eigen::LLT<Eigen::MatrixXd> llt(matrix);
  if (llt.info() != Eigen::Success) {
    return -std::numeric_limits<double>::infinity();
  }
  const auto lower = llt.matrixL();
  double logdet = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double diag =
        lower(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(i));
    if (diag <= 0.0 || !std::isfinite(diag)) {
      return -std::numeric_limits<double>::infinity();
    }
    logdet += 2.0 * std::log(diag);
  }
  return logdet;
#else
  const std::vector<double> lower = cholesky_decompose(matrix_row_major, n);
  double logdet = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    logdet += 2.0 * std::log(lower[i * n + i]);
  }
  return logdet;
#endif
}

std::vector<double> least_squares(const std::vector<double>& matrix_row_major,
                                  std::span<const double> rhs,
                                  std::size_t rows, std::size_t cols) {
  if (matrix_row_major.size() != rows * cols || rhs.size() != rows) {
    throw Error(ExitCode::numeric_error,
                "Least-squares inputs have inconsistent dimensions.");
  }

#ifdef FLASHDEG_HAVE_EIGEN
  using RowMajorMatrix =
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  const Eigen::Map<const RowMajorMatrix> matrix(matrix_row_major.data(), rows,
                                               cols);
  const Eigen::Map<const Eigen::VectorXd> y(rhs.data(),
                                           static_cast<Eigen::Index>(rows));
  const Eigen::VectorXd beta = matrix.colPivHouseholderQr().solve(y);
  if (!beta.allFinite()) {
    throw Error(ExitCode::numeric_error,
                "Least-squares solve returned a non-finite solution.");
  }
  return std::vector<double>(beta.data(), beta.data() + beta.size());
#else
  std::vector<double> xtx(cols * cols, 0.0);
  std::vector<double> xty(cols, 0.0);
  for (std::size_t row = 0; row < rows; ++row) {
    const double y = rhs[row];
    for (std::size_t i = 0; i < cols; ++i) {
      const double xi = matrix_row_major[row * cols + i];
      xty[i] += xi * y;
      for (std::size_t j = 0; j <= i; ++j) {
        xtx[i * cols + j] += xi * matrix_row_major[row * cols + j];
      }
    }
  }
  for (std::size_t i = 0; i < cols; ++i) {
    for (std::size_t j = i + 1; j < cols; ++j) {
      xtx[i * cols + j] = xtx[j * cols + i];
    }
  }
  return cholesky_solve(xtx, xty, cols);
#endif
}

std::vector<double> least_squares_multi_rhs(
    const std::vector<double>& matrix_row_major,
    const std::vector<double>& rhs_col_major, std::size_t rows,
    std::size_t cols, std::size_t rhs_cols) {
  if (matrix_row_major.size() != rows * cols ||
      rhs_col_major.size() != rows * rhs_cols) {
    throw Error(ExitCode::numeric_error,
                "Multi-RHS least-squares inputs have inconsistent dimensions.");
  }

#ifdef FLASHDEG_HAVE_EIGEN
  using RowMajorMatrix =
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  using ColMajorMatrix =
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::ColMajor>;
  const Eigen::Map<const RowMajorMatrix> matrix(matrix_row_major.data(), rows,
                                               cols);
  const Eigen::Map<const ColMajorMatrix> rhs(rhs_col_major.data(), rows,
                                            rhs_cols);
  const Eigen::MatrixXd beta = matrix.colPivHouseholderQr().solve(rhs);
  if (!beta.allFinite()) {
    throw Error(ExitCode::numeric_error,
                "Multi-RHS least-squares solve returned a non-finite solution.");
  }
  std::vector<double> result(cols * rhs_cols, 0.0);
  for (std::size_t col = 0; col < rhs_cols; ++col) {
    for (std::size_t row = 0; row < cols; ++row) {
      result[col * cols + row] =
          beta(static_cast<Eigen::Index>(row), static_cast<Eigen::Index>(col));
    }
  }
  return result;
#else
  std::vector<double> result(cols * rhs_cols, 0.0);
  for (std::size_t rhs_col = 0; rhs_col < rhs_cols; ++rhs_col) {
    const double* begin = rhs_col_major.data() + rhs_col * rows;
    const std::vector<double> beta =
        least_squares(matrix_row_major, std::span<const double>(begin, rows),
                      rows, cols);
    for (std::size_t row = 0; row < cols; ++row) {
      result[rhs_col * cols + row] = beta[row];
    }
  }
  return result;
#endif
}

std::size_t matrix_rank(const std::vector<double>& matrix_row_major,
                        std::size_t rows, std::size_t cols,
                        double tolerance) {
  if (matrix_row_major.size() != rows * cols) {
    throw Error(ExitCode::numeric_error,
                "Rank input matrix has inconsistent dimensions.");
  }

#ifdef FLASHDEG_HAVE_EIGEN
  using RowMajorMatrix =
      Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
  const Eigen::Map<const RowMajorMatrix> matrix(matrix_row_major.data(), rows,
                                               cols);
  Eigen::ColPivHouseholderQR<Eigen::MatrixXd> qr(matrix);
  if (tolerance >= 0.0) {
    qr.setThreshold(tolerance);
  }
  return static_cast<std::size_t>(qr.rank());
#else
  return fallback_matrix_rank(matrix_row_major, rows, cols, tolerance);
#endif
}

}  // namespace ccdeseq2
