#include "verifier/Verifier.h"

#include "manifest/Manifest.h"
#include "verifier/Sha256.h"

namespace modlist {

VerificationSummary Verifier::Verify(const std::filesystem::path& downloadFolder, const std::vector<ManifestFile>& files) const {
  VerificationSummary summary;
  summary.ok = true;

  for (const auto& expected : files) {
    FileVerificationResult result;
    result.path = expected.path;
    result.expectedSize = expected.size;
    result.expectedSha256 = expected.sha256;

    if (!IsSafeManifestRelativePath(expected.path)) {
      result.message = "Unsafe manifest path";
      summary.ok = false;
      summary.files.push_back(std::move(result));
      continue;
    }

    const auto fullPath = (downloadFolder / expected.path).lexically_normal();
    std::error_code ec;
    result.exists = std::filesystem::exists(fullPath, ec) && std::filesystem::is_regular_file(fullPath, ec);
    if (!result.exists) {
      result.message = "File is missing";
      summary.ok = false;
      summary.files.push_back(std::move(result));
      continue;
    }

    result.actualSize = std::filesystem::file_size(fullPath, ec);
    result.sizeMatches = !ec && result.actualSize == result.expectedSize;
    if (!result.sizeMatches) {
      result.message = "File size mismatch";
      summary.ok = false;
      summary.files.push_back(std::move(result));
      continue;
    }

    auto digest = Sha256::FileHexDigest(fullPath);
    if (!digest.ok()) {
      result.message = digest.error();
      summary.ok = false;
      summary.files.push_back(std::move(result));
      continue;
    }
    result.actualSha256 = digest.value();
    result.hashMatches = result.actualSha256 == result.expectedSha256;
    if (!result.hashMatches) {
      result.message = "SHA256 mismatch";
      summary.ok = false;
    } else {
      result.message = "OK";
    }
    summary.files.push_back(std::move(result));
  }

  return summary;
}

}  // namespace modlist
