#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ccdeseq2 {

enum class CountOrientation {
  features_as_rows,
  features_as_cols,
};

class CountMatrix {
 public:
  CountMatrix() = default;
  CountMatrix(std::vector<std::string> sample_names,
              std::vector<std::string> gene_names);

  [[nodiscard]] std::size_t sample_count() const noexcept {
    return sample_names_.size();
  }
  [[nodiscard]] std::size_t gene_count() const noexcept { return gene_names_.size(); }

  [[nodiscard]] const std::vector<std::string>& sample_names() const noexcept {
    return sample_names_;
  }
  [[nodiscard]] const std::vector<std::string>& gene_names() const noexcept {
    return gene_names_;
  }

  double& operator()(std::size_t sample_index, std::size_t gene_index);
  [[nodiscard]] double operator()(std::size_t sample_index,
                                  std::size_t gene_index) const;

  [[nodiscard]] double* gene_data(std::size_t gene_index);
  [[nodiscard]] const double* gene_data(std::size_t gene_index) const;
  void fill(double value);

  [[nodiscard]] std::vector<double> gene_values(std::size_t gene_index) const;
  [[nodiscard]] std::size_t sample_index(std::string_view sample_name) const;
  [[nodiscard]] std::size_t gene_index(std::string_view gene_name) const;

 private:
  [[nodiscard]] std::size_t offset(std::size_t sample_index,
                                   std::size_t gene_index) const noexcept;

  std::vector<std::string> sample_names_;
  std::vector<std::string> gene_names_;
  std::unordered_map<std::string, std::size_t> sample_index_;
  std::unordered_map<std::string, std::size_t> gene_index_;
  std::vector<double> values_;
};

class MetadataTable {
 public:
  MetadataTable() = default;
  MetadataTable(std::vector<std::string> sample_names,
                std::vector<std::string> column_names,
                std::vector<std::vector<std::string>> rows);

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

  [[nodiscard]] const std::string& value(std::size_t sample_index,
                                         std::string_view column_name) const;
  [[nodiscard]] const std::string& value(std::size_t sample_index,
                                         std::size_t column_index) const;
  [[nodiscard]] std::size_t sample_index(std::string_view sample_name) const;
  [[nodiscard]] std::size_t column_index(std::string_view column_name) const;

 private:
  std::vector<std::string> sample_names_;
  std::vector<std::string> column_names_;
  std::unordered_map<std::string, std::size_t> sample_index_;
  std::unordered_map<std::string, std::size_t> column_index_;
  std::vector<std::vector<std::string>> rows_;
};

void ensure_unique_names(const std::vector<std::string>& names,
                         std::string_view kind);

}  // namespace ccdeseq2
