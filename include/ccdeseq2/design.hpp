#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ccdeseq2/table.hpp"

namespace ccdeseq2 {

struct FactorInfo {
  std::string name;
  bool numeric = false;
  std::string reference_level;
  std::vector<std::string> levels;
  std::map<std::string, std::size_t> level_to_column;
};

class DesignMatrix {
 public:
  DesignMatrix() = default;
  DesignMatrix(std::vector<std::string> sample_names,
               std::vector<std::string> column_names,
               std::vector<double> values_row_major,
               std::vector<FactorInfo> factors);

  [[nodiscard]] std::size_t sample_count() const noexcept {
    return sample_names_.size();
  }
  [[nodiscard]] std::size_t column_count() const noexcept {
    return column_names_.size();
  }
  [[nodiscard]] const std::vector<std::string>& sample_names() const noexcept {
    return sample_names_;
  }
  [[nodiscard]] const std::vector<std::string>& column_names() const noexcept {
    return column_names_;
  }
  [[nodiscard]] const std::vector<double>& values_row_major() const noexcept {
    return values_;
  }
  [[nodiscard]] double operator()(std::size_t sample_index,
                                  std::size_t column_index) const;
  [[nodiscard]] std::vector<double> contrast_vector(std::string_view factor,
                                                    std::string_view tested_level,
                                                    std::string_view control_level) const;

 private:
  [[nodiscard]] const FactorInfo& factor(std::string_view name) const;

  std::vector<std::string> sample_names_;
  std::vector<std::string> column_names_;
  std::vector<double> values_;
  std::vector<FactorInfo> factors_;
};

[[nodiscard]] DesignMatrix build_design_matrix(
    const MetadataTable& metadata,
    const std::vector<std::string>& ordered_samples,
    std::string_view formula,
    const std::map<std::string, std::string>& ref_levels);

// Result of checking that a reduced design is nested within a full design for a
// likelihood-ratio test. degrees_of_freedom is the rank difference, i.e. the
// number of parameters the reduced model drops relative to the full model.
struct NestedDesignValidation {
  std::size_t full_rank = 0;
  std::size_t reduced_rank = 0;
  std::size_t degrees_of_freedom = 0;
};

// Validates that `reduced` is nested within `full`: identical samples in the
// same order, both designs full column rank, and the reduced column space is
// contained in the full column space (rank([full | reduced]) == rank(full)).
// Returns the ranks and df = rank(full) - rank(reduced). Throws ccdeseq2::Error
// on sample mismatch, rank deficiency, non-nestedness, or df == 0.
[[nodiscard]] NestedDesignValidation validate_nested_designs(
    const DesignMatrix& full, const DesignMatrix& reduced);

[[nodiscard]] std::vector<double> parse_contrast_vector(std::string_view text);

}  // namespace ccdeseq2
