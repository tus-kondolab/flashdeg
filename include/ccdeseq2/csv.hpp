#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "ccdeseq2/table.hpp"

namespace ccdeseq2 {

struct CsvTable {
  std::vector<std::string> header;
  std::vector<std::string> row_names;
  std::vector<std::vector<std::string>> rows;
};

[[nodiscard]] std::string trim_copy(std::string_view text);
[[nodiscard]] std::vector<std::string> parse_csv_line(std::string_view line,
                                                      char delimiter = ',');
// Returns ',' or '\t' based on the file extension (.tsv / .tab -> tab,
// everything else -> comma). Case-insensitive.
[[nodiscard]] char detect_delimiter_from_extension(
    const std::filesystem::path& path);

[[nodiscard]] CsvTable read_csv_table(const std::filesystem::path& path);
[[nodiscard]] CountMatrix read_count_matrix(const std::filesystem::path& path,
                                            CountOrientation orientation);
[[nodiscard]] MetadataTable read_metadata_table(const std::filesystem::path& path);

void write_matrix_csv(const std::filesystem::path& path,
                      const std::vector<std::string>& row_names,
                      const std::vector<std::string>& column_names,
                      const std::vector<double>& values_row_major,
                      const std::string& row_name_column = "");

void write_series_csv(const std::filesystem::path& path,
                      const std::string& column_name,
                      const std::vector<std::string>& row_names,
                      const std::vector<double>& values,
                      const std::string& row_name_column = "");

}  // namespace ccdeseq2
