#include "ccdeseq2/table.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <unordered_set>
#include <utility>

#include "ccdeseq2/errors.hpp"

namespace ccdeseq2 {
namespace {

std::unordered_map<std::string, std::size_t> make_index(
    const std::vector<std::string>& names, std::string_view kind) {
  ensure_unique_names(names, kind);
  std::unordered_map<std::string, std::size_t> index;
  index.reserve(names.size());
  for (std::size_t i = 0; i < names.size(); ++i) {
    index.emplace(names[i], i);
  }
  return index;
}

std::size_t lookup(const std::unordered_map<std::string, std::size_t>& index,
                   std::string_view name, std::string_view kind) {
  const auto it = index.find(std::string(name));
  if (it == index.end()) {
    std::ostringstream msg;
    msg << "Unknown " << kind << " '" << name << "'.";
    throw Error(ExitCode::input_error, msg.str());
  }
  return it->second;
}

}  // namespace

void ensure_unique_names(const std::vector<std::string>& names,
                         std::string_view kind) {
  std::unordered_set<std::string> seen;
  seen.reserve(names.size());
  for (const auto& name : names) {
    if (name.empty()) {
      std::ostringstream msg;
      msg << "Empty " << kind << " name is not allowed.";
      throw Error(ExitCode::input_error, msg.str());
    }
    if (!seen.insert(name).second) {
      std::ostringstream msg;
      msg << "Duplicate " << kind << " name '" << name << "'.";
      throw Error(ExitCode::input_error, msg.str());
    }
  }
}

CountMatrix::CountMatrix(std::vector<std::string> sample_names,
                         std::vector<std::string> gene_names)
    : sample_names_(std::move(sample_names)),
      gene_names_(std::move(gene_names)),
      sample_index_(make_index(sample_names_, "sample")),
      gene_index_(make_index(gene_names_, "gene")),
      values_(sample_names_.size() * gene_names_.size(), 0.0) {}

double& CountMatrix::operator()(std::size_t sample_index,
                                std::size_t gene_index) {
  assert(sample_index < sample_count());
  assert(gene_index < gene_count());
  return values_[offset(sample_index, gene_index)];
}

double CountMatrix::operator()(std::size_t sample_index,
                               std::size_t gene_index) const {
  assert(sample_index < sample_count());
  assert(gene_index < gene_count());
  return values_[offset(sample_index, gene_index)];
}

double* CountMatrix::gene_data(std::size_t gene_index) {
  return values_.data() + offset(0, gene_index);
}

const double* CountMatrix::gene_data(std::size_t gene_index) const {
  return values_.data() + offset(0, gene_index);
}

void CountMatrix::fill(double value) { std::fill(values_.begin(), values_.end(), value); }

std::vector<double> CountMatrix::gene_values(std::size_t gene_index) const {
  const double* begin = gene_data(gene_index);
  return std::vector<double>(begin, begin + sample_count());
}

std::size_t CountMatrix::sample_index(std::string_view sample_name) const {
  return lookup(sample_index_, sample_name, "sample");
}

std::size_t CountMatrix::gene_index(std::string_view gene_name) const {
  return lookup(gene_index_, gene_name, "gene");
}

std::size_t CountMatrix::offset(std::size_t sample_index,
                                std::size_t gene_index) const noexcept {
  return gene_index * sample_names_.size() + sample_index;
}

MetadataTable::MetadataTable(std::vector<std::string> sample_names,
                             std::vector<std::string> column_names,
                             std::vector<std::vector<std::string>> rows)
    : sample_names_(std::move(sample_names)),
      column_names_(std::move(column_names)),
      sample_index_(make_index(sample_names_, "metadata sample")),
      column_index_(make_index(column_names_, "metadata column")),
      rows_(std::move(rows)) {}

const std::string& MetadataTable::value(std::size_t sample_index,
                                        std::string_view column_name) const {
  return value(sample_index, column_index(column_name));
}

const std::string& MetadataTable::value(std::size_t sample_index,
                                        std::size_t column_index) const {
  assert(sample_index < sample_count());
  assert(column_index < column_count());
  return rows_[sample_index][column_index];
}

std::size_t MetadataTable::sample_index(std::string_view sample_name) const {
  return lookup(sample_index_, sample_name, "metadata sample");
}

std::size_t MetadataTable::column_index(std::string_view column_name) const {
  return lookup(column_index_, column_name, "metadata column");
}

}  // namespace ccdeseq2
