#include "paths/WindowsFileMove.h"

#include <algorithm>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace modlist {
namespace {

constexpr unsigned long kCancelledError = 1223;

MoveTreeResult FailedTreeMove(const std::filesystem::path& source,
                              const std::filesystem::path& target,
                              unsigned long error,
                              bool usedDirectoryFallback) {
  return {false, error, source, target, usedDirectoryFallback};
}

bool IsTraversableDirectory(const std::filesystem::path& path,
                            std::error_code& ec) {
  const auto status = std::filesystem::symlink_status(path, ec);
  if (ec || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    return false;
  }
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
    return false;
  }
  return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
#else
  return true;
#endif
}

bool ContainsPayload(const std::filesystem::path& directory,
                     std::error_code& ec) {
  auto iterator = std::filesystem::directory_iterator(directory, ec);
  const auto end = std::filesystem::directory_iterator();
  while (!ec && iterator != end) {
    const auto entry = iterator->path();
    const bool traversableDirectory = IsTraversableDirectory(entry, ec);
    if (ec) {
      return true;
    }
    if (!traversableDirectory || ContainsPayload(entry, ec)) {
      return true;
    }
    iterator.increment(ec);
  }
  return ec ? true : false;
}

MoveTreeResult MoveTree(const std::filesystem::path& source,
                        const std::filesystem::path& target,
                        const MoveTreeOptions& options,
                        bool usedDirectoryFallback) {
  if (options.cancelRequested && options.cancelRequested()) {
    return FailedTreeMove(source, target, kCancelledError, usedDirectoryFallback);
  }

  std::error_code ec;
  const bool directory = IsTraversableDirectory(source, ec);
  if (ec) {
    return FailedTreeMove(source, target, static_cast<unsigned long>(ec.value()),
                          usedDirectoryFallback);
  }

  if (!directory) {
    const auto moved = MovePathWithRetry(
        source, target, true, options.maxAttempts, options.retryDelay,
        [&](uint32_t attempt, uint32_t maxAttempts, unsigned long error) {
          if (options.retryCallback) {
            options.retryCallback(source, target, attempt, maxAttempts, error);
          }
        });
    return {moved.ok, moved.error, source, target, usedDirectoryFallback};
  }

  const bool targetExists = std::filesystem::exists(target, ec);
  if (ec) {
    return FailedTreeMove(source, target, static_cast<unsigned long>(ec.value()),
                          usedDirectoryFallback);
  }
  if (!targetExists) {
    const auto moved = MovePathWithRetry(
        source, target, false, 1, std::chrono::milliseconds(0));
    if (moved.ok) {
      return {true, 0, source, target, usedDirectoryFallback};
    }
    usedDirectoryFallback = true;
    if (options.directoryFallbackCallback) {
      options.directoryFallbackCallback(source, target, moved.error);
    }
  } else {
    usedDirectoryFallback = true;
  }

  std::filesystem::create_directories(target, ec);
  if (ec) {
    return FailedTreeMove(source, target, static_cast<unsigned long>(ec.value()),
                          usedDirectoryFallback);
  }

  std::vector<std::filesystem::path> entries;
  auto iterator = std::filesystem::directory_iterator(source, ec);
  const auto end = std::filesystem::directory_iterator();
  while (!ec && iterator != end) {
    entries.push_back(iterator->path());
    iterator.increment(ec);
  }
  if (ec) {
    return FailedTreeMove(source, target, static_cast<unsigned long>(ec.value()),
                          usedDirectoryFallback);
  }

  for (const auto& entry : entries) {
    auto result = MoveTree(entry, target / entry.filename(), options,
                           usedDirectoryFallback);
    usedDirectoryFallback = usedDirectoryFallback || result.usedDirectoryFallback;
    if (!result.ok) {
      result.usedDirectoryFallback = usedDirectoryFallback;
      return result;
    }
  }

  std::filesystem::remove(source, ec);
  if (ec) {
    const auto cleanupError = static_cast<unsigned long>(ec.value());
    ec.clear();
    const bool sourceContainsPayload = ContainsPayload(source, ec);
    if (!ec && !sourceContainsPayload) {
      if (options.sourceCleanupFailureCallback) {
        options.sourceCleanupFailureCallback(source, cleanupError);
      }
      return {true, 0, source, target, usedDirectoryFallback};
    }
    return FailedTreeMove(source, target, cleanupError, usedDirectoryFallback);
  }
  return {true, 0, source, target, usedDirectoryFallback};
}

}  // namespace

bool IsTransientMoveError(unsigned long error) {
#ifdef _WIN32
  return error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ||
         error == ERROR_LOCK_VIOLATION;
#else
  (void)error;
  return false;
#endif
}

MovePathResult MovePathWithRetry(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    bool replaceExisting,
    uint32_t maxAttempts,
    std::chrono::milliseconds retryDelay,
    MoveRetryCallback retryCallback) {
  MovePathResult result;
  const uint32_t attempts = std::max<uint32_t>(1, maxAttempts);
  for (uint32_t attempt = 1; attempt <= attempts; ++attempt) {
    result.attempts = attempt;
#ifdef _WIN32
    DWORD flags = MOVEFILE_WRITE_THROUGH;
    if (replaceExisting) {
      flags |= MOVEFILE_REPLACE_EXISTING;
    }
    if (MoveFileExW(source.c_str(), target.c_str(), flags)) {
      result.ok = true;
      result.error = ERROR_SUCCESS;
      return result;
    }
    result.error = GetLastError();
#else
    std::error_code ec;
    if (replaceExisting) {
      std::filesystem::remove(target, ec);
      ec.clear();
    }
    std::filesystem::rename(source, target, ec);
    if (!ec) {
      result.ok = true;
      return result;
    }
    result.error = static_cast<unsigned long>(ec.value());
#endif
    if (attempt == attempts || !IsTransientMoveError(result.error)) {
      return result;
    }
    if (retryCallback) {
      retryCallback(attempt, attempts, result.error);
    }
    std::this_thread::sleep_for(retryDelay);
  }
  return result;
}

MoveTreeResult MovePathTreeWithRetry(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    const MoveTreeOptions& options) {
  return MoveTree(source, target, options, false);
}

}  // namespace modlist
