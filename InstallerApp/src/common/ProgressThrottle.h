#pragma once

#include <chrono>
#include <mutex>

namespace modlist {

class ProgressThrottle {
public:
  explicit ProgressThrottle(std::chrono::milliseconds interval = std::chrono::milliseconds(100))
      : interval_(interval) {}

  bool ShouldReport(bool force = false,
                    std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
    std::lock_guard lock(mutex_);
    if (!force && reported_ && now - last_ < interval_) return false;
    reported_ = true;
    last_ = now;
    return true;
  }

private:
  std::chrono::milliseconds interval_;
  std::chrono::steady_clock::time_point last_{};
  bool reported_{false};
  std::mutex mutex_;
};

}  // namespace modlist
