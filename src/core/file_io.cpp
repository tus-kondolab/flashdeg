#include "ccdeseq2/file_io.hpp"

#include <sstream>
#include <string_view>

#include "ccdeseq2/errors.hpp"

namespace ccdeseq2 {

void ensure_parent_directory(const std::filesystem::path& path,
                             std::string_view output_kind) {
  const std::filesystem::path parent = path.parent_path();
  if (parent.empty()) {
    return;
  }

  std::error_code ec;
  if (std::filesystem::exists(parent, ec)) {
    if (!ec && std::filesystem::is_directory(parent, ec) && !ec) {
      return;
    }
    std::ostringstream msg;
    msg << "Could not write " << output_kind
        << ": output parent is not a directory: " << parent.string();
    throw Error(ExitCode::input_error, msg.str());
  }
  if (ec) {
    std::ostringstream msg;
    msg << "Could not inspect output directory for " << output_kind << ": "
        << parent.string() << " (" << ec.message() << ")";
    throw Error(ExitCode::input_error, msg.str());
  }

  std::filesystem::create_directories(parent, ec);
  if (ec || !std::filesystem::is_directory(parent, ec) || ec) {
    std::ostringstream msg;
    msg << "Could not create output directory for " << output_kind << ": "
        << parent.string();
    if (ec) {
      msg << " (" << ec.message() << ")";
    }
    throw Error(ExitCode::input_error, msg.str());
  }
}

}  // namespace ccdeseq2
