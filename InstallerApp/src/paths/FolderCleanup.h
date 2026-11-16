#pragma once

#include "common/Result.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

namespace modlist {

enum class FolderCleanupKind { ExplicitFinalOverwrite, OwnedStaging };
struct FolderCleanupLimits {
  std::size_t maxEntries = 1000000;
  std::size_t maxDepth = 512;
};
std::filesystem::path StagingOwnerPath(const std::filesystem::path& folder);
bool InstallationOverlapsRuntime(const std::filesystem::path& folder);

class FolderCleanupTicket {
public:
  struct Details;
  std::shared_ptr<const Details> details;
};

// Preparation records identity, not permission to delete a later replacement.
Result<FolderCleanupTicket> PrepareFolderCleanup(
    const std::filesystem::path& folder, const std::filesystem::path& packageFolder,
    FolderCleanupKind kind, const std::string& packageId = {});
Result<void> ClearFolderContents(const FolderCleanupTicket& ticket, const FolderCleanupLimits& limits = {});

class StagingSession {
public:
  struct Impl;
  explicit StagingSession(std::unique_ptr<Impl> impl);
  ~StagingSession();
  StagingSession(const StagingSession&) = delete;
  StagingSession& operator=(const StagingSession&) = delete;
  Result<void> Complete();

private:
  std::unique_ptr<Impl> impl_;
};

// Ownership is a locked two-phase record in the per-user runtime namespace.
Result<std::unique_ptr<StagingSession>> BeginOwnedStaging(
    const std::filesystem::path& folder, const std::filesystem::path& packageFolder,
    const std::string& packageId);

} // namespace modlist
