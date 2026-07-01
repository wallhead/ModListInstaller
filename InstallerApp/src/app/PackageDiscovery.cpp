#include "app/PackageDiscovery.h"

#include <algorithm>
#include <vector>

namespace modlist {

namespace {

std::string Lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

bool HasExtension(const std::filesystem::path& path, const std::string& extension) {
  return Lower(path.extension().string()) == extension;
}

bool IsFirstArchivePart(const std::filesystem::path& path) {
  const std::string name = Lower(path.filename().string());
  return name.size() > 7 && name.ends_with(".7z.001");
}

}  // namespace

Result<PackageDiscovery> DiscoverPackageNear(const std::filesystem::path& folder) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
    return Result<PackageDiscovery>::Error("Package folder does not exist: " + folder.string());
  }

  std::vector<std::filesystem::path> torrents;
  std::optional<std::filesystem::path> firstArchivePart;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec || !entry.is_regular_file()) {
      continue;
    }
    const auto path = entry.path();
    if (HasExtension(path, ".torrent")) {
      torrents.push_back(path);
    } else if (!firstArchivePart.has_value() && IsFirstArchivePart(path)) {
      firstArchivePart = path;
    }
  }

  if (torrents.empty()) {
    return Result<PackageDiscovery>::Error("No .torrent file found in: " + folder.string());
  }
  if (torrents.size() > 1) {
    return Result<PackageDiscovery>::Error("More than one .torrent file found. Pass the torrent path explicitly.");
  }

  PackageDiscovery package;
  package.torrentFile = torrents.front();
  package.firstArchivePart = firstArchivePart;
  return Result<PackageDiscovery>::Ok(std::move(package));
}

Result<PackageDiscovery> DiscoverPackageFromTorrent(const std::filesystem::path& torrentFile) {
  std::error_code ec;
  if (!std::filesystem::exists(torrentFile, ec) || !std::filesystem::is_regular_file(torrentFile, ec)) {
    return Result<PackageDiscovery>::Error("Torrent file does not exist: " + torrentFile.string());
  }
  if (!HasExtension(torrentFile, ".torrent")) {
    return Result<PackageDiscovery>::Error("Expected a .torrent file: " + torrentFile.string());
  }

  auto package = DiscoverPackageNear(torrentFile.parent_path());
  if (!package.ok()) {
    PackageDiscovery direct;
    direct.torrentFile = torrentFile;
    return Result<PackageDiscovery>::Ok(std::move(direct));
  }
  package.value().torrentFile = torrentFile;
  return package;
}

}  // namespace modlist
