#pragma once

#include "manifest/Manifest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace modlist {

struct DownloadConfig {
  TorrentSource torrent;
  std::filesystem::path downloadFolder;
  std::vector<std::string> trackers;
  TorrentFeatures features;
};

enum class DownloadStage {
  Idle,
  Loading,
  Downloading,
  Checking,
  Seeding,
  Completed,
  Paused,
  Cancelled,
  Failed,
};

struct DownloadStatus {
  DownloadStage stage{DownloadStage::Idle};
  float progress{0.0f};
  int downloadRateBytesPerSecond{0};
  int uploadRateBytesPerSecond{0};
  int peerCount{0};
  int seedCount{0};
  int trackerCount{0};
  int64_t downloadedBytes{0};
  int64_t totalBytes{0};
  int etaSeconds{-1};
  bool dhtEnabled{false};
  std::string stateText;
  std::string error;
};

class ITorrentDownloader {
public:
  virtual ~ITorrentDownloader() = default;
  virtual void Start(const DownloadConfig& config) = 0;
  virtual void Cancel() = 0;
  virtual void Pause() = 0;
  virtual void Resume() = 0;
  virtual DownloadStatus GetStatus() const = 0;
};

}  // namespace modlist
