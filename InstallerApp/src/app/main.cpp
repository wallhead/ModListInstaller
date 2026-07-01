#include "app/PackageDiscovery.h"
#include "manifest/Manifest.h"
#include "paths/PathValidator.h"
#include "tracker/TrackerProvider.h"
#include "verifier/Verifier.h"

#include <filesystem>
#include <iostream>

using namespace modlist;

int main(int argc, char** argv) {
  if (argc < 2) {
    auto package = DiscoverPackageNear(std::filesystem::current_path());
    if (!package.ok()) {
      std::cout << "Usage:\n"
                << "  modlist-installer\n"
                << "  modlist-installer <package.torrent> [download-folder] [install-folder]\n"
                << "  modlist-installer <manifest.json> [download-folder] [install-folder]\n\n"
                << "No argument mode scans the current folder for exactly one .torrent file.\n"
                << "Auto-discovery error: " << package.error() << "\n";
      return 1;
    }
    std::cout << "Discovered torrent: " << package.value().torrentFile << "\n";
    if (package.value().firstArchivePart.has_value()) {
      std::cout << "Discovered archive first part: " << *package.value().firstArchivePart << "\n";
    } else {
      std::cout << "No .7z.001 archive part found next to the torrent yet.\n";
    }
    return 0;
  }

  const std::filesystem::path input = argv[1];
  if (input.extension() == ".torrent") {
    auto package = DiscoverPackageFromTorrent(input);
    if (!package.ok()) {
      std::cerr << "Package error: " << package.error() << "\n";
      return 1;
    }
    std::cout << "Using torrent: " << package.value().torrentFile << "\n";
    if (argc >= 3) {
      PathValidator validator;
      const auto download = validator.ValidateDownloadFolder(argv[2]);
      std::cout << "Download folder: " << download.message << "\n";
      if (!download.ok) {
        return 1;
      }
    }
    if (argc >= 4) {
      PathValidator validator;
      const auto install = validator.ValidateInstallFolder(argv[3]);
      std::cout << "Install folder: " << install.message << "\n";
      if (!install.ok) {
        return 1;
      }
    }
    if (package.value().firstArchivePart.has_value()) {
      std::cout << "Archive first part near torrent: " << *package.value().firstArchivePart << "\n";
    }
    std::cout << "Manifest was skipped, so SHA256 verification and extraction settings must be inferred or disabled.\n";
    return 0;
  }

  ManifestLoader loader;
  auto manifest = loader.LoadFromFile(input);
  if (!manifest.ok()) {
    std::cerr << "Manifest error: " << manifest.error() << "\n";
    return 1;
  }

  std::cout << "Loaded manifest version " << manifest.value().version << "\n";
  std::cout << "Files: " << manifest.value().files.size() << "\n";
  std::cout << "Tracker list: " << manifest.value().trackers.trackerListUrl << "\n";

  PathValidator validator(manifest.value().install.maxRecommendedPathLength);
  if (argc >= 3) {
    const auto download = validator.ValidateDownloadFolder(argv[2]);
    std::cout << "Download folder: " << download.message << "\n";
    if (!download.ok) {
      return 1;
    }
  }
  if (argc >= 4) {
    const auto install = validator.ValidateInstallFolder(argv[3]);
    std::cout << "Install folder: " << install.message << "\n";
    if (!install.ok) {
      return 1;
    }
  }

  return 0;
}
