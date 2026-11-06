#pragma once

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace modlist::detail {

struct VerifiedBlock {
  std::vector<uint8_t> data;
  uint64_t accessStamp{0};
};

// Queue/cache policy only; the stream serializes access and performs I/O outside its lock.
class VerifiedBlockScheduler {
public:
  explicit VerifiedBlockScheduler(size_t count) : count_(count) {}

  void Request(size_t index) {
    if (index >= count_) {
      ResetWindow();
      return;
    }
    if (requested_ != index) {
      ResetWindow();
      requested_ = index;
    }
    Queue(index, true);
    for (size_t ahead = 1; ahead <= kReadAhead && ahead < count_ - index; ++ahead) {
      Queue(index + ahead, false);
    }
  }

  std::shared_ptr<VerifiedBlock> Find(size_t index) {
    const auto found = cache_.find(index);
    if (found == cache_.end()) {
      return {};
    }
    found->second->accessStamp = ++stamp_;
    return found->second;
  }

  bool Contains(size_t index) const { return cache_.contains(index); }

  std::optional<size_t> Next() {
    if (loading_) {
      return std::nullopt;
    }
    while (!tasks_.empty()) {
      const size_t index = tasks_.front();
      if (!cache_.contains(index)) {
        if (!MakeRoom(index)) {
          return std::nullopt;
        }
        loading_ = index;
      }
      tasks_.pop_front();
      queued_.erase(index);
      if (loading_) {
        return index;
      }
    }
    return std::nullopt;
  }

  void Complete(size_t index, std::shared_ptr<VerifiedBlock> block) {
    assert(loading_ && *loading_ == index);
    loading_.reset();
    block->accessStamp = ++stamp_;
    cache_[index] = std::move(block);
  }

  void ResetWindow() {
    requested_ = kNoRequest;
    tasks_.clear();
    queued_.clear();
  }

  void VerifyRemaining(const std::vector<bool>& verified) {
    ResetWindow();
    for (size_t index = 0; index < count_; ++index) {
      if (!verified[index]) {
        Queue(index, false);
      }
    }
  }

private:
  static constexpr size_t kReadAhead = 4;
  static constexpr size_t kBufferLimit = 6;
  static constexpr size_t kNoRequest = std::numeric_limits<size_t>::max();

  bool InWindow(size_t index) const {
    return requested_ != kNoRequest && index >= requested_ && index - requested_ <= kReadAhead;
  }

  bool MakeRoom(size_t demand) {
    if (cache_.size() < kBufferLimit) {
      return true;
    }
    auto victim = cache_.end();
    for (auto iterator = cache_.begin(); iterator != cache_.end(); ++iterator) {
      // Upcoming blocks are older than recently consumed ones; LRU alone evicts them.
      if (InWindow(iterator->first) || iterator->second.use_count() > 1) {
        continue;
      }
      if (victim == cache_.end() || iterator->second->accessStamp < victim->second->accessStamp) {
        victim = iterator;
      }
    }
    // Borrowed obsolete blocks may fill the spare capacity. Demand still takes
    // precedence over speculation; discard the farthest forward buffer first.
    if (victim == cache_.end() && demand == requested_) {
      for (auto iterator = cache_.begin(); iterator != cache_.end(); ++iterator) {
        if (iterator->first != requested_ && iterator->second.use_count() == 1 &&
            (victim == cache_.end() || iterator->first > victim->first)) {
          victim = iterator;
        }
      }
    }
    if (victim == cache_.end()) {
      return false;
    }
    cache_.erase(victim);
    return true;
  }

  void Queue(size_t index, bool priority) {
    if (index >= count_ || cache_.contains(index) || loading_ == index) {
      return;
    }
    if (!queued_.insert(index).second) {
      if (!priority) {
        return;
      }
      const auto existing = std::find(tasks_.begin(), tasks_.end(), index);
      tasks_.erase(existing);
    }
    if (priority) {
      tasks_.push_front(index);
    } else {
      tasks_.push_back(index);
    }
  }

  size_t count_;
  size_t requested_{kNoRequest};
  std::optional<size_t> loading_;
  uint64_t stamp_{0};
  std::deque<size_t> tasks_;
  std::unordered_set<size_t> queued_;
  std::unordered_map<size_t, std::shared_ptr<VerifiedBlock>> cache_;
};

}  // namespace modlist::detail
