#include "app/PackageDiscovery.h"
#include "common/PathText.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
  return Lower(PathToUtf8(path.extension())) == extension;
}

bool IsFirstArchivePart(const std::filesystem::path& path) {
  const std::string name = Lower(PathToUtf8(path.filename()));
  return name.size() > 7 && name.ends_with(".7z.001");
}

}  // namespace

bool InstallationOverlapsPackage(const std::filesystem::path& target,
                                 const std::filesystem::path& packageFolder) {
  std::error_code ec;
  auto install = std::filesystem::weakly_canonical(target, ec).make_preferred();
  if (ec || target.empty()) return true;
  if (packageFolder.empty()) return true;
  const auto isParent = [](const auto& parent, const auto& child) {
    const auto a = parent.native();
    const auto b = child.native();
    if (a.size() > b.size()) return false;
#ifdef _WIN32
    const bool prefix = CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(a.size()), TRUE) == CSTR_EQUAL;
#else
    const bool prefix = b.compare(0, a.size(), a) == 0;
#endif
    return prefix && (a.size() == b.size() || a.back() == std::filesystem::path::preferred_separator ||
                      b[a.size()] == std::filesystem::path::preferred_separator);
  };
  const auto layout = LocateInstallerPackage(packageFolder);
  for (const auto& inputFolder : {packageFolder, layout.archiveFolder,
                                  layout.manifestPath.parent_path()}) {
    auto resolved = std::filesystem::weakly_canonical(inputFolder, ec).make_preferred();
    if (ec || isParent(install, resolved) || isParent(resolved, install)) {
      return true;
    }
  }
  return false;
}

InstallerPackageLayout LocateInstallerPackage(const std::filesystem::path& executableFolder) {
  std::error_code ec;
  const auto manifest = executableFolder / "manifest.json";
  if (std::filesystem::is_regular_file(manifest, ec)) {
    return {manifest, executableFolder};
  }
  ec.clear();
  const auto legacyManifest = executableFolder / "data" / "package" / "manifest.json";
  if (std::filesystem::is_regular_file(legacyManifest, ec)) {
    return {legacyManifest, executableFolder / "data" / "downloads"};
  }
  return {manifest, executableFolder};
}

Result<PackageDiscovery> DiscoverPackageNear(const std::filesystem::path& folder, bool includeTorrents) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
    return Result<PackageDiscovery>::Error("Package folder does not exist: " + PathToUtf8(folder));
  }

  std::vector<std::filesystem::path> torrents;
  std::optional<std::filesystem::path> firstArchivePart;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec || !entry.is_regular_file()) {
      continue;
    }
    const auto path = entry.path();
    if (includeTorrents && HasExtension(path, ".torrent")) {
      torrents.push_back(path);
    } else if (!firstArchivePart.has_value() && IsFirstArchivePart(path)) {
      firstArchivePart = path;
    }
  }

  if (torrents.size() > 1) {
    return Result<PackageDiscovery>::Error("More than one .torrent file found. Pass the torrent path explicitly.");
  }

  PackageDiscovery package;
  if (!torrents.empty()) {
    package.torrentFile = torrents.front();
  }
  package.firstArchivePart = firstArchivePart;
  return Result<PackageDiscovery>::Ok(std::move(package));
}

Result<PackageDiscovery> DiscoverPackageFromTorrent(const std::filesystem::path& torrentFile) {
  std::error_code ec;
  if (!std::filesystem::exists(torrentFile, ec) || !std::filesystem::is_regular_file(torrentFile, ec)) {
    return Result<PackageDiscovery>::Error("Torrent file does not exist: " + PathToUtf8(torrentFile));
  }
  if (!HasExtension(torrentFile, ".torrent")) {
    return Result<PackageDiscovery>::Error("Expected a .torrent file: " + PathToUtf8(torrentFile));
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
