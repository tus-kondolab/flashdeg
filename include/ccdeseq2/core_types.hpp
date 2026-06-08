#pragma once

#include <cstdint>
#include <vector>

namespace ccdeseq2 {

using ByteMask = std::vector<std::uint8_t>;

enum class DispersionTrendKind {
  parametric,
  local,
  mean,
};

}  // namespace ccdeseq2
