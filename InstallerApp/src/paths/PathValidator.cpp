#include "paths/PathValidator.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace modlist {

namespace {

std::string Upper(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return value;
}

bool StartsWithUsersFolder(const std::filesystem::path& path) {
  const std::string text = Upper(path.lexically_normal().string());
  return text.find(":\\USERS\\") != std::string::npos || text.find(":/USERS/") != std::string::npos;
}

size_t Depth(const std::filesystem::path& path) {
  size_t depth = 0;
  for (const auto& part : path.lexically_normal()) {
    if (part == path.root_name() || part == path.root_directory()) {
      continue;
    }
    ++depth;
  }
  return depth;
}

bool CanWriteProbe(const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (ec) {
    return false;
  }
  const auto probe = folder / ".modlist_write_probe";
  {
    std::ofstream out(probe, std::ios::binary);
    if (!out) {
      return false;
    }
    out << "probe";
  }
  std::filesystem::remove(probe, ec);
  return true;
}

}  // namespace

PathValidator::PathValidator(size_t maxRecommendedInstallPathLength)
    : maxRecommendedInstallPathLength_(maxRecommendedInstallPathLength) {}

PathValidationResult PathValidator::ValidateDownloadFolder(const std::filesystem::path& path, uintmax_t requiredBytes) const {
  auto result = ValidateBaseFolder(path, requiredBytes);
  if (!result.ok) {
    return result;
  }
  if (result.normalizedPath.string().size() > 120) {
    result.warning = true;
    result.message = "Download folder path is long; resume and archive handling will be safer in a shorter folder.";
  }
  return result;
}

PathValidationResult PathValidator::ValidateInstallFolder(const std::filesystem::path& path, uintmax_t requiredBytes) const {
  auto result = ValidateBaseFolder(path, requiredBytes);
  if (!result.ok) {
    return result;
  }

  const std::string text = result.normalizedPath.string();
  if (text.size() > maxRecommendedInstallPathLength_) {
    result.warning = true;
    result.message = "Install path is longer than recommended. Choose a short root-level folder like D:\\Sky.";
  }
  if (StartsWithUsersFolder(result.normalizedPath)) {
    result.warning = true;
    result.message = "Install path is under a user profile. Skyrim modlists are safer in a short root-level folder like D:\\Sky.";
  }
  if (Depth(result.normalizedPath) > 2) {
    result.warning = true;
    result.message = "Install path is deeply nested. Choose a short folder close to the drive root, such as D:\\Sky.";
  }
  return result;
}

bool PathValidator::IsSameDrive(const std::filesystem::path& a, const std::filesystem::path& b) const {
  return Upper(a.lexically_normal().root_name().string()) == Upper(b.lexically_normal().root_name().string());
}

PathValidationResult PathValidator::ValidateBaseFolder(const std::filesystem::path& path, uintmax_t requiredBytes) const {
  PathValidationResult result;
  if (path.empty()) {
    result.message = "Folder path is empty.";
    return result;
  }

  std::error_code ec;
  result.normalizedPath = std::filesystem::absolute(path, ec).lexically_normal();
  if (ec) {
    result.message = "Unable to normalize folder path: " + ec.message();
    return result;
  }
  result.driveRoot = result.normalizedPath.root_name().string();

  if (!CanWriteProbe(result.normalizedPath)) {
    result.message = "Folder is not writable or cannot be created.";
    return result;
  }

  const auto space = std::filesystem::space(result.normalizedPath, ec);
  if (ec) {
    result.message = "Unable to check free disk space: " + ec.message();
    return result;
  }
  result.freeBytes = space.available;
  if (requiredBytes > 0 && result.freeBytes < requiredBytes) {
    result.message = "Folder does not have enough free disk space.";
    return result;
  }

  result.ok = true;
  result.message = "Folder is usable.";
  return result;
}

std::filesystem::path SameDiskTempPath(const std::filesystem::path& installFolder) {
  return installFolder / ".install_temp";
}

}  // namespace modlist
