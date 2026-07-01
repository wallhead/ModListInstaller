#pragma once

#include "common/Result.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace modlist {

enum class TorrentSourceType {
  Magnet,
  TorrentFile,
};

struct TorrentSource {
  TorrentSourceType type{TorrentSourceType::Magnet};
  std::string source;
};

struct TrackerSettings {
  bool useOnlineTrackerList{true};
  std::string trackerListUrl{"https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_all.txt"};
  bool continueIfTrackerListFails{true};
};

struct TorrentFeatures {
  bool enableDht{true};
  bool enablePex{true};
  bool enableLsd{true};
};

struct InstallSettings {
  bool askUser{true};
  bool preferShortRootPath{true};
  std::string examplePath{"D:\\Sky"};
  size_t maxRecommendedPathLength{20};
  bool sameDiskTempOnly{true};
};

struct ManifestFile {
  std::filesystem::path path;
  uint64_t size{0};
  std::string sha256;
};

struct ExtractSettings {
  std::filesystem::path firstArchivePart;
  std::filesystem::path targetSubfolder;
  bool cleanupAfterSuccess{false};
  bool useSameDiskTemp{true};
};

struct Manifest {
  std::string version;
  TorrentSource torrent;
  TrackerSettings trackers;
  TorrentFeatures torrentFeatures;
  InstallSettings install;
  std::vector<ManifestFile> files;
  ExtractSettings extract;
};

class ManifestLoader {
public:
  Result<Manifest> LoadFromFile(const std::filesystem::path& path) const;
  Result<Manifest> LoadFromString(const std::string& json) const;
};

bool IsSafeManifestRelativePath(const std::filesystem::path& path);

}  // namespace modlist
