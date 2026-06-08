#include "ccdeseq2/profile.hpp"

#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#include "ccdeseq2/errors.hpp"
#include "ccdeseq2/file_io.hpp"

namespace ccdeseq2 {
namespace {

[[nodiscard]] std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (const char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        out << ch;
        break;
    }
  }
  return out.str();
}

}  // namespace

ProfileReport::ProfileReport(bool include_cpu_time)
    : include_cpu_time_(include_cpu_time) {}

void ProfileReport::add_wall_time(std::string name, double wall_ms, double cpu_ms) {
  ProfileEntry& entry = entries_[std::move(name)];
  entry.wall_ms += wall_ms;
  entry.cpu_ms += cpu_ms;
}

void ProfileReport::set_peak_memory_mib(double mib) { peak_memory_mib_ = mib; }

void ProfileReport::set_metadata(std::string key, std::string value) {
  metadata_[std::move(key)] = std::move(value);
}

void ProfileReport::set_metadata(std::map<std::string, std::string> metadata) {
  for (auto& [key, value] : metadata) {
    metadata_[std::move(key)] = std::move(value);
  }
}

void ProfileReport::write_text(std::ostream& out) const {
  for (const auto& [key, value] : metadata_) {
    out << "[profile] " << std::left << std::setw(28) << (key + ":")
        << std::right << value << '\n';
  }
  for (const auto& [name, entry] : entries_) {
    out << "[profile] " << std::left << std::setw(28) << (name + ":") << std::right
        << std::fixed << std::setprecision(3) << entry.wall_ms << " ms";
    if (include_cpu_time_) {
      out << " cpu=" << std::fixed << std::setprecision(3) << entry.cpu_ms << " ms";
    }
    out << '\n';
  }
  out << "[profile] " << std::left << std::setw(28) << "peak_memory:"
      << std::right << std::fixed << std::setprecision(3) << peak_memory_mib_
      << " MiB\n";
}

void ProfileReport::write_json(const std::filesystem::path& path) const {
  ensure_parent_directory(path, "profile JSON");
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    throw Error(ExitCode::input_error,
                "Could not write profile JSON: " + path.string());
  }
  out << "{\n";
  out << "  \"schema_version\": 1,\n";
  out << "  \"metadata\": {";
  bool first_metadata = true;
  for (const auto& [key, value] : metadata_) {
    if (!first_metadata) {
      out << ",";
    }
    first_metadata = false;
    out << "\n    \"" << json_escape(key) << "\": \""
        << json_escape(value) << "\"";
  }
  if (!metadata_.empty()) {
    out << "\n  ";
  }
  out << "},\n";
  out << "  \"steps\": {\n";
  bool first = true;
  for (const auto& [name, entry] : entries_) {
    if (!first) {
      out << ",\n";
    }
    first = false;
    out << "    \"" << json_escape(name) << "\": {\"wall_ms\": "
        << std::setprecision(17) << entry.wall_ms;
    if (include_cpu_time_) {
      out << ", \"cpu_ms\": " << std::setprecision(17) << entry.cpu_ms;
    }
    out << "}";
  }
  out << "\n  },\n";
  out << "  \"peak_memory_mib\": " << std::setprecision(17) << peak_memory_mib_
      << "\n";
  out << "}\n";
}

ScopedProfileTimer::ScopedProfileTimer(ProfileReport* report, std::string name)
    : report_(report),
      name_(std::move(name)),
      wall_start_(std::chrono::steady_clock::now()),
      cpu_start_(std::clock()) {}

ScopedProfileTimer::~ScopedProfileTimer() {
  if (report_ == nullptr) {
    return;
  }
  const auto wall_end = std::chrono::steady_clock::now();
  const std::clock_t cpu_end = std::clock();
  const double wall_ms =
      std::chrono::duration<double, std::milli>(wall_end - wall_start_).count();
  const double cpu_ms =
      1000.0 * static_cast<double>(cpu_end - cpu_start_) / CLOCKS_PER_SEC;
  report_->add_wall_time(std::move(name_), wall_ms, cpu_ms);
}

double peak_memory_mib() {
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS info;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info))) {
    return static_cast<double>(info.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }
  return 0.0;
#else
  struct rusage usage {};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0.0;
  }
#ifdef __APPLE__
  return static_cast<double>(usage.ru_maxrss) / (1024.0 * 1024.0);
#else
  return static_cast<double>(usage.ru_maxrss) / 1024.0;
#endif
#endif
}

}  // namespace ccdeseq2
