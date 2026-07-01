#include "verifier/Sha256.h"

#include <fstream>
#include <iomanip>
#include <sstream>

namespace modlist {

namespace {

constexpr std::array<uint32_t, 64> k = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

uint32_t RotRight(uint32_t value, uint32_t bits) {
  return (value >> bits) | (value << (32 - bits));
}

uint32_t ReadBigEndian32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) |
         static_cast<uint32_t>(data[3]);
}

void WriteBigEndian32(uint8_t* output, uint32_t value) {
  output[0] = static_cast<uint8_t>(value >> 24);
  output[1] = static_cast<uint8_t>(value >> 16);
  output[2] = static_cast<uint8_t>(value >> 8);
  output[3] = static_cast<uint8_t>(value);
}

std::string ToHex(const std::array<uint8_t, 32>& digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

}  // namespace

Sha256::Sha256() {
  state_ = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
}

void Sha256::Update(const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    buffer_[bufferLength_++] = data[i];
    if (bufferLength_ == buffer_.size()) {
      Transform(buffer_.data());
      bitLength_ += 512;
      bufferLength_ = 0;
    }
  }
}

void Sha256::Update(const std::vector<uint8_t>& data) {
  Update(data.data(), data.size());
}

std::array<uint8_t, 32> Sha256::Final() {
  uint64_t totalBitLength = bitLength_ + bufferLength_ * 8;
  buffer_[bufferLength_++] = 0x80;
  if (bufferLength_ > 56) {
    while (bufferLength_ < 64) {
      buffer_[bufferLength_++] = 0;
    }
    Transform(buffer_.data());
    bufferLength_ = 0;
  }
  while (bufferLength_ < 56) {
    buffer_[bufferLength_++] = 0;
  }
  for (int i = 7; i >= 0; --i) {
    buffer_[bufferLength_++] = static_cast<uint8_t>(totalBitLength >> (i * 8));
  }
  Transform(buffer_.data());

  std::array<uint8_t, 32> digest{};
  for (size_t i = 0; i < state_.size(); ++i) {
    WriteBigEndian32(digest.data() + i * 4, state_[i]);
  }
  return digest;
}

Result<std::string> Sha256::FileHexDigest(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Result<std::string>::Error("Unable to open file for SHA256: " + path.string());
  }
  Sha256 sha;
  std::array<uint8_t, 64 * 1024> buffer{};
  while (input) {
    input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
    const auto count = input.gcount();
    if (count > 0) {
      sha.Update(buffer.data(), static_cast<size_t>(count));
    }
  }
  return Result<std::string>::Ok(ToHex(sha.Final()));
}

std::string Sha256::HexDigest(const std::string& text) {
  Sha256 sha;
  sha.Update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
  return ToHex(sha.Final());
}

void Sha256::Transform(const uint8_t* chunk) {
  std::array<uint32_t, 64> w{};
  for (size_t i = 0; i < 16; ++i) {
    w[i] = ReadBigEndian32(chunk + i * 4);
  }
  for (size_t i = 16; i < 64; ++i) {
    const uint32_t s0 = RotRight(w[i - 15], 7) ^ RotRight(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = RotRight(w[i - 2], 17) ^ RotRight(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = state_[0];
  uint32_t b = state_[1];
  uint32_t c = state_[2];
  uint32_t d = state_[3];
  uint32_t e = state_[4];
  uint32_t f = state_[5];
  uint32_t g = state_[6];
  uint32_t h = state_[7];

  for (size_t i = 0; i < 64; ++i) {
    const uint32_t s1 = RotRight(e, 6) ^ RotRight(e, 11) ^ RotRight(e, 25);
    const uint32_t ch = (e & f) ^ (~e & g);
    const uint32_t temp1 = h + s1 + ch + k[i] + w[i];
    const uint32_t s0 = RotRight(a, 2) ^ RotRight(a, 13) ^ RotRight(a, 22);
    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t temp2 = s0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

}  // namespace modlist
