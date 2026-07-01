#include "downloader/LibtorrentDownloader.h"

#if defined(MODLIST_HAVE_LIBTORRENT)
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/posix_disk_io.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#endif

#include <algorithm>
#include <thread>

namespace modlist {

#if defined(MODLIST_HAVE_LIBTORRENT)
namespace lt = libtorrent;

namespace {

int BoundedWorkerCount(unsigned int reserveForUi = 1, int minimum = 2, int maximum = 4) {
  const unsigned int detected = std::thread::hardware_concurrency();
  if (detected <= reserveForUi) {
    return minimum;
  }
  return std::clamp(static_cast<int>(detected - reserveForUi), minimum, maximum);
}

lt::session MakeSession() {
  lt::settings_pack settings;
  settings.set_bool(lt::settings_pack::enable_dht, true);
  settings.set_bool(lt::settings_pack::enable_lsd, true);
  settings.set_int(lt::settings_pack::active_downloads, 1);
  settings.set_int(lt::settings_pack::active_limit, 1);
  settings.set_int(lt::settings_pack::connections_limit, 80);
  settings.set_int(lt::settings_pack::aio_threads, BoundedWorkerCount(2, 2, 4));
  settings.set_int(lt::settings_pack::hashing_threads, BoundedWorkerCount(1, 2, 4));
  settings.set_int(lt::settings_pack::checking_mem_usage, 4096);
  settings.set_int(lt::settings_pack::max_queued_disk_bytes, 32 * 1024 * 1024);
  settings.set_int(lt::settings_pack::disk_io_read_mode, lt::settings_pack::disable_os_cache);
  settings.set_int(lt::settings_pack::disk_io_write_mode, lt::settings_pack::write_through);
  lt::session_params params(settings);
  params.disk_io_constructor = lt::posix_disk_io_constructor;
  return lt::session(std::move(params));
}

}  // namespace

struct LibtorrentDownloader::Impl {
  lt::session session;
  lt::torrent_handle handle;
  bool recheckRequested{false};
  bool recheckObserved{false};
  bool validationOnly{false};

  Impl() : session(MakeSession()) {}
};
#endif

LibtorrentDownloader::LibtorrentDownloader() {
#if defined(MODLIST_HAVE_LIBTORRENT)
  impl_ = std::make_unique<Impl>();
#endif
}

LibtorrentDownloader::~LibtorrentDownloader() = default;

Result<uintmax_t> LibtorrentDownloader::ReadTorrentPayloadSize(const std::filesystem::path& torrentFile) {
#if defined(MODLIST_HAVE_LIBTORRENT)
  lt::error_code ec;
  lt::torrent_info info(torrentFile.string(), ec);
  if (ec) {
    return Result<uintmax_t>::Error("Unable to read torrent metadata: " + ec.message());
  }
  const auto total = info.total_size();
  if (total < 0) {
    return Result<uintmax_t>::Error("Torrent metadata reported an invalid payload size.");
  }
  return Result<uintmax_t>::Ok(static_cast<uintmax_t>(total));
#else
  (void)torrentFile;
  return Result<uintmax_t>::Error("libtorrent-rasterbar backend is not enabled.");
#endif
}

void LibtorrentDownloader::Start(const DownloadConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = {};
  status_.stage = DownloadStage::Loading;
  status_.trackerCount = static_cast<int>(config.trackers.size());
  status_.dhtEnabled = config.features.enableDht;

#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_) {
    impl_->validationOnly = false;
  }
  lt::add_torrent_params params;
  params.save_path = config.downloadFolder.string();
  params.trackers = config.trackers;

  lt::error_code ec;
  if (config.torrent.type == TorrentSourceType::Magnet) {
    params = lt::parse_magnet_uri(config.torrent.source, ec);
    params.save_path = config.downloadFolder.string();
    params.trackers.insert(params.trackers.end(), config.trackers.begin(), config.trackers.end());
  } else {
    params.ti = std::make_shared<lt::torrent_info>(config.torrent.source, ec);
  }
  if (ec) {
    SetFailure("Unable to load torrent: " + ec.message());
    return;
  }

  impl_->handle = impl_->session.add_torrent(params, ec);
  if (ec) {
    SetFailure("Unable to start torrent: " + ec.message());
    return;
  }
  impl_->handle.force_recheck();
  status_.stage = DownloadStage::Checking;
  status_.stateText = "Checking existing files";
#else
  (void)config;
  SetFailure("libtorrent-rasterbar backend is not enabled. Configure with -DMODLIST_USE_LIBTORRENT=ON after installing libtorrent.");
#endif
}

void LibtorrentDownloader::StartLocalValidation(const DownloadConfig& config) {
  std::lock_guard<std::mutex> lock(mutex_);
  status_ = {};
  status_.stage = DownloadStage::Loading;
  status_.trackerCount = static_cast<int>(config.trackers.size());
  status_.dhtEnabled = config.features.enableDht;

#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_) {
    impl_->validationOnly = true;
  }
  lt::add_torrent_params params;
  params.save_path = config.downloadFolder.string();
  params.trackers = config.trackers;

  lt::error_code ec;
  if (config.torrent.type == TorrentSourceType::Magnet) {
    params = lt::parse_magnet_uri(config.torrent.source, ec);
    params.save_path = config.downloadFolder.string();
    params.trackers.insert(params.trackers.end(), config.trackers.begin(), config.trackers.end());
  } else {
    params.ti = std::make_shared<lt::torrent_info>(config.torrent.source, ec);
  }
  params.flags |= lt::torrent_flags::paused;
  if (ec) {
    SetFailure("Unable to load torrent: " + ec.message());
    return;
  }

  impl_->handle = impl_->session.add_torrent(params, ec);
  if (ec) {
    SetFailure("Unable to start torrent validation: " + ec.message());
    return;
  }
  impl_->handle.force_recheck();
  impl_->recheckRequested = true;
  impl_->recheckObserved = false;
  status_.stage = DownloadStage::Checking;
  status_.stateText = "Validating local files";
#else
  (void)config;
  SetFailure("libtorrent-rasterbar backend is not enabled. Configure with -DMODLIST_USE_LIBTORRENT=ON after installing libtorrent.");
#endif
}

void LibtorrentDownloader::Cancel() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->handle.is_valid()) {
    impl_->session.remove_torrent(impl_->handle);
  }
#endif
  status_.stage = DownloadStage::Cancelled;
  status_.stateText = "Cancelled";
}

void LibtorrentDownloader::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->handle.is_valid()) {
    impl_->handle.pause();
  }
#endif
  status_.stage = DownloadStage::Paused;
  status_.stateText = "Paused";
}

void LibtorrentDownloader::Resume() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->handle.is_valid()) {
    impl_->handle.resume();
  }
  status_.stage = DownloadStage::Downloading;
  status_.stateText = "Downloading";
#else
  SetFailure("libtorrent-rasterbar backend is not enabled.");
#endif
}

void LibtorrentDownloader::ForceRecheck() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->handle.is_valid()) {
    impl_->handle.force_recheck();
    impl_->recheckRequested = true;
    impl_->recheckObserved = false;
    status_.stage = DownloadStage::Checking;
    status_.stateText = "Validating downloaded files";
  }
#else
  SetFailure("libtorrent-rasterbar backend is not enabled.");
#endif
}

void LibtorrentDownloader::ReleaseFiles() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(MODLIST_HAVE_LIBTORRENT)
  impl_.reset();
  impl_ = std::make_unique<Impl>();
#endif
}

DownloadStatus LibtorrentDownloader::GetStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  DownloadStatus current = status_;

#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->handle.is_valid()) {
    const auto s = impl_->handle.status();
    current.progress = s.progress;
    current.downloadRateBytesPerSecond = s.download_rate;
    current.uploadRateBytesPerSecond = s.upload_rate;
    current.peerCount = s.num_peers;
    current.seedCount = s.num_seeds;
    current.trackerCount = static_cast<int>(impl_->handle.trackers().size());
    current.downloadedBytes = s.total_wanted_done;
    current.totalBytes = s.total_wanted;
    if (current.downloadRateBytesPerSecond > 0 && current.totalBytes > current.downloadedBytes) {
      current.etaSeconds = static_cast<int>((current.totalBytes - current.downloadedBytes) / current.downloadRateBytesPerSecond);
    }
    const bool checking = s.state == lt::torrent_status::checking_files ||
                          s.state == lt::torrent_status::checking_resume_data;
    const bool complete = s.is_seeding || s.progress >= 1.0f;

    if (s.errc) {
      current.stage = DownloadStage::Failed;
      current.error = s.errc.message();
      current.stateText = "Failed";
    } else if (impl_->recheckRequested && checking) {
      impl_->recheckObserved = true;
      current.stage = DownloadStage::Checking;
      current.stateText = impl_->validationOnly ? "Validating local files" : "Validating downloaded files";
    } else if (impl_->recheckRequested && complete) {
      impl_->recheckRequested = false;
      impl_->recheckObserved = false;
      current.stage = DownloadStage::Completed;
      current.stateText = "Validated";
    } else if (impl_->validationOnly && impl_->recheckRequested && !checking && !complete) {
      current.stage = DownloadStage::Failed;
      current.error = "Local package validation failed. Archive files are missing or incomplete.";
      current.stateText = "Failed";
    } else if (impl_->recheckRequested && !impl_->recheckObserved) {
      current.stage = DownloadStage::Checking;
      current.stateText = "Starting validation";
    } else if (checking) {
      current.stage = DownloadStage::Checking;
      current.stateText = "Checking existing files";
    } else if (complete) {
      current.stage = DownloadStage::Completed;
      current.stateText = "Completed";
    } else if (static_cast<bool>(s.flags & lt::torrent_flags::paused)) {
      current.stage = DownloadStage::Paused;
      current.stateText = "Paused";
    } else {
      current.stage = DownloadStage::Downloading;
      current.stateText = "Downloading";
    }
  }
#endif

  return current;
}

void LibtorrentDownloader::SetFailure(const std::string& message) {
  status_.stage = DownloadStage::Failed;
  status_.error = message;
  status_.stateText = "Failed";
}

}  // namespace modlist
