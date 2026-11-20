#pragma once

#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace modlist {

// Use only at I/O boundaries; path comparisons and logs use the original names.
inline std::filesystem::path WindowsIoPath(const std::filesystem::path& path) {
#ifdef _WIN32
  // Directory APIs can hit the legacy limit before the pathname reaches MAX_PATH.
  if (path.native().size() < MAX_PATH - 12 || !path.is_absolute()) {
    return path;
  }
  auto normalized = path.lexically_normal();
  normalized.make_preferred();
  const auto& text = normalized.native();
  if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\")) {
    return path;
  }
  if (text.starts_with(L"\\\\")) {
    return std::filesystem::path(L"\\\\?\\UNC\\" + text.substr(2));
  }
  return std::filesystem::path(L"\\\\?\\" + text);
#else
  return path;
#endif
}

}  // namespace modlist
