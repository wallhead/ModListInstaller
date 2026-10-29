#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <sstream>
#include <vector>

namespace modlist {

inline std::filesystem::path UserRuntimeFolder(const std::filesystem::path& packageRoot) {
  std::vector<wchar_t> buffer(32768, L'\0');
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
  const auto cacheRoot = length > 0 && length < buffer.size()
      ? std::filesystem::path(std::wstring(buffer.data(), length))
      : std::filesystem::temp_directory_path();
  std::error_code ec;
  auto identity = std::filesystem::absolute(packageRoot, ec).lexically_normal().wstring();
  if (ec) {
    identity = packageRoot.lexically_normal().wstring();
  }
  uint64_t hash = 14695981039346656037ull;
  for (wchar_t ch : identity) {
    hash ^= static_cast<uint64_t>(std::towlower(ch));
    hash *= 1099511628211ull;
  }
  std::wostringstream key;
  key << std::hex << hash;
  return cacheRoot / L"ModlistInstaller" / key.str();
}

}  // namespace modlist
