#include "ccdeseq2/design.hpp"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <set>
#include <sstream>
#include <utility>

#include "ccdeseq2/csv.hpp"
#include "ccdeseq2/errors.hpp"

namespace ccdeseq2 {
namespace {

struct InteractionTerm {
  std::string original;
  std::string left;
  std::string right;
};

[[nodiscard]] bool parse_double_strict(std::string_view text, double& value) {
  const std::string trimmed = trim_copy(text);
  if (trimmed.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  value = std::strtod(trimmed.c_str(), &end);
  return errno != ERANGE && end == trimmed.c_str() + trimmed.size() &&
         std::isfinite(value);
}

[[nodiscard]] std::vector<std::string> split_on_plus(std::string_view text) {
  std::vector<std::string> terms;
  std::string current;
  for (const char ch : text) {
    if (ch == '+') {
      const std::string term = trim_copy(current);
      if (term.empty()) {
        throw Error(ExitCode::input_error, "Empty term in design formula.");
      }
      terms.push_back(term);
      current.clear();
    } else {
      current.push_back(ch);
    }
  }
  const std::string term = trim_copy(current);
  if (!term.empty()) {
    terms.push_back(term);
  }
  return terms;
}

[[nodiscard]] std::vector<std::string> expand_star_terms(
    std::string_view formula) {
  std::string text = trim_copy(formula);
  if (text.empty() || text[0] != '~') {
    throw Error(ExitCode::input_error,
                "Design formula must start with '~', for example '~ condition'.");
  }
  text.erase(text.begin());

  std::vector<std::string> expanded;
  for (const auto& item : split_on_plus(text)) {
    const std::size_t star_count =
        static_cast<std::size_t>(std::count(item.begin(), item.end(), '*'));
    if (star_count == 0) {
      expanded.push_back(item);
      continue;
    }
    if (star_count > 1) {
      throw Error(ExitCode::unsupported,
                  "Three-way or higher interactions ('" + item +
                      "') are not yet supported.");
    }
    const std::size_t pos = item.find('*');
    const std::string left = trim_copy(std::string_view(item).substr(0, pos));
    const std::string right = trim_copy(std::string_view(item).substr(pos + 1));
    if (left.empty() || right.empty()) {
      throw Error(ExitCode::input_error,
                  "Empty side in interaction shorthand '" + item + "'.");
    }
    if (left.find(':') != std::string::npos ||
        right.find(':') != std::string::npos) {
      throw Error(ExitCode::unsupported,
                  "Nested interaction shorthand '" + item +
                      "' is not supported.");
    }
    expanded.push_back(left);
    expanded.push_back(right);
    expanded.push_back(left + ":" + right);
  }
  return expanded;
}

[[nodiscard]] std::vector<std::string> split_terms(std::string_view formula) {
  std::vector<std::string> terms = expand_star_terms(formula);
  if (terms.empty()) {
    throw Error(ExitCode::input_error, "Design formula has no terms.");
  }
  for (const auto& item : terms) {
    if (item == "0" || item == "-1") {
      throw Error(ExitCode::unsupported,
                  "Removing the intercept ('0' / '-1' terms) is not supported.");
    }
    if (item.find('(') != std::string::npos ||
        item.find(')') != std::string::npos ||
        item.find('^') != std::string::npos ||
        item.find('*') != std::string::npos ||
        item.find('-') != std::string::npos ||
        item.find('/') != std::string::npos) {
      throw Error(ExitCode::unsupported,
                  "Formula term '" + item +
                      "' uses unsupported syntax. Two-way interactions via ':' "
                      "and '*' are supported; other operators are not.");
    }
    const std::size_t colon_count =
        static_cast<std::size_t>(std::count(item.begin(), item.end(), ':'));
    if (colon_count > 1) {
      throw Error(ExitCode::unsupported,
                  "Three-way or higher interactions ('" + item +
                      "') are not yet supported.");
    }
  }
  return terms;
}

[[nodiscard]] InteractionTerm parse_interaction_term(const std::string& term) {
  const std::size_t pos = term.find(':');
  if (pos == std::string::npos) {
    throw Error(ExitCode::input_error,
                "Internal error: expected interaction term.");
  }
  InteractionTerm result;
  result.original = term;
  result.left = trim_copy(std::string_view(term).substr(0, pos));
  result.right = trim_copy(std::string_view(term).substr(pos + 1));
  if (result.left.empty() || result.right.empty()) {
    throw Error(ExitCode::input_error,
                "Empty side in interaction term '" + term + "'.");
  }
  return result;
}

[[nodiscard]] std::vector<std::string> sorted_unique_levels(
    const MetadataTable& metadata, const std::vector<std::size_t>& ordered_rows,
    std::size_t column_index) {
  std::set<std::string> levels;
  for (const std::size_t row : ordered_rows) {
    levels.insert(metadata.value(row, column_index));
  }
  return std::vector<std::string>(levels.begin(), levels.end());
}

[[nodiscard]] const FactorInfo* find_factor(
    const std::vector<FactorInfo>& factors, std::string_view name) {
  const auto it = std::find_if(factors.begin(), factors.end(),
                               [&](const FactorInfo& info) { return info.name == name; });
  return it == factors.end() ? nullptr : std::addressof(*it);
}

}  // namespace

DesignMatrix::DesignMatrix(std::vector<std::string> sample_names,
                           std::vector<std::string> column_names,
                           std::vector<double> values_row_major,
                           std::vector<FactorInfo> factors)
    : sample_names_(std::move(sample_names)),
      column_names_(std::move(column_names)),
      values_(std::move(values_row_major)),
      factors_(std::move(factors)) {
  ensure_unique_names(sample_names_, "design sample");
  ensure_unique_names(column_names_, "design column");
}

double DesignMatrix::operator()(std::size_t sample_index,
                                std::size_t column_index) const {
  assert(sample_index < sample_count());
  assert(column_index < column_count());
  return values_[sample_index * column_names_.size() + column_index];
}

std::vector<double> DesignMatrix::contrast_vector(
    std::string_view factor_name, std::string_view tested_level,
    std::string_view control_level) const {
  const FactorInfo& info = factor(factor_name);
  if (info.numeric) {
    throw Error(ExitCode::input_error,
                "Factor-level contrast is only valid for categorical factors.");
  }

  const auto has_level = [&](std::string_view level) {
    const std::string key(level);
    return std::find(info.levels.begin(), info.levels.end(), key) !=
           info.levels.end();
  };
  if (!has_level(tested_level) || !has_level(control_level)) {
    throw Error(ExitCode::input_error,
                "Contrast level was not found in factor '" + info.name + "'.");
  }
  if (tested_level == control_level) {
    throw Error(ExitCode::input_error, "Contrast tested and control levels match.");
  }

  std::vector<double> contrast(column_names_.size(), 0.0);
  const auto add_level = [&](std::string_view level, double sign) {
    const std::string key(level);
    if (key == info.reference_level) {
      return;
    }
    const auto it = info.level_to_column.find(key);
    if (it == info.level_to_column.end()) {
      throw Error(ExitCode::input_error,
                  "Internal contrast error for level '" + key + "'.");
    }
    contrast[it->second] += sign;
  };
  add_level(tested_level, 1.0);
  add_level(control_level, -1.0);
  return contrast;
}

const FactorInfo& DesignMatrix::factor(std::string_view name) const {
  const auto it = std::find_if(factors_.begin(), factors_.end(),
                               [&](const FactorInfo& info) { return info.name == name; });
  if (it == factors_.end()) {
    throw Error(ExitCode::input_error,
                "Unknown design factor '" + std::string(name) + "'.");
  }
  return *it;
}

DesignMatrix build_design_matrix(
    const MetadataTable& metadata, const std::vector<std::string>& ordered_samples,
    std::string_view formula,
    const std::map<std::string, std::string>& ref_levels) {
  std::vector<std::size_t> ordered_rows;
  ordered_rows.reserve(ordered_samples.size());
  for (const auto& sample : ordered_samples) {
    ordered_rows.push_back(metadata.sample_index(sample));
  }

  const std::vector<std::string> terms = split_terms(formula);
  std::vector<std::string> main_terms;
  std::vector<InteractionTerm> interaction_terms;
  main_terms.reserve(terms.size());
  for (const auto& term : terms) {
    if (term.find(':') == std::string::npos) {
      main_terms.push_back(term);
    } else {
      interaction_terms.push_back(parse_interaction_term(term));
    }
  }

  std::vector<std::string> columns{"Intercept"};
  std::vector<FactorInfo> factors;
  std::vector<std::vector<double>> column_values;
  column_values.push_back(std::vector<double>(ordered_samples.size(), 1.0));

  for (const auto& term : main_terms) {
    const std::size_t term_column = metadata.column_index(term);
    bool numeric = true;
    std::vector<double> numeric_values;
    numeric_values.reserve(ordered_samples.size());
    for (const std::size_t row : ordered_rows) {
      double value = 0.0;
      if (!parse_double_strict(metadata.value(row, term_column), value)) {
        numeric = false;
        break;
      }
      numeric_values.push_back(value);
    }

    FactorInfo info;
    info.name = term;
    info.numeric = numeric;

    if (numeric) {
      info.level_to_column.emplace(term, columns.size());
      columns.push_back(term);
      column_values.push_back(std::move(numeric_values));
      factors.push_back(std::move(info));
      continue;
    }

    info.levels = sorted_unique_levels(metadata, ordered_rows, term_column);
    if (info.levels.size() < 2) {
      throw Error(ExitCode::input_error,
                  "Categorical factor '" + term + "' must have at least two levels.");
    }
    const auto ref_it = ref_levels.find(term);
    info.reference_level =
        ref_it == ref_levels.end() ? info.levels.front() : ref_it->second;
    if (std::find(info.levels.begin(), info.levels.end(), info.reference_level) ==
        info.levels.end()) {
      throw Error(ExitCode::input_error,
                  "Reference level '" + info.reference_level +
                      "' was not found in factor '" + term + "'.");
    }

    for (const auto& level : info.levels) {
      if (level == info.reference_level) {
        continue;
      }
      const std::size_t column_index = columns.size();
      info.level_to_column.emplace(level, column_index);
      columns.push_back(term + "[T." + level + "]");
      std::vector<double> values;
      values.reserve(ordered_samples.size());
      for (const std::size_t row : ordered_rows) {
        values.push_back(metadata.value(row, term_column) == level ? 1.0 : 0.0);
      }
      column_values.push_back(std::move(values));
    }
    factors.push_back(std::move(info));
  }

  for (const auto& interaction : interaction_terms) {
    const FactorInfo* left_info = find_factor(factors, interaction.left);
    const FactorInfo* right_info = find_factor(factors, interaction.right);
    if (left_info == nullptr || right_info == nullptr) {
      throw Error(ExitCode::unsupported,
                  "Interaction term '" + interaction.original +
                      "' requires both main effects to be present in the "
                      "design formula.");
    }

    const auto numeric_column = [](const FactorInfo& info) -> std::size_t {
      const auto it = info.level_to_column.find(info.name);
      if (it == info.level_to_column.end()) {
        throw Error(ExitCode::input_error,
                    "Internal numeric interaction error for factor '" +
                        info.name + "'.");
      }
      return it->second;
    };

    if (left_info->numeric && right_info->numeric) {
      const std::size_t left_col = numeric_column(*left_info);
      const std::size_t right_col = numeric_column(*right_info);
      columns.push_back(left_info->name + ":" + right_info->name);
      std::vector<double> values;
      values.reserve(ordered_samples.size());
      for (std::size_t row = 0; row < ordered_samples.size(); ++row) {
        values.push_back(column_values[left_col][row] *
                         column_values[right_col][row]);
      }
      column_values.push_back(std::move(values));
      continue;
    }

    const auto append_numeric_categorical =
        [&](const FactorInfo& numeric_info, const FactorInfo& categorical_info,
            bool numeric_left) {
          const std::size_t numeric_col = numeric_column(numeric_info);
          for (const auto& level : categorical_info.levels) {
            if (level == categorical_info.reference_level) {
              continue;
            }
            const auto cat_it = categorical_info.level_to_column.find(level);
            if (cat_it == categorical_info.level_to_column.end()) {
              throw Error(ExitCode::input_error,
                          "Internal categorical interaction error for level '" +
                              level + "'.");
            }
            const std::string cat_name =
                categorical_info.name + "[T." + level + "]";
            columns.push_back(numeric_left ? numeric_info.name + ":" + cat_name
                                           : cat_name + ":" + numeric_info.name);
            std::vector<double> values;
            values.reserve(ordered_samples.size());
            for (std::size_t row = 0; row < ordered_samples.size(); ++row) {
              values.push_back(column_values[numeric_col][row] *
                               column_values[cat_it->second][row]);
            }
            column_values.push_back(std::move(values));
          }
        };

    if (left_info->numeric || right_info->numeric) {
      if (left_info->numeric) {
        append_numeric_categorical(*left_info, *right_info, true);
      } else {
        append_numeric_categorical(*right_info, *left_info, false);
      }
      continue;
    }

    for (const auto& left_level : left_info->levels) {
      if (left_level == left_info->reference_level) {
        continue;
      }
      const auto left_it = left_info->level_to_column.find(left_level);
      if (left_it == left_info->level_to_column.end()) {
        throw Error(ExitCode::input_error,
                    "Internal interaction error for level '" + left_level + "'.");
      }
      for (const auto& right_level : right_info->levels) {
        if (right_level == right_info->reference_level) {
          continue;
        }
        const auto right_it = right_info->level_to_column.find(right_level);
        if (right_it == right_info->level_to_column.end()) {
          throw Error(ExitCode::input_error,
                      "Internal interaction error for level '" + right_level +
                          "'.");
        }
        const std::string left_name =
            left_info->name + "[T." + left_level + "]";
        const std::string right_name =
            right_info->name + "[T." + right_level + "]";
        columns.push_back(left_name + ":" + right_name);
        std::vector<double> values;
        values.reserve(ordered_samples.size());
        for (std::size_t row = 0; row < ordered_samples.size(); ++row) {
          values.push_back(column_values[left_it->second][row] *
                           column_values[right_it->second][row]);
        }
        column_values.push_back(std::move(values));
      }
    }
  }

  std::vector<double> row_major(ordered_samples.size() * columns.size(), 0.0);
  for (std::size_t col = 0; col < columns.size(); ++col) {
    for (std::size_t row = 0; row < ordered_samples.size(); ++row) {
      row_major[row * columns.size() + col] = column_values[col][row];
    }
  }
  return DesignMatrix(ordered_samples, columns, row_major, factors);
}

std::vector<double> parse_contrast_vector(std::string_view text) {
  std::vector<double> values;
  std::string token;
  std::stringstream stream{std::string(text)};
  while (std::getline(stream, token, ',')) {
    double value = 0.0;
    if (!parse_double_strict(token, value)) {
      throw Error(ExitCode::input_error,
                  "Invalid contrast vector value '" + trim_copy(token) + "'.");
    }
    values.push_back(value);
  }
  if (values.empty()) {
    throw Error(ExitCode::input_error, "Contrast vector is empty.");
  }
  return values;
}

}  // namespace ccdeseq2
