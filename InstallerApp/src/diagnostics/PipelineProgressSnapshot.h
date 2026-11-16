#pragma once

#include "common/PathText.h"
#include "extractor/PipelinedSevenZipExtractor.h"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace modlist::diagnostics {

class PipelineProgressSnapshot {
public:
  using LineSink = std::function<void(const std::string&)>;

  explicit PipelineProgressSnapshot(LineSink sink) : sink_(std::move(sink)) {}

  void Publish(const PipelineExtractionProgress& progress) {
    std::lock_guard lock(mutex_);
    latest_ = Snapshot{progress.validationArchive, progress.validatedBytes, progress.extractionPercent};
  }

  // Stage's sampler is the sole consumer; Finish samples only after joining it.
  void Sample(bool final = false) {
    std::optional<Snapshot> snapshot;
    { std::lock_guard lock(mutex_); snapshot = latest_; }
    if (!snapshot) return;
    if (!final && logged_ && snapshot->archive == logged_->archive &&
        snapshot->validatedBytes == logged_->validatedBytes && snapshot->percent == logged_->percent) return;
    sink_("PIPELINE archive=" + PathToUtf8(snapshot->archive) +
          "; validation_bytes=" + std::to_string(snapshot->validatedBytes) +
          "; extraction_percent=" + std::to_string(snapshot->percent));
    logged_ = std::move(snapshot);
  }

private:
  struct Snapshot {
    std::filesystem::path archive;
    uint64_t validatedBytes;
    int percent;
  };
  LineSink sink_;
  std::mutex mutex_;
  std::optional<Snapshot> latest_, logged_;
};

}  // namespace modlist::diagnostics
