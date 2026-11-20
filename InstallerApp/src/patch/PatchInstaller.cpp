#include "patch/PatchInstaller.h"

#include "common/BuildIdentity.h"
#include "common/PathText.h"
#include "common/ProgressThrottle.h"
#include "extractor/PipelinedSevenZipExtractor.h"
#include "manifest/Manifest.h"
#include "paths/PathValidator.h"
#include "verifier/Sha256.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <utility>

namespace modlist::patch {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

constexpr char kDeleteList[] = "for delete.txt";
constexpr size_t kMaxTreeEntries = 1000000;

const fs::path kPayload = fs::path(kWorkFolder) / L"new";
const fs::path kOriginals = fs::path(kWorkFolder) / L"originals";

bool Cancelled(const PatchOptions& options) {
  return options.cancelRequested && options.cancelRequested->load();
}

void Notify(const PatchOptions& options, int percent, std::string_view status) noexcept {
  if (options.progress) {
    try { options.progress(percent, std::string(status)); } catch (...) {}
  }
}

void Log(const PatchOptions& options, std::string_view line) noexcept {
  if (options.log) {
    try { options.log(line); } catch (...) {}
  }
}

template <class Format>
void LogFormatted(const std::function<void(std::string_view)>& sink, Format format) noexcept {
  if (sink) {
    try { sink(format()); } catch (...) {}
  }
}

template <class Format>
void LogDetails(const PatchOptions& options, Format format) noexcept {
  LogFormatted(options.log, std::move(format));
}

void Stage(const PatchOptions& options, int percent, std::string_view status) noexcept {
  Notify(options, percent, status);
  Log(options, status);
}

std::string FormatPatchBytes(uintmax_t bytes) {
  constexpr const char* units[]{"B", "KiB", "MiB", "GiB", "TiB"};
  double size = static_cast<double>(bytes);
  size_t unit = 0;
  while (size >= 1024 && unit + 1 < std::size(units)) {
    size /= 1024;
    ++unit;
  }
  std::ostringstream display;
  display << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << size << ' ' << units[unit];
  return display.str();
}

auto ElapsedMs(Clock::time_point started) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
}

const char* StepState(PipelineStepState state) {
  switch (state) {
    case PipelineStepState::Completed: return "completed";
    case PipelineStepState::Failed: return "failed";
    case PipelineStepState::Aborted: return "aborted";
    case PipelineStepState::Cancelled: return "cancelled";
    default: return "skipped";
  }
}

void LogExtractionDiagnostics(const PatchOptions& options, const PipelineExtractionDiagnostics& metrics,
                              const ExtractionResult& result, Clock::time_point started) {
  LogFormatted(options.diagnostics, [&] {
    std::ostringstream report;
    report << "PATCH_EXTRACT result=" << (result.ok ? "success" : "failed")
           << " exit_code=" << result.exitCode << " elapsed_ms=" << ElapsedMs(started);
    const auto step = [&](const char* name, const PipelineStepTiming& timing) {
      report << " " << name << "_ms=" << timing.nanoseconds / 1000000
             << " " << name << "_state=" << StepState(timing.state);
    };
    step("open", metrics.timings.open);
    step("extract", metrics.timings.extract);
    step("final_verify", metrics.timings.finalVerification);
    report << " input_read_bytes=" << metrics.inputDiskBytes.load()
           << " input_read_ops=" << metrics.inputReadOps.load()
           << " block_reloads=" << metrics.blockReloads.load()
           << " cache_hits=" << metrics.cacheHits.load()
           << " cache_misses=" << metrics.cacheMisses.load()
           << " hash_ms=" << metrics.hashNanoseconds.load() / 1000000
           << " output_files=" << metrics.outputFilesCreated.load()
           << " output_bytes=" << metrics.outputBytesWritten.load();
    return report.str();
  });
}

class OrderedArchiveLog {
public:
  void Report(const PatchOptions& options, std::string_view action, size_t index,
              size_t count, const fs::path& archive) {
    if (!options.log || index <= reported_ || index > count || archive.empty()) return;
    if (pending_.empty()) pending_.resize(count);
    if (count != pending_.size() || !pending_[index - 1].empty()) return;
    pending_[index - 1] = archive.filename();
    // Opening the archive may seek into the last volume before the middle ones.
    while (reported_ < pending_.size() && !pending_[reported_].empty()) {
      const size_t next = reported_++;
      LogDetails(options, [&] {
        return std::string(action) + " " + std::to_string(next + 1) + "/" +
               std::to_string(count) + ": " + PathToUtf8(pending_[next]);
      });
    }
  }

private:
  std::vector<fs::path> pending_;
  size_t reported_ = 0;
};

std::optional<PatchOutcome> ExistingWorkspace(PatchFolder& folder) {
  const auto path = folder.Root() / kWorkFolder;
  if (folder.RecoveryOnly()) {
    return PatchOutcome{false, false, "An earlier patch workspace needs attention. Inspect recovery.txt.", path};
  }
  const auto previous = folder.Inspect(kWorkFolder);
  if (!previous.ok()) {
    return PatchOutcome{false, false, "Cannot check the reserved patch workspace: " + previous.error(), path};
  }
  if (previous.value()) {
    return PatchOutcome{false, false, "An earlier patch workspace needs attention. Inspect recovery.txt.", path};
  }
  return std::nullopt;
}

Result<std::string> ReadDeleteText(const fs::path& path) {
  using Outcome = Result<std::string>;
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input) return Outcome::Error("Cannot read for delete.txt next to the patch installer");
  const auto size = input.tellg();
  if (size < 0 || size > static_cast<std::streamoff>(kMaxDeleteListBytes)) {
    return Outcome::Error("for delete.txt exceeds the supported size");
  }
  std::string text(static_cast<size_t>(size), '\0');
  input.seekg(0);
  if (!input.read(text.data(), static_cast<std::streamsize>(text.size())) ||
      input.peek() != std::char_traits<char>::eof() || input.bad()) {
    return Outcome::Error("Could not read the complete delete list");
  }
  return Outcome::Ok(std::move(text));
}

bool IsLegacyDeleteFile(const ManifestFile& entry) {
  return PathTextEqual(entry.path.native(), L"for delete.txt");
}

Result<void> VerifyDeleteText(const Manifest& manifest, const std::string& bytes) {
  const ManifestFile* match = nullptr;
  for (const auto& entry : manifest.files) {
    if (!IsLegacyDeleteFile(entry)) continue;
    if (match) return Result<void>::Error("Manifest contains more than one delete-list entry");
    match = &entry;
  }
  if (!match) {
    return Result<void>::Error("Manifest does not bind for delete.txt; run Prepare-PatchPackage.ps1 on the patch package");
  }
  if (bytes.size() != match->size || Sha256::HexDigest(bytes) != match->sha256) {
    return Result<void>::Error("for delete.txt does not match this package manifest");
  }
  return Result<void>::Ok();
}

Result<std::vector<fs::path>> ReadDeletions(const Manifest& manifest, const fs::path& package) {
  if (manifest.deletePaths) {
    if (std::any_of(manifest.files.begin(), manifest.files.end(), IsLegacyDeleteFile)) {
      return Result<std::vector<fs::path>>::Error("Manifest cannot use both delete_paths and a bound for delete.txt");
    }
    return ParseManifestDeletePaths(*manifest.deletePaths);
  }
  // Early preview packages bound a separate text file in the manifest.
  const auto text = ReadDeleteText(package / kDeleteList);
  if (!text.ok()) return Result<std::vector<fs::path>>::Error(text.error());
  const auto digest = VerifyDeleteText(manifest, text.value());
  if (!digest.ok()) return Result<std::vector<fs::path>>::Error(digest.error());
  std::string_view lines = text.value();
  if (lines.starts_with("\xef\xbb\xbf")) lines.remove_prefix(3);
  return ParsePatchDeleteList(lines);
}

Result<void> EnsureOutputDirectory(PatchFolder& folder, const fs::path& relative) {
  if (!relative.empty() && !IsPatchPathAllowed(relative)) {
    return Result<void>::Error("Unsafe archive output directory");
  }
  auto parent = kPayload;
  auto root = folder.Inspect(parent);
  if (!root.ok() || !root.value() || !root.value()->directory) {
    return Result<void>::Error("Private archive staging folder changed");
  }
  for (const auto& part : relative) {
    parent /= part;
    const auto entry = folder.Inspect(parent);
    if (!entry.ok()) return Result<void>::Error(entry.error());
    if (!entry.value()) {
      const auto made = folder.MakeDirectory(parent);
      if (!made.ok()) return Result<void>::Error(made.error());
    } else if (!entry.value()->directory) {
      return Result<void>::Error("Archive output collides with a file");
    }
  }
  return Result<void>::Ok();
}

Result<void> CollectPatchEntries(PatchFolder& folder, const fs::path& source,
    const fs::path& relative, std::vector<PatchChild>& output, const PatchOptions& options) {
  if (Cancelled(options)) return Result<void>::Error("Patch cancelled");
  auto list = folder.List(source);
  if (!list.ok()) return Result<void>::Error("Cannot inspect " + PathToUtf8(source) + ": " + list.error());
  auto& children = list.value();
  std::sort(children.begin(), children.end(), [](const auto& left, const auto& right) {
    return PathLess{}(left.name, right.name);
  });
  for (size_t index = 0; index < children.size(); ++index) {
    const auto& child = children[index];
    const auto path = relative / child.name;
    if (output.size() >= kMaxTreeEntries || !IsPatchPathAllowed(path) ||
        (index && PathTextEqual(child.name.native(), children[index - 1].name.native()))) {
      return Result<void>::Error("Unsafe or ambiguous patch tree entry: " + PathToUtf8(path));
    }
    output.push_back({path, child.entry});
    if (child.entry.directory) {
      const auto nested = CollectPatchEntries(folder, source / child.name, path, output, options);
      if (!nested.ok()) return nested;
    }
  }
  return Result<void>::Ok();
}

struct Change {
  fs::path from, to;
  PatchEntry entry;
  bool directoryCreation = false;
  bool applied = false;
};

class Changes {
public:
  Changes(PatchFolder& folder, const PatchOptions& options) : folder_(folder), options_(options) {}

  void Backup(const fs::path& relative, const PatchEntry& entry) {
    Move(relative, kOriginals / std::to_wstring(++backupIndex_), entry);
  }
  size_t BackupCount() const { return backupIndex_; }
  void Directory(const fs::path& path) { entries_.push_back({{}, path, {}, true, false}); }
  void Move(const fs::path& from, const fs::path& to, const PatchEntry& entry) {
    entries_.push_back({from, to, entry, false, false});
  }

  Result<void> Apply() {
    std::string planned;
    for (size_t i = next_; i < entries_.size(); ++i) {
      const auto& change = entries_[i];
      if (change.directoryCreation) {
        planned += "MKDIR\t" + GenericPathToUtf8(change.to) + "\n";
      } else {
        planned += "MOVE\t" + GenericPathToUtf8(change.from) + "\t" +
            GenericPathToUtf8(change.to) + "\t" + std::to_string(change.entry.volume) + ":" +
            std::to_string(change.entry.id) + "\n";
      }
      if (planned.size() >= 64 * 1024) {
        const auto saved = folder_.WriteRecovery(planned);
        if (!saved.ok()) return saved;
        planned.clear();
      }
    }
    if (!planned.empty()) {
      const auto saved = folder_.WriteRecovery(planned);
      if (!saved.ok()) return saved;
    }
    // Persist the batch before the first mutation. In-process rollback knows
    // exactly which moves completed; a crash leaves the plan and original data.
    for (; next_ < entries_.size(); ++next_) {
      if (Cancelled(options_)) return Result<void>::Error("Patch cancelled");
      auto& change = entries_[next_];
      if (change.directoryCreation) {
        auto made = folder_.MakeDirectory(change.to);
        if (!made.ok()) return Result<void>::Error("Cannot create " + PathToUtf8(change.to) + ": " + made.error());
        change.entry = made.value();
      } else {
        const auto moved = folder_.Rename(change.from, change.entry, change.to);
        if (!moved.ok()) {
          return Result<void>::Error("Cannot move " + PathToUtf8(change.from) + " to " +
                                    PathToUtf8(change.to) + ": " + moved.error());
        }
      }
      change.applied = true;
    }
    return Result<void>::Ok();
  }

  Result<void> Restore() {
    std::string firstError;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
      if (!it->applied) continue;
      const auto restored = it->directoryCreation
          ? folder_.RemoveEmptyDirectory(it->to, it->entry)
          : folder_.Rename(it->to, it->entry, it->from);
      if (!restored.ok() && firstError.empty()) {
        firstError = "Could not restore " + PathToUtf8(it->directoryCreation ? it->to : it->from) +
                     ": " + restored.error();
      }
    }
    return firstError.empty() ? Result<void>::Ok() : Result<void>::Error(std::move(firstError));
  }

private:
  PatchFolder& folder_;
  const PatchOptions& options_;
  std::vector<Change> entries_;
  size_t next_ = 0;
  size_t backupIndex_ = 0;
};

Result<void> PlanRemovals(PatchFolder& folder, std::span<const fs::path> deletions,
    Changes& changes, const PatchOptions& options) {
  std::set<fs::path, PathLess> unique;
  for (const auto& path : deletions) {
    if (!IsPatchPathAllowed(path)) return Result<void>::Error("Unsafe delete-list path: " + PathToUtf8(path));
    if (!unique.insert(path).second) return Result<void>::Error("Duplicate delete-list path");
  }
  const auto distinct = ValidateDistinctPaths(unique);
  if (!distinct.ok()) return distinct;
  for (const auto& path : deletions) {
    if (Cancelled(options)) return Result<void>::Error("Patch cancelled");
    const auto entry = folder.Inspect(path);
    if (!entry.ok()) return Result<void>::Error("Cannot inspect " + PathToUtf8(path) + ": " + entry.error());
    if (!entry.value()) continue;
    if (entry.value()->directory) {
      std::vector<PatchChild> children;
      const auto checked = CollectPatchEntries(folder, path, path, children, options);
      if (!checked.ok()) return checked;
    }
    changes.Backup(path, *entry.value());
  }
  return Result<void>::Ok();
}

Result<void> PlanOverlay(PatchFolder& folder, Changes& changes, const PatchOptions& options) {
  std::vector<PatchChild> payload;
  const auto scanned = CollectPatchEntries(folder, kPayload, {}, payload, options);
  if (!scanned.ok()) return scanned;
  for (const auto& item : payload) {
    if (Cancelled(options)) return Result<void>::Error("Patch cancelled");
    const auto existing = folder.Inspect(item.name);
    if (!existing.ok()) return Result<void>::Error("Cannot inspect " + PathToUtf8(item.name) + ": " + existing.error());
    if (existing.value() && existing.value()->directory != item.entry.directory) {
      return Result<void>::Error("File/folder conflict at " + PathToUtf8(item.name) +
                                 "; explicitly list the old path for removal");
    }
    if (item.entry.directory) {
      if (!existing.value()) changes.Directory(item.name);
    } else {
      if (existing.value()) changes.Backup(item.name, *existing.value());
      changes.Move(kPayload / item.name, item.name, item.entry);
    }
  }
  return Result<void>::Ok();
}

}  // namespace

PatchOutcome ApplyPatchTransaction(PatchFolder& folder, std::span<const fs::path> deletions,
    const PatchExtract& extract, const PatchOptions& options) {
  if (auto recovery = ExistingWorkspace(folder)) return std::move(*recovery);
  const auto workspace = folder.Root() / kWorkFolder;
  Changes changes(folder, options);
  bool ownsWorkspace = false;
  bool committed = false;
  bool recordingCommit = false;
  auto run = [&]() -> Result<void> {
    if (!extract) return Result<void>::Error("No patch archive extractor");
    Stage(options, 0, "Checking paths to remove");
    const auto deletionsPlanned = PlanRemovals(folder, deletions, changes, options);
    if (!deletionsPlanned.ok()) return deletionsPlanned;
    LogDetails(options, [&] {
      return "Deletion check: " + std::to_string(changes.BackupCount()) + " found, " +
             std::to_string(deletions.size() - changes.BackupCount()) + " already missing";
    });
    if (Cancelled(options)) return Result<void>::Error("Patch cancelled");
    const auto work = folder.BeginWorkspace();
    if (!work.ok()) return work;
    ownsWorkspace = true;
    const auto pending = folder.WriteRecovery(
        "Modlist patch recovery v1\nPENDING\nRoot: " + PathToUtf8(folder.Root()) +
        "\nMOVE rows describe planned operations, which may not all have run.\n"
        "Original entries are in originals/. Do not restore if this record ends with COMMITTED.\n");
    if (!pending.ok()) return pending;
    for (const auto& directory : {kOriginals, kPayload}) {
      const auto made = folder.MakeDirectory(directory);
      if (!made.ok()) return Result<void>::Error(made.error());
    }
    Stage(options, 5, "Removing listed paths");
    const auto removed = changes.Apply();
    if (!removed.ok()) return removed;
    if (Cancelled(options)) return Result<void>::Error("Patch cancelled");
    Stage(options, 10, "Verifying and unpacking the patch");
    const auto extracted = extract(folder.Root() / kPayload);
    if (!extracted.ok()) return extracted;
    if (Cancelled(options)) return Result<void>::Error("Patch cancelled");
    Stage(options, 91, "Checking the destination before replacing files");
    const auto overlay = PlanOverlay(folder, changes, options);
    if (!overlay.ok()) return overlay;
    Stage(options, 92, "Replacing files with the unpacked patch");
    const auto published = changes.Apply();
    if (!published.ok()) return published;
    if (Cancelled(options)) return Result<void>::Error("Patch cancelled");
    recordingCommit = true;
    const auto completion = folder.WriteRecovery("COMMITTED\n");
    if (!completion.ok()) return completion;
    committed = true;
    return Result<void>::Ok();
  };

  std::string error;
  try {
    auto applied = run();
    if (!applied.ok()) error = applied.error();
  } catch (const std::exception& e) {
    error = e.what();
  } catch (...) {
    error = "Unexpected patch failure";
  }
  if (!ownsWorkspace) {
    const auto previous = folder.Inspect(kWorkFolder);
    return {false, Cancelled(options), error,
        previous.ok() && previous.value() ? workspace : fs::path{}};
  }
  if (recordingCommit && !committed) {
    return {false, false, "Patch files were copied, but the completion record could not be saved. "
        "Originals were kept.\n" + error, workspace};
  }
  if (!committed) {
    Stage(options, 92, "Restoring the original files");
    auto restored = changes.Restore();
    if (!restored.ok()) {
      return {false, Cancelled(options), error + "\n" + restored.error(), workspace};
    }
    const auto record = folder.WriteRecovery("RESTORED\n");
    if (!record.ok()) {
      return {false, Cancelled(options), error + "\n" + record.error(), workspace};
    }
    Log(options, "Original files restored");
  }
  if (committed) Stage(options, 97, "Cleaning up the patch workspace");
  else Log(options, "Cleaning up the patch workspace");
  const auto cleared = folder.DiscardWorkspace();
  if (!cleared.ok()) {
    if (committed) Stage(options, 100, "Patch applied; temporary files retained");
    return {committed, !committed && Cancelled(options),
        (committed ? "Patch applied; could not remove temporary files: " : error + "\n") +
        cleared.error(), workspace};
  }
  if (committed) Stage(options, 100, "Patch applied");
  return {committed, !committed && Cancelled(options), committed ? "Patch applied" : error, {}};
}

static PatchOutcome ApplyPatchImpl(PatchFolder& folder, const PatchOptions& options) {
  const auto failure = [&](const std::string& detail) {
    return PatchOutcome{false, Cancelled(options), detail, {}};
  };
  Stage(options, 0, "Checking the patch package");
  LogDetails(options, [&] { return "Patch package: " + PathToUtf8(folder.Package()); });
  try {
    if (Cancelled(options)) return failure("Patch cancelled");
    if (auto recovery = ExistingWorkspace(folder)) return std::move(*recovery);
    const auto manifestPath = folder.Package() / "manifest.json";
    std::string manifestHash;
    const auto manifest = ManifestLoader().LoadFromFile(manifestPath, options.diagnostics ? &manifestHash : nullptr);
    if (!manifest.ok()) return failure(manifest.error());
    LogDetails(options, [&] { return "Manifest loaded: " + PathToUtf8(manifestPath); });
    LogFormatted(options.diagnostics, [&] {
      return "PATCH_PACKAGE manifest_sha256=" + (manifestHash.empty() ? "unavailable" : manifestHash) +
          " chunk_bytes=" + std::to_string(manifest.value().hashChunkSize) +
          " unpacked_bytes=" + std::to_string(manifest.value().unpackedSize);
    });
    if (!PipelinedSevenZipExtractor::CanUse(manifest.value())) {
      return failure("Patch requires a current chunk-verified 7z or ZIP package");
    }
    LogDetails(options, [&] {
      const auto archive = PipelinedSevenZipExtractor::DescribeArchive(manifest.value());
      return "Archive: " + PathToUtf8(manifest.value().extract.firstArchivePart) +
          (archive ? " | Volumes: " + std::to_string(archive->volumeCount) + " | " + FormatPatchBytes(archive->bytes) : "") +
          (manifest.value().unpackedSize > 0 ? " | Unpacked: " + FormatPatchBytes(manifest.value().unpackedSize) : "");
    });
    const auto deletions = ReadDeletions(manifest.value(), folder.Package());
    if (!deletions.ok()) return failure(deletions.error());
    LogDetails(options, [&] { return "Delete list: " + std::to_string(deletions.value().size()) + " path(s)"; });
    std::error_code spaceError;
    const auto space = fs::space(folder.Root(), spaceError);
    const auto needed = ExtractionSpaceRequirement(manifest.value().unpackedSize);
    LogDetails(options, [&] {
      return spaceError ? "Cannot read free space: " + spaceError.message()
          : "Free space: " + FormatPatchBytes(space.available) + " | Required: " + FormatPatchBytes(needed);
    });
    if (spaceError || space.available < needed) return failure("Not enough free space for the unpacked patch and retained originals");
    PipelinedSevenZipExtractor extractor;
    const auto library = extractor.LocateLibrary(folder.Package());
    if (!library.ok()) return failure(library.error());
    PipelinedExtractionConfig config;
    config.sevenZipLibrary = library.value();
    config.archiveFolder = folder.Package();
    config.manifest = &manifest.value();
    config.cancelRequested = options.cancelRequested;
    const auto staging = folder.Root() / kPayload;
    Log(options, "Checking paths in the patch archive");
    const auto inspectionStarted = Clock::now();
    const auto inspected = extractor.PlanExtraction(config, staging, staging);
    LogFormatted(options.diagnostics, [&] {
      return std::string("PATCH_INSPECT status=") + (inspected.ok() ? "success" : "failed") +
          " elapsed_ms=" + std::to_string(ElapsedMs(inspectionStarted)) +
          (inspected.ok() ? " entries=" + std::to_string(inspected.value().entryCount) +
              " longest_staging_path=" + std::to_string(inspected.value().longestFinalPath) : "");
    });
    if (!inspected.ok()) return failure("Cannot read the patch archive: " + inspected.error());
    if (inspected.value().longestFinalPath > 32760) return failure("Patch output path exceeds the Windows path limit");
    LogDetails(options, [&] {
      return "Archive paths checked: " + std::to_string(inspected.value().entryCount) + " entries";
    });
    return ApplyPatchTransaction(folder, deletions.value(), [&](const fs::path& output) {
      const PipelineOutput guarded{
          [&](const fs::path& path) { return EnsureOutputDirectory(folder, path); },
          [&](const fs::path& path) {
            return IsPatchPathAllowed(path)
                ? folder.CreateNewOutputFile(kPayload / path)
                : Result<void*>::Error("Unsafe archive output path");
          }};
      config.installFolder = output;
      config.maxOutputPathLength = 32765;
      config.output = &guarded;
      PipelineExtractionDiagnostics metrics;
      config.diagnostics = options.diagnostics ? &metrics : nullptr;
      LogDetails(options, [&] {
        return "SHA-256 verification and 7-Zip extraction (" + std::to_string(config.decoderThreads) + " threads)";
      });
      OrderedArchiveLog verifying, verified, unpacking;
      ProgressThrottle throttle;
      uint64_t validatedBytes = 0;
      int extractionPercent = 0;
      const auto extractStartedAt = Clock::now();
      const auto extracted = extractor.Extract(config, [&](const PipelineExtractionProgress& progress) noexcept {
        try {
          verifying.Report(options, "Verifying archive", progress.validationArchiveIndex,
                           progress.validationArchiveCount, progress.validationArchive);
          if (progress.validationArchiveTotalBytes > 0 &&
              progress.validationArchiveBytes >= progress.validationArchiveTotalBytes) {
            verified.Report(options, "Archive verified", progress.validationArchiveIndex,
                            progress.validationArchiveCount, progress.validationArchive);
          }
          // A decoder read only uses a verified block, but the reader's status may arrive later.
          verifying.Report(options, "Verifying archive", progress.extractionArchiveIndex,
                           progress.extractionArchiveCount, progress.extractionArchive);
          unpacking.Report(options, "Unpacking archive", progress.extractionArchiveIndex,
                           progress.extractionArchiveCount, progress.extractionArchive);
          if (!options.progress) return;
          validatedBytes = std::max(validatedBytes, progress.validatedBytes);
          extractionPercent = std::max(extractionPercent, std::clamp(progress.extractionPercent, 0, 100));
          const auto total = progress.validationTotalBytes;
          const int verificationPercent = total == 0 ? 0 :
              static_cast<int>(100.0 * static_cast<double>(std::min(total, validatedBytes)) / static_cast<double>(total));
          if (!throttle.ShouldReport()) return;
          const int percent = 10 + std::min(verificationPercent, extractionPercent) * 81 / 100;
          const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - extractStartedAt);
          auto status = "Verification " + std::to_string(verificationPercent) + "% | Unpacking " +
                        std::to_string(extractionPercent) + "%";
          const auto speed = elapsed.count() > 0 ? validatedBytes / static_cast<uint64_t>(elapsed.count()) : 0;
          if (speed > 0) {
            status += " | " + FormatPatchBytes(speed) + "/s";
          }
          status += " | Elapsed " + std::to_string(elapsed.count()) + "s";
          Notify(options, percent, status);
        } catch (...) {
          // An optional observer must not interrupt the archive reader.
        }
      });
      LogExtractionDiagnostics(options, metrics, extracted, extractStartedAt);
      if (extracted.ok) {
        LogDetails(options, [&] {
          return "Verification and extraction completed: " + std::to_string(ElapsedMs(extractStartedAt)) + " ms";
        });
      }
      return extracted.ok ? Result<void>::Ok() : Result<void>::Error(extracted.message);
    }, options);
  } catch (const std::exception& e) {
    return failure(e.what());
  }
}

PatchOutcome ApplyPatch(PatchFolder& folder, const PatchOptions& options) {
  const auto started = Clock::now();
  LogFormatted(options.diagnostics, [&] {
    return "PATCH_RUN start pid=" + std::to_string(GetCurrentProcessId()) +
        " build_id=" + std::string(BuildIdentity());
  });
  LogFormatted(options.diagnostics, [&] { return "Selected modlist: " + PathToUtf8(folder.Root()); });
  auto result = ApplyPatchImpl(folder, options);
  const auto elapsed = ElapsedMs(started);
  LogDetails(options, [&] { return "Patch elapsed time: " + std::to_string(elapsed) + " ms"; });
  LogFormatted(options.diagnostics, [&] {
    return std::string("PATCH_RUN end status=") + (result.succeeded ? "success" : result.cancelled ? "cancelled" : "failed") +
        " elapsed_ms=" + std::to_string(elapsed) + " recovery_needed=" + (result.recoveryFolder.empty() ? "0" : "1");
  });
  if (!result.succeeded || !result.recoveryFolder.empty()) {
    LogFormatted(options.diagnostics, [&] { return "Result detail: " + result.detail; });
    if (!result.recoveryFolder.empty()) {
      LogFormatted(options.diagnostics, [&] { return "Recovery workspace: " + PathToUtf8(result.recoveryFolder); });
    }
  }
  return result;
}

}  // namespace modlist::patch
