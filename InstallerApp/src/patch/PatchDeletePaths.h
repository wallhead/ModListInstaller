#pragma once

#include "common/Result.h"

#include <filesystem>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace modlist::patch {

inline constexpr wchar_t kWorkFolder[] = L".modlist-patch";
inline constexpr size_t kMaxDeleteListBytes = 4 * 1024 * 1024;

struct PathLess {
  bool operator()(const std::filesystem::path& a, const std::filesystem::path& b) const;
};

bool IsPatchPathAllowed(const std::filesystem::path& path);
Result<void> ValidateDistinctPaths(const std::set<std::filesystem::path, PathLess>& paths);
Result<std::vector<std::filesystem::path>> ParsePatchDeleteList(std::string_view text);
Result<std::vector<std::filesystem::path>> ParseManifestDeletePaths(std::span<const std::string> paths);

}  // namespace modlist::patch
