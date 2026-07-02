#pragma once

#include "common/Result.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <atomic>

namespace modlist {

struct ExtractionConfig {
  std::filesystem::path sevenZipExe;
  std::filesystem::path archiveFirstPart;
  std::filesystem::path installFolder;
  const std::atomic_bool* cancelRequested{nullptr};
  bool useSameDiskTemp{true};
};

struct ExtractionResult {
  bool ok{false};
  int exitCode{-1};
  std::string command;
  std::string output;
  std::filesystem::path outputLogPath;
  std::filesystem::path tempFolder;
  std::string message;
};

class SevenZipExtractor {
public:
  using ProgressCallback = std::function<void(int)>;

  Result<std::filesystem::path> LocateExecutable(const std::filesystem::path& appRoot) const;
  ExtractionResult Extract(const ExtractionConfig& config, ProgressCallback progressCallback = {}) const;
  static std::string BuildCommand(const ExtractionConfig& config);
};

}  // namespace modlist
