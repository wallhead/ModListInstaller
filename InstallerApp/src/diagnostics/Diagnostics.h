#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace modlist::diagnostics {

struct Config {
  std::filesystem::path sourceParent;
  std::filesystem::path destinationParent;
  uint32_t sizeGiB{10};
  uint32_t threads{4};
  bool sevenZipFirst{false};
  bool reversePass{false};
  bool smoke{false};
};

struct FileSpec {
  std::filesystem::path relative;
  uint64_t size{0};
  bool smallFile{false};
  bool directory{false};
};

enum class ProgressBasis { LogicalBytes, ChildInput, ChildOutput };

constexpr uint64_t CounterDelta(uint64_t current,uint64_t previous) {
  return current>=previous?current-previous:0;
}

constexpr uint64_t SelectProgressBytes(ProgressBasis basis,uint64_t logical,uint64_t read,uint64_t written,uint64_t total) {
  if(basis==ProgressBasis::LogicalBytes)return logical;
  const auto count=basis==ProgressBasis::ChildInput?read:written;
  return count<total?count:total;
}

constexpr const char* ProgressBasisName(ProgressBasis basis) {
  return basis==ProgressBasis::ChildInput?"child_process_read_proxy":
         basis==ProgressBasis::ChildOutput?"child_process_write_proxy":"logical_bytes";
}

struct Progress {
  std::string stage;
  std::string message;
  uint64_t bytes{0};
  uint64_t totalBytes{0};
  uint64_t files{0};
  double elapsedSeconds{0};
  double currentMiBps{0};
  int percent{0};
  std::filesystem::path logFolder;
  ProgressBasis basis{ProgressBasis::LogicalBytes};
  double processReadMiBps{0};
  double processWriteMiBps{0};
  double cpuPercent{0};
  double workingSetMiB{0};
};

struct Outcome {
  bool ok{false};
  bool cancelled{false};
  std::string error;
  std::filesystem::path logFolder;
  std::filesystem::path sourceRun;
  std::filesystem::path destinationRun;
};

using ProgressCallback = std::function<void(const Progress&)>;

std::vector<FileSpec> BuildWorkload(uint64_t totalBytes, bool smoke = false);
std::filesystem::path TestParentForDrive(const std::filesystem::path& drive);
std::string ValidateConfig(const Config& config);
Outcome RunBenchmark(const Config& config, std::atomic_bool& cancel, ProgressCallback callback = {});

}  // namespace modlist::diagnostics
