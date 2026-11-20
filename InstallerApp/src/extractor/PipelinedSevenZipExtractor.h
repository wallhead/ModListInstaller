#pragma once

#include "common/Result.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Manifest.h"
#include "paths/PathValidator.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>

namespace modlist {

struct PipelineArchiveInfo {
  uint64_t bytes;
  size_t volumeCount;
  uint64_t chunkCount;
};

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

enum class PipelineStepState { Skipped, Completed, Failed, Aborted, Cancelled };

struct PipelineStepTiming {
  PipelineStepState state{PipelineStepState::Skipped};
  uint64_t nanoseconds{0};
};

// Cumulative, independently readable counters; durations can overlap and are not additive.
struct PipelineExtractionDiagnostics {
  // Completed successful overlapped reads, including rereads; not physical device traffic.
  std::atomic<uint64_t> inputDiskBytes{0};
  std::atomic<uint64_t> inputReadOps{0};
  // Successful cache loads; reloads are the subset already verified before loading.
  std::atomic<uint64_t> blockLoads{0};
  std::atomic<uint64_t> blockReloads{0};
  // SDK GetBlock lookups, excluding prefetch/verification worker lookups.
  std::atomic<uint64_t> cacheHits{0};
  std::atomic<uint64_t> cacheMisses{0};
  // Missing-block SDK waits only; excludes VerifyAll and worker idle waits.
  std::atomic<uint64_t> blockWaitNanoseconds{0};
  std::atomic<uint64_t> diskWaitNanoseconds{0};
  // Per-block BCrypt setup, data hashing and finalization (not disk reads).
  std::atomic<uint64_t> hashNanoseconds{0};
  // Successful CREATE_ALWAYS opens, actual WriteFile bytes, and attempted WriteFile calls.
  std::atomic<uint64_t> outputFilesCreated{0};
  std::atomic<uint64_t> outputBytesWritten{0};
  std::atomic<uint64_t> outputWriteCalls{0};
  std::atomic<uint64_t> outputCreateNanoseconds{0};
  std::atomic<uint64_t> outputWriteNanoseconds{0};
  std::atomic<uint64_t> outputCloseNanoseconds{0};
  // create_directories calls, including existing directories, root and failed checks.
  std::atomic<uint64_t> outputDirectoriesChecked{0};
  std::atomic<uint64_t> outputDirectoryNanoseconds{0};
  // Per extraction, not cumulative. Read only after Extract returns.
  struct {
    PipelineStepTiming open;
    PipelineStepTiming extract;
    PipelineStepTiming finalVerification;
  } timings;
};

struct PipelineOutput {
  // Relative paths resolved by the caller. A new file returns an owned Win32
  // handle; the extraction stream closes it.
  std::function<Result<void>(const std::filesystem::path&)> ensureDirectory;
  std::function<Result<void*>(const std::filesystem::path&)> openNewFile;
};

struct PipelinedExtractionConfig {
  std::filesystem::path sevenZipLibrary;
  std::filesystem::path archiveFolder;
  std::filesystem::path installFolder;
  const Manifest* manifest{nullptr};
  const std::atomic_bool* cancelRequested{nullptr};
  uint32_t decoderThreads{4};
  size_t maxOutputPathLength{0};
  // Caller-owned; keep alive through Extract, which joins its stream worker before returning.
  PipelineExtractionDiagnostics* diagnostics{nullptr};
  const PipelineOutput* output{nullptr};
};

class PipelinedSevenZipExtractor {
public:
  using ProgressCallback = std::function<void(const PipelineExtractionProgress&)>;

  static std::optional<PipelineArchiveInfo> DescribeArchive(const Manifest& manifest);
  static bool CanUse(const Manifest& manifest);
  Result<std::filesystem::path> LocateLibrary(const std::filesystem::path& appRoot) const;
  Result<ExtractionPathPlan> PlanExtraction(
      const PipelinedExtractionConfig& config,
      const std::filesystem::path& finalFolder,
      const std::filesystem::path& stagingFolder) const;
  ExtractionResult Extract(
      const PipelinedExtractionConfig& config,
      ProgressCallback progressCallback = {}) const;
};

}  // namespace modlist
