#pragma once

#include <chrono>
#include <ctime>
#include <filesystem>
#include <map>
#include <optional>
#include <ostream>
#include <string>

namespace ccdeseq2 {

struct ProfileEntry {
  double wall_ms = 0.0;
  double cpu_ms = 0.0;
};

class ProfileReport {
 public:
  explicit ProfileReport(bool include_cpu_time = false);

  void add_wall_time(std::string name, double wall_ms, double cpu_ms);
  void set_peak_memory_mib(double mib);
  void set_metadata(std::string key, std::string value);
  void set_metadata(std::map<std::string, std::string> metadata);

  [[nodiscard]] bool include_cpu_time() const noexcept {
    return include_cpu_time_;
  }

  void write_text(std::ostream& out) const;
  void write_json(const std::filesystem::path& path) const;

 private:
  bool include_cpu_time_;
  std::map<std::string, ProfileEntry> entries_;
  std::map<std::string, std::string> metadata_;
  double peak_memory_mib_ = 0.0;
};

class ScopedProfileTimer {
 public:
  ScopedProfileTimer(ProfileReport* report, std::string name);
  ScopedProfileTimer(const ScopedProfileTimer&) = delete;
  ScopedProfileTimer& operator=(const ScopedProfileTimer&) = delete;
  ~ScopedProfileTimer();

 private:
  ProfileReport* report_;
  std::string name_;
  std::chrono::steady_clock::time_point wall_start_;
  std::clock_t cpu_start_;
};

[[nodiscard]] double peak_memory_mib();

}  // namespace ccdeseq2
