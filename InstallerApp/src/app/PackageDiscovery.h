#pragma once

#include "common/Result.h"

#include <filesystem>
#include <optional>

namespace modlist {

struct PackageDiscovery {
  std::filesystem::path torrentFile;
  std::optional<std::filesystem::path> firstArchivePart;
};

struct InstallerPackageLayout {
  std::filesystem::path manifestPath;
  std::filesystem::path archiveFolder;
};

InstallerPackageLayout LocateInstallerPackage(const std::filesystem::path& executableFolder);
bool InstallationOverlapsPackage(const std::filesystem::path& target,
                                 const std::filesystem::path& packageFolder);

Result<PackageDiscovery> DiscoverPackageNear(const std::filesystem::path& folder,
                                           bool includeTorrents = true);
Result<PackageDiscovery> DiscoverPackageFromTorrent(const std::filesystem::path& torrentFile);

}  // namespace modlist
