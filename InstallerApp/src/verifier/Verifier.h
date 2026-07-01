#pragma once

#include "manifest/Manifest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace modlist {

struct FileVerificationResult {
  std::filesystem::path path;
  bool exists{false};
  bool sizeMatches{false};
  bool hashMatches{false};
  uint64_t expectedSize{0};
  uint64_t actualSize{0};
  std::string expectedSha256;
  std::string actualSha256;
  std::string message;
};

struct VerificationSummary {
  bool ok{false};
  std::vector<FileVerificationResult> files;
};

class Verifier {
public:
  VerificationSummary Verify(const std::filesystem::path& downloadFolder, const std::vector<ManifestFile>& files) const;
};

}  // namespace modlist
