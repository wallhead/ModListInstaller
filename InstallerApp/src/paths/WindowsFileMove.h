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

using MoveRetryCallback =
    std::function<void(uint32_t attempt, uint32_t maxAttempts, unsigned long error)>;

bool IsTransientMoveError(unsigned long error);

MovePathResult MovePathWithRetry(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    bool replaceExisting,
    uint32_t maxAttempts,
    std::chrono::milliseconds retryDelay,
    MoveRetryCallback retryCallback = {});

}  // namespace modlist
