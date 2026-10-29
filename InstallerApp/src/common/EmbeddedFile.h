#pragma once

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace modlist {

inline bool EmbeddedFileMatches(const std::filesystem::path& path, const void* data, size_t size) {
  std::error_code ec;
  if (std::filesystem::file_size(path, ec) != size || ec) return false;
  std::ifstream input(path, std::ios::binary);
  std::array<char, 64 * 1024> buffer{};
  const auto* expected = static_cast<const char*>(data);
  for (size_t offset = 0; offset < size;) {
    const auto count = std::min(buffer.size(), size - offset);
    input.read(buffer.data(), static_cast<std::streamsize>(count));
    if (input.gcount() != static_cast<std::streamsize>(count) ||
        std::memcmp(buffer.data(), expected + offset, count) != 0) return false;
    offset += count;
  }
  return static_cast<bool>(input);
}

}  // namespace modlist
