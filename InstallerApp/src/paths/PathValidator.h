#pragma once

#include "common/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <span>

namespace modlist {

struct PathValidationResult {
  bool ok{false};
  bool warning{false};
  std::string message;
  std::filesystem::path normalizedPath;
  std::string driveRoot;
  uintmax_t freeBytes{0};
};

struct InstallSpacePlan {
  uintmax_t unpackRequiredBytes{0};
  uintmax_t installRequiredBytes{0};
};

inline constexpr size_t kDirectExtractionPathBudget = 240;

struct ExtractionPathPlan {
  std::filesystem::path folder;
  size_t longestFinalPath{0};
  size_t entryCount{0};
  bool direct{false};
};

bool IsSafeArchiveOutputPath(const std::filesystem::path& path);
Result<ExtractionPathPlan> PlanArchiveExtraction(
    const std::filesystem::path& finalFolder,
    const std::filesystem::path& stagingFolder,
    std::span<const std::filesystem::path> entries);

uintmax_t ExtractionSpaceRequirement(uintmax_t unpackedBytes);
InstallSpacePlan PlanInstallSpace(uintmax_t unpackedBytes, bool sameVolume);
std::filesystem::path AutomaticUnpackFolder(
    const std::filesystem::path& selectedInstallRoot);

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
