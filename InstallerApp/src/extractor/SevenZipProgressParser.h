#pragma once

#include <optional>
#include <string_view>

namespace modlist::detail {

class SevenZipProgressParser {
 public:
  std::optional<int> Consume(std::string_view output) noexcept {
    std::optional<int> latest;
    for (const char ch : output) {
      // 7-Zip prints the percentage first and redraws the console line.
      const bool newFrame = ch == '\r' || ch == '\n' || ch == '\b';
      const bool padding = ch == ' ' || ch == '\t';
      if (atPercent_) {
        if (newFrame || padding) latest = value_;
        atPercent_ = false;
        ignoreFrame_ = true;
      }
      if (newFrame) {
        value_ = 0;
        digits_ = 0;
        ignoreFrame_ = false;
      } else if (!ignoreFrame_) {
        if (padding && digits_ == 0) continue;
        if (ch >= '0' && ch <= '9' && digits_ < 3) {
          value_ = value_ * 10 + (ch - '0');
          ++digits_;
          ignoreFrame_ = value_ > 100;
        } else if (ch == '%' && digits_ != 0) {
          atPercent_ = true;
        } else {
          ignoreFrame_ = true;
        }
      }
    }
    return latest;
  }

  std::optional<int> Finish() const noexcept {
    return atPercent_ ? std::optional<int>(value_) : std::nullopt;
  }

 private:
  int value_{0};
  int digits_{0};
  bool atPercent_{false};
  bool ignoreFrame_{false};
};

}  // namespace modlist::detail
