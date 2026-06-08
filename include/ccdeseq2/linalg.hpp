#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace ccdeseq2 {

[[nodiscard]] std::vector<double> cholesky_decompose(
    const std::vector<double>& matrix_row_major, std::size_t n);

void cholesky_decompose_into(const std::vector<double>& matrix_row_major,
                             std::size_t n,
                             std::vector<double>& lower_row_major);

[[nodiscard]] std::vector<double> cholesky_solve_from_factor(
    const std::vector<double>& lower_row_major, const std::vector<double>& rhs,
    std::size_t n);

// Buffer contract: rhs, solution, and temp must be distinct vector objects.
// The function resizes solution and temp.
void cholesky_solve_from_factor_into(const std::vector<double>& lower_row_major,
                                     const std::vector<double>& rhs,
                                     std::size_t n,
                                     std::vector<double>& solution,
                                     std::vector<double>& temp);

[[nodiscard]] std::vector<double> cholesky_solve(
    const std::vector<double>& matrix_row_major, const std::vector<double>& rhs,
    std::size_t n);

[[nodiscard]] double positive_definite_logdet(
    const std::vector<double>& matrix_row_major, std::size_t n);

[[nodiscard]] std::vector<double> least_squares(
    const std::vector<double>& matrix_row_major, std::span<const double> rhs,
    std::size_t rows, std::size_t cols);

// rhs_col_major stores rhs_cols response vectors, each of length rows.
// The returned coefficients use the same column-major grouping:
// result[rhs_col * cols + coefficient].
[[nodiscard]] std::vector<double> least_squares_multi_rhs(
    const std::vector<double>& matrix_row_major,
    const std::vector<double>& rhs_col_major, std::size_t rows,
    std::size_t cols, std::size_t rhs_cols);

[[nodiscard]] std::size_t matrix_rank(
    const std::vector<double>& matrix_row_major, std::size_t rows,
    std::size_t cols, double tolerance = -1.0);

}  // namespace ccdeseq2
