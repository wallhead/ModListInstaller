#pragma once

#include "patch/PatchDeletePaths.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace modlist::patch {

struct PatchEntry {
  uint32_t volume = 0;
  uint64_t id = 0;
  bool directory = false;
  bool operator==(const PatchEntry&) const = default;
};

struct PatchChild {
  std::filesystem::path name;
  PatchEntry entry;
};

// A confirmed root stays pinned until the operation and any rollback finish.
class PatchFolder {
public:
  struct Impl;
  explicit PatchFolder(std::unique_ptr<Impl> impl);
  ~PatchFolder();
  PatchFolder(const PatchFolder&) = delete;
  PatchFolder& operator=(const PatchFolder&) = delete;

  const std::filesystem::path& Root() const;
  const std::filesystem::path& Package() const;
  bool RecoveryOnly() const;
  Result<std::optional<PatchEntry>> Inspect(const std::filesystem::path& relative) const;
  Result<std::vector<PatchChild>> List(const std::filesystem::path& relative) const;
  Result<PatchEntry> MakeDirectory(const std::filesystem::path& relative);
  Result<void*> CreateNewOutputFile(const std::filesystem::path& relative);
  Result<void> Rename(const std::filesystem::path& source, const PatchEntry& expected,
                      const std::filesystem::path& destination);
  Result<void> RemoveEmptyDirectory(const std::filesystem::path& relative, const PatchEntry& expected);

  Result<void> BeginWorkspace();
  Result<void> WriteRecovery(std::string_view bytes);
  Result<void> DiscardWorkspace();

private:
  std::unique_ptr<Impl> impl_;
};

enum class PatchSelection { Manual, RegisteredStock };

Result<std::unique_ptr<PatchFolder>> OpenPatchFolder(const std::filesystem::path& selection,
    const std::filesystem::path& package, PatchSelection origin = PatchSelection::Manual);

class PatchLogFile {
public:
  static Result<std::unique_ptr<PatchLogFile>> Open(const PatchFolder& folder);
  ~PatchLogFile();
  const std::filesystem::path& Path() const;
  bool Append(std::string_view text) noexcept;

private:
  struct Impl;
  explicit PatchLogFile(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

}  // namespace modlist::patch
