#include "ccdeseq2/csv.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>

#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/file_io.hpp"

namespace ccdeseq2 {
namespace {

[[nodiscard]] long long parse_count(std::string_view text,
                                    const std::filesystem::path& path,
                                    std::size_t line_number,
                                    std::string_view sample_name,
                                    std::string_view gene_name) {
  const std::string trimmed = trim_copy(text);
  if (trimmed.empty()) {
    std::ostringstream msg;
    msg << path.string() << ":" << line_number
        << ": empty count for sample '" << sample_name << "', gene '" << gene_name
        << "'.";
    throw Error(ExitCode::input_error, msg.str());
  }
  long long value = 0;
  const char* first = trimmed.data();
  const char* last = trimmed.data() + trimmed.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  if (ec != std::errc() || ptr != last || value < 0) {
    std::ostringstream msg;
    msg << path.string() << ":" << line_number
        << ": invalid non-negative integer count '" << trimmed << "' for sample '"
        << sample_name << "', gene '" << gene_name << "'.";
    throw Error(ExitCode::input_error, msg.str());
  }
  return value;
}

void write_csv_escaped(std::ostream& out, const std::string& value) {
  const bool quote = value.find_first_of(",\"\n\r") != std::string::npos;
  if (!quote) {
    out << value;
    return;
  }
  out << '"';
  for (const char ch : value) {
    if (ch == '"') {
      out << "\"\"";
    } else {
      out << ch;
    }
  }
  out << '"';
}

void write_number(std::ostream& out, double value) {
  if (std::isnan(value)) {
    return;
  }
  out << std::setprecision(17) << value;
}

}  // namespace

std::string trim_copy(std::string_view text) {
  std::size_t first = 0;
  while (first < text.size()) {
    const char ch = text[first];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      break;
    }
    ++first;
  }

  std::size_t last = text.size();
  while (last > first) {
    const char ch = text[last - 1];
    if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      break;
    }
    --last;
  }
  return std::string(text.substr(first, last - first));
}

std::vector<std::string> parse_csv_line(std::string_view line, char delimiter) {
  std::vector<std::string> fields;
  std::string current;
  bool in_quotes = false;

  for (std::size_t i = 0; i < line.size(); ++i) {
    const char ch = line[i];
    if (in_quotes) {
      if (ch == '"') {
        if (i + 1 < line.size() && line[i + 1] == '"') {
          current.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        current.push_back(ch);
      }
    } else {
      if (ch == '"') {
        in_quotes = true;
      } else if (ch == delimiter) {
        fields.push_back(trim_copy(current));
        current.clear();
      } else {
        current.push_back(ch);
      }
    }
  }

  if (in_quotes) {
    throw Error(ExitCode::input_error, "Unterminated quoted CSV field.");
  }
  fields.push_back(trim_copy(current));
  return fields;
}

char detect_delimiter_from_extension(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  if (ext == ".tsv" || ext == ".tab") {
    return '\t';
  }
  return ',';
}

CsvTable read_csv_table(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw Error(ExitCode::input_error, "Could not open CSV file: " + path.string());
  }

  const char delimiter = detect_delimiter_from_extension(path);

  CsvTable table;
  std::string line;
  std::size_t line_number = 0;

  if (!std::getline(in, line)) {
    throw Error(ExitCode::input_error, "CSV file is empty: " + path.string());
  }
  ++line_number;
  table.header = parse_csv_line(line, delimiter);
  if (table.header.size() < 2) {
    throw Error(ExitCode::input_error,
                "CSV header must contain row-name column and at least one data column: " +
                    path.string());
  }
  std::vector<std::string> column_names(table.header.begin() + 1, table.header.end());
  ensure_unique_names(column_names, "CSV column");

  while (std::getline(in, line)) {
    ++line_number;
    if (trim_copy(line).empty()) {
      continue;
    }
    auto fields = parse_csv_line(line, delimiter);
    if (fields.size() != table.header.size()) {
      std::ostringstream msg;
      msg << path.string() << ":" << line_number << ": expected "
          << table.header.size() << " fields, found " << fields.size() << ".";
      throw Error(ExitCode::input_error, msg.str());
    }
    table.row_names.push_back(fields.front());
    fields.erase(fields.begin());
    table.rows.push_back(std::move(fields));
  }

  ensure_unique_names(table.row_names, "CSV row");
  return table;
}

CountMatrix read_count_matrix(const std::filesystem::path& path,
                              CountOrientation orientation) {
  CsvTable table = read_csv_table(path);
  std::vector<std::string> column_names(table.header.begin() + 1, table.header.end());

  if (orientation == CountOrientation::features_as_rows) {
    CountMatrix counts(column_names, table.row_names);
    for (std::size_t gene = 0; gene < table.row_names.size(); ++gene) {
      for (std::size_t sample = 0; sample < column_names.size(); ++sample) {
        counts(sample, gene) = static_cast<double>(parse_count(
            table.rows[gene][sample], path, gene + 2, column_names[sample],
            table.row_names[gene]));
      }
    }
    return counts;
  }

  CountMatrix counts(table.row_names, column_names);
  for (std::size_t sample = 0; sample < table.row_names.size(); ++sample) {
    for (std::size_t gene = 0; gene < column_names.size(); ++gene) {
      counts(sample, gene) = static_cast<double>(parse_count(
          table.rows[sample][gene], path, sample + 2, table.row_names[sample],
          column_names[gene]));
    }
  }
  return counts;
}

MetadataTable read_metadata_table(const std::filesystem::path& path) {
  CsvTable table = read_csv_table(path);
  std::vector<std::string> column_names(table.header.begin() + 1, table.header.end());
  return MetadataTable(table.row_names, column_names, table.rows);
}

void write_matrix_csv(const std::filesystem::path& path,
                      const std::vector<std::string>& row_names,
                      const std::vector<std::string>& column_names,
                      const std::vector<double>& values_row_major,
                      const std::string& row_name_column) {
  ensure_parent_directory(path, "CSV file");
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw Error(ExitCode::input_error, "Could not write CSV file: " + path.string());
  }
  write_csv_escaped(out, row_name_column);
  for (const auto& column : column_names) {
    out << ',';
    write_csv_escaped(out, column);
  }
  out << '\n';
  for (std::size_t row = 0; row < row_names.size(); ++row) {
    write_csv_escaped(out, row_names[row]);
    for (std::size_t col = 0; col < column_names.size(); ++col) {
      out << ',';
      write_number(out, values_row_major.at(row * column_names.size() + col));
    }
    out << '\n';
  }
}

void write_series_csv(const std::filesystem::path& path,
                      const std::string& column_name,
                      const std::vector<std::string>& row_names,
                      const std::vector<double>& values,
                      const std::string& row_name_column) {
  ensure_parent_directory(path, "CSV file");
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw Error(ExitCode::input_error, "Could not write CSV file: " + path.string());
  }
  write_csv_escaped(out, row_name_column);
  out << ',';
  write_csv_escaped(out, column_name);
  out << '\n';
  for (std::size_t i = 0; i < row_names.size(); ++i) {
    write_csv_escaped(out, row_names[i]);
    out << ',';
    write_number(out, values.at(i));
    out << '\n';
  }
}

}  // namespace ccdeseq2
