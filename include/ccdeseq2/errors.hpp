#pragma once

#include <stdexcept>
#include <string>

namespace ccdeseq2 {

enum class ExitCode {
  success = 0,
  input_error = 1,
  convergence_error = 2,
  numeric_error = 3,
  unsupported = 4,
};

class Error : public std::runtime_error {
 public:
  Error(ExitCode code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  [[nodiscard]] ExitCode code() const noexcept { return code_; }

 private:
  ExitCode code_;
};

}  // namespace ccdeseq2
