#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>

namespace modlist {

struct MovePathResult {
  bool ok{false};
  unsigned long error{0};
  uint32_t attempts{0};
};

struct MoveTreeResult {
  bool ok{false};
  unsigned long error{0};
  std::filesystem::path source;
  std::filesystem::path target;
  bool usedDirectoryFallback{false};
};

using MoveRetryCallback =
    std::function<void(uint32_t attempt, uint32_t maxAttempts, unsigned long error)>;

struct MoveTreeOptions {
  uint32_t maxAttempts{3};
  std::chrono::milliseconds retryDelay{std::chrono::seconds(20)};
  std::function<bool()> cancelRequested;
  std::function<void(const std::filesystem::path& source,
                     const std::filesystem::path& target,
                     uint32_t attempt,
                     uint32_t maxAttempts,
                     unsigned long error)> retryCallback;
  std::function<void(const std::filesystem::path& source,
                     const std::filesystem::path& target,
                     unsigned long error)> directoryFallbackCallback;
  std::function<void(const std::filesystem::path& source,
                     unsigned long error)> sourceCleanupFailureCallback;
};

bool IsTransientMoveError(unsigned long error);

MovePathResult MovePathWithRetry(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    bool replaceExisting,
    uint32_t maxAttempts,
    std::chrono::milliseconds retryDelay,
    MoveRetryCallback retryCallback = {});

MoveTreeResult MovePathTreeWithRetry(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    const MoveTreeOptions& options = {});

}  // namespace modlist
