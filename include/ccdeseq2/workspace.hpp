#pragma once

#include <cstddef>
#include <vector>

namespace ccdeseq2 {

struct ThreadWorkspace {
  // Reused inside gene-block loops. The names describe the intended lifetime
  // class; individual kernels may reinterpret them only after earlier uses end.
  std::vector<double> sample_buffer;
  std::vector<double> coefficient_buffer;
  std::vector<double> coefficient_step_buffer;
  std::vector<double> rhs_buffer;
  std::vector<double> solve_temp_buffer;
  std::vector<double> system_matrix_buffer;
  std::vector<double> chol_factor_buffer;

  void reserve_for_design_columns(unsigned int columns) {
    const unsigned int capped = columns < 32U ? columns : 32U;
    coefficient_buffer.reserve(capped);
    coefficient_step_buffer.reserve(capped);
    rhs_buffer.reserve(capped);
    solve_temp_buffer.reserve(capped);
    system_matrix_buffer.reserve(static_cast<std::size_t>(capped) * capped);
    chol_factor_buffer.reserve(static_cast<std::size_t>(capped) * capped);
  }
};

}  // namespace ccdeseq2
