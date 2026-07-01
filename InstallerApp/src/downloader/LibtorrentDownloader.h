#pragma once

#include "downloader/ITorrentDownloader.h"
#include "common/Result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>

namespace modlist {

class LibtorrentDownloader : public ITorrentDownloader {
public:
  LibtorrentDownloader();
  ~LibtorrentDownloader() override;

  void Start(const DownloadConfig& config) override;
  void StartLocalValidation(const DownloadConfig& config);
  void Cancel() override;
  void Pause() override;
  void Resume() override;
  void ForceRecheck();
  void ReleaseFiles();
  DownloadStatus GetStatus() const override;

  static Result<uintmax_t> ReadTorrentPayloadSize(const std::filesystem::path& torrentFile);

private:
  void SetFailure(const std::string& message);

  mutable std::mutex mutex_;
  DownloadStatus status_;

#if defined(MODLIST_HAVE_LIBTORRENT)
  struct Impl;
  std::unique_ptr<Impl> impl_;
#endif
};

}  // namespace modlist
