#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace modlist {

class NativeStrings {
public:
  bool Load(const std::filesystem::path& path, std::wstring* warning);
  std::wstring Get(std::string_view key, std::wstring_view fallback) const;
  std::wstring Format(
      std::string_view key,
      std::wstring_view fallback,
      const std::vector<std::pair<std::wstring, std::wstring>>& replacements) const;

private:
  std::vector<std::pair<std::string, std::wstring>> values_;
};

}  // namespace modlist
