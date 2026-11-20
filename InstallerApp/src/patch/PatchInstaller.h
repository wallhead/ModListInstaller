#pragma once

#include "paths/PatchFolder.h"

#include <atomic>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace modlist::patch {

struct PatchOptions {
  const std::atomic_bool* cancelRequested = nullptr;
  std::function<void(int percent, const std::string& status)> progress;
  // UTF-8 lines emitted serially; the view can queue them for its UI thread.
  std::function<void(std::string_view line)> log;
  // Detailed timings and identifiers intended for the saved log.
  std::function<void(std::string_view line)> diagnostics;
};

struct PatchOutcome {
  bool succeeded = false;
  bool cancelled = false;
  std::string detail;
  std::filesystem::path recoveryFolder;
};

using PatchExtract = std::function<Result<void>(const std::filesystem::path& output)>;

PatchOutcome ApplyPatchTransaction(PatchFolder& folder,
    std::span<const std::filesystem::path> deletions, const PatchExtract& extract,
    const PatchOptions& options = {});

PatchOutcome ApplyPatch(PatchFolder& folder, const PatchOptions& options = {});

}  // namespace modlist::patch
