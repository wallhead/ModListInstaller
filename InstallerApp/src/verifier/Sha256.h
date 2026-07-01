#pragma once

#include "common/Result.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace modlist {

class Sha256 {
public:
  Sha256();

  void Update(const uint8_t* data, size_t length);
  void Update(const std::vector<uint8_t>& data);
  std::array<uint8_t, 32> Final();

  static Result<std::string> FileHexDigest(const std::filesystem::path& path);
  static std::string HexDigest(const std::string& text);

private:
  void Transform(const uint8_t* chunk);

  std::array<uint8_t, 64> buffer_{};
  std::array<uint32_t, 8> state_{};
  uint64_t bitLength_{0};
  size_t bufferLength_{0};
};

}  // namespace modlist
