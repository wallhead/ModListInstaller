#include "paths/WindowsFileMove.h"

#include <algorithm>
#include <system_error>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace modlist {

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

}  // namespace modlist
