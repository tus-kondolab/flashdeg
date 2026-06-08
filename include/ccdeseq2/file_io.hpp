#pragma once

#include <filesystem>
#include <string_view>

namespace ccdeseq2 {

void ensure_parent_directory(const std::filesystem::path& path,
                             std::string_view output_kind);

}  // namespace ccdeseq2
