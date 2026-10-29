#pragma once

#include "common/Result.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace modlist {

inline Result<std::string> EmbeddedTextResource(int identifier) {
  const HMODULE module = GetModuleHandleW(nullptr);
  const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(identifier), MAKEINTRESOURCEW(10));
  if (resource == nullptr) {
    return Result<std::string>::Error("Embedded UI resource was not found");
  }
  const HGLOBAL loaded = LoadResource(module, resource);
  const auto* bytes = static_cast<const char*>(LockResource(loaded));
  const DWORD size = SizeofResource(module, resource);
  if (bytes == nullptr || size == 0) {
    return Result<std::string>::Error("Embedded UI resource could not be loaded");
  }
  return Result<std::string>::Ok(std::string(bytes, size));
}

}  // namespace modlist
