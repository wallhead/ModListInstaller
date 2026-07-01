#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace modlist {

struct PathValidationResult {
  bool ok{false};
  bool warning{false};
  std::string message;
  std::filesystem::path normalizedPath;
  std::string driveRoot;
  uintmax_t freeBytes{0};
};

class IPathValidator {
public:
  virtual ~IPathValidator() = default;
  virtual PathValidationResult ValidateDownloadFolder(const std::filesystem::path& path, uintmax_t requiredBytes = 0) const = 0;
  virtual PathValidationResult ValidateInstallFolder(const std::filesystem::path& path, uintmax_t requiredBytes = 0) const = 0;
  virtual bool IsSameDrive(const std::filesystem::path& a, const std::filesystem::path& b) const = 0;
};

class PathValidator : public IPathValidator {
public:
  explicit PathValidator(size_t maxRecommendedInstallPathLength = 20);

  PathValidationResult ValidateDownloadFolder(const std::filesystem::path& path, uintmax_t requiredBytes = 0) const override;
  PathValidationResult ValidateInstallFolder(const std::filesystem::path& path, uintmax_t requiredBytes = 0) const override;
  bool IsSameDrive(const std::filesystem::path& a, const std::filesystem::path& b) const override;

private:
  PathValidationResult ValidateBaseFolder(const std::filesystem::path& path, uintmax_t requiredBytes) const;
  size_t maxRecommendedInstallPathLength_;
};

std::filesystem::path SameDiskTempPath(const std::filesystem::path& installFolder);

}  // namespace modlist
