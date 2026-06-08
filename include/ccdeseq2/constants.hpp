#pragma once

namespace ccdeseq2 {

inline constexpr double kDefaultRidgeFactor = 1e-6;

enum class CompatMode {
  pydeseq2,
  deseq2_r,
};

}  // namespace ccdeseq2
