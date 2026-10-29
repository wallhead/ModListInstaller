#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace modlist {

inline std::string PathToUtf8(const std::filesystem::path& path) {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline std::string GenericPathToUtf8(const std::filesystem::path& path) {
  const auto text = path.generic_u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline std::filesystem::path PathFromUtf8(std::string_view text) {
  return std::filesystem::path(std::u8string(
      reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

inline bool PathTextEqual(std::wstring_view left, std::wstring_view right) {
  if (left.size() != right.size()) return false;
  if (left.empty()) return true;
#ifdef _WIN32
  return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
      right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
#else
  return left == right;
#endif
}

}  // namespace modlist
