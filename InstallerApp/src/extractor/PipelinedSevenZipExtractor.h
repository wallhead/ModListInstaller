#pragma once

#include "common/Result.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Manifest.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace modlist {

struct PipelineExtractionProgress {
  int extractionPercent{0};
  uint64_t validatedBytes{0};
  uint64_t validationTotalBytes{0};
  std::filesystem::path validationArchive;
  size_t validationArchiveIndex{0};
  size_t validationArchiveCount{0};
  uint64_t validationArchiveBytes{0};
  uint64_t validationArchiveTotalBytes{0};
  std::filesystem::path extractionArchive;
  size_t extractionArchiveIndex{0};
  size_t extractionArchiveCount{0};
};

struct PipelinedExtractionConfig {
  std::filesystem::path sevenZipLibrary;
  std::filesystem::path archiveFolder;
  std::filesystem::path installFolder;
  const Manifest* manifest{nullptr};
  const std::atomic_bool* cancelRequested{nullptr};
  uint32_t decoderThreads{4};
};

class PipelinedSevenZipExtractor {
public:
  using ProgressCallback = std::function<void(const PipelineExtractionProgress&)>;

  static bool CanUse(const Manifest& manifest);
  Result<std::filesystem::path> LocateLibrary(const std::filesystem::path& appRoot) const;
  ExtractionResult Extract(
      const PipelinedExtractionConfig& config,
      ProgressCallback progressCallback = {}) const;
};

}  // namespace modlist
