#pragma once

#include "common/Result.h"

#include <filesystem>
#include <optional>

namespace modlist {

struct PackageDiscovery {
  std::filesystem::path torrentFile;
  std::optional<std::filesystem::path> firstArchivePart;
};

Result<PackageDiscovery> DiscoverPackageNear(const std::filesystem::path& folder);
Result<PackageDiscovery> DiscoverPackageFromTorrent(const std::filesystem::path& torrentFile);

}  // namespace modlist
