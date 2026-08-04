#include "ui/NativeStrings.h"

#include "manifest/Json.h"

#include <windows.h>

#include <fstream>
#include <iterator>

namespace modlist {
namespace {

std::wstring WidenUtf8(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                       static_cast<int>(value.size()), nullptr, 0);
  if (size <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                          static_cast<int>(value.size()), result.data(), size) <= 0) {
    return {};
  }
  return result;
}

void ReplaceAll(std::wstring& value, const std::wstring& needle,
                const std::wstring& replacement) {
  if (needle.empty()) {
    return;
  }
  size_t offset = 0;
  while ((offset = value.find(needle, offset)) != std::wstring::npos) {
    value.replace(offset, needle.size(), replacement);
    offset += replacement.size();
  }
}

}  // namespace

bool NativeStrings::Load(const std::filesystem::path& path, std::wstring* warning) {
  values_.clear();
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    if (warning != nullptr) {
      *warning = L"Файл текста не найден; используются встроенные строки: " + path.wstring();
    }
    return false;
  }
  const std::string content((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  auto parsed = ParseJson(content);
  if (!parsed.ok() || !parsed.value().IsObject()) {
    if (warning != nullptr) {
      *warning = L"Не удалось прочитать файл текста; используются встроенные строки: " +
                 path.wstring();
    }
    return false;
  }
  for (const auto& [key, value] : parsed.value().AsObject()) {
    if (!value.IsString()) {
      continue;
    }
    auto text = WidenUtf8(value.AsString());
    if (!text.empty() || value.AsString().empty()) {
      values_.emplace_back(key, std::move(text));
    }
  }
  return true;
}

std::wstring NativeStrings::Get(std::string_view key, std::wstring_view fallback) const {
  for (const auto& [candidate, value] : values_) {
    if (candidate == key) {
      return value;
    }
  }
  return std::wstring(fallback);
}

std::wstring NativeStrings::Format(
    std::string_view key,
    std::wstring_view fallback,
    const std::vector<std::pair<std::wstring, std::wstring>>& replacements) const {
  std::wstring result = Get(key, fallback);
  for (const auto& [name, value] : replacements) {
    ReplaceAll(result, L"{" + name + L"}", value);
  }
  return result;
}

}  // namespace modlist
