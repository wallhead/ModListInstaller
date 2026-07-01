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

#include <openssl/evp.h>
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

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

int ValidationWorkerCount() {
  return BoundedWorkerCount(1, 2, 8);
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
  settings.set_int(lt::settings_pack::checking_mem_usage, 8192);
  settings.set_int(lt::settings_pack::max_queued_disk_bytes, 64 * 1024 * 1024);
  settings.set_int(lt::settings_pack::disk_io_read_mode, lt::settings_pack::disable_os_cache);
  settings.set_int(lt::settings_pack::disk_io_write_mode, lt::settings_pack::write_through);
  lt::session_params params(settings);
  params.disk_io_constructor = lt::posix_disk_io_constructor;
  return lt::session(std::move(params));
}

std::string SlicePath(const lt::torrent_info& info,
                      const lt::file_slice& slice,
                      const std::filesystem::path& root) {
  return info.files().file_path(slice.file_index, root.string());
}

class Sha1Hasher {
public:
  Sha1Hasher() : context_(EVP_MD_CTX_new()) {
    ok_ = context_ != nullptr && EVP_DigestInit_ex(context_, EVP_sha1(), nullptr) == 1;
  }

  ~Sha1Hasher() {
    EVP_MD_CTX_free(context_);
  }

  bool ok() const {
    return ok_;
  }

  bool Update(const void* data, size_t size) {
    ok_ = ok_ && EVP_DigestUpdate(context_, data, size) == 1;
    return ok_;
  }

  bool Final(lt::sha1_hash& hash) {
    unsigned char digest[lt::sha1_hash::size()]{};
    unsigned int length = 0;
    ok_ = ok_ && EVP_DigestFinal_ex(context_, digest, &length) == 1 && length == lt::sha1_hash::size();
    if (!ok_) {
      return false;
    }
    hash.assign(reinterpret_cast<const char*>(digest));
    return true;
  }

private:
  EVP_MD_CTX* context_{nullptr};
  bool ok_{false};
};

bool FeedZeros(Sha1Hasher& hasher, std::int64_t bytes) {
  constexpr int kChunkSize = 4 * 1024 * 1024;
  std::vector<char> zeros(kChunkSize, '\0');
  while (bytes > 0) {
    const int chunk = static_cast<int>(std::min<std::int64_t>(bytes, kChunkSize));
    if (!hasher.Update(zeros.data(), static_cast<size_t>(chunk))) {
      return false;
    }
    bytes -= chunk;
  }
  return true;
}

bool ValidatePiece(const lt::torrent_info& info,
                   int pieceIndex,
                   const std::filesystem::path& root,
                   std::string& error) {
  constexpr int kChunkSize = 4 * 1024 * 1024;
  const lt::piece_index_t piece(pieceIndex);
  const int pieceSize = info.piece_size(piece);
  const auto slices = info.map_block(piece, 0, pieceSize);
  std::vector<char> buffer(kChunkSize);
  Sha1Hasher hasher;
  if (!hasher.ok()) {
    error = "Unable to initialize SHA1 validator.";
    return false;
  }

  for (const auto& slice : slices) {
    if (info.files().pad_file_at(slice.file_index)) {
      if (!FeedZeros(hasher, slice.size)) {
        error = "Unable to hash torrent padding data.";
        return false;
      }
      continue;
    }

    const auto path = SlicePath(info, slice, root);
    std::ifstream file{std::filesystem::path(path), std::ios::binary};
    if (!file) {
      error = "Missing package file: " + path;
      return false;
    }
    file.seekg(slice.offset, std::ios::beg);
    if (!file) {
      error = "Unable to seek package file: " + path;
      return false;
    }

    std::int64_t remaining = slice.size;
    while (remaining > 0) {
      const std::streamsize chunk = static_cast<std::streamsize>(
          std::min<std::int64_t>(remaining, static_cast<std::int64_t>(buffer.size())));
      file.read(buffer.data(), chunk);
      const std::streamsize read = file.gcount();
      if (read <= 0) {
        std::ostringstream out;
        out << "Package file ended early while validating piece " << pieceIndex << ": " << path;
        error = out.str();
        return false;
      }
      if (!hasher.Update(buffer.data(), static_cast<size_t>(read))) {
        error = "Unable to hash package data.";
        return false;
      }
      remaining -= read;
    }
  }

  lt::sha1_hash actual;
  if (!hasher.Final(actual)) {
    error = "Unable to finish SHA1 validation.";
    return false;
  }
  if (actual != info.hash_for_piece(piece)) {
    std::ostringstream out;
    out << "Piece hash mismatch at piece " << pieceIndex << ".";
    error = out.str();
    return false;
  }

  return true;
}

}  // namespace

struct LibtorrentDownloader::Impl {
  lt::session session;
  lt::torrent_handle handle;
  bool recheckRequested{false};
  bool recheckObserved{false};
  bool validationOnly{false};
  std::thread validationThread;
  std::atomic_bool validationRunning{false};
  std::atomic_bool validationCancelRequested{false};
  std::atomic_bool validationPauseRequested{false};
  std::mutex validationPauseMutex;
  std::condition_variable validationPauseCv;

  Impl() : session(MakeSession()) {}
  ~Impl() {
    StopValidation();
    JoinValidation();
  }

  void StopValidation() {
    validationCancelRequested = true;
    validationPauseRequested = false;
    validationPauseCv.notify_all();
  }

  void JoinValidation() {
    if (validationThread.joinable()) {
      validationThread.join();
    }
  }
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
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_) {
    impl_->StopValidation();
    impl_->JoinValidation();
  }
#endif

  std::lock_guard<std::mutex> lock(mutex_);
  status_ = {};
  status_.stage = DownloadStage::Loading;
  status_.trackerCount = static_cast<int>(config.trackers.size());
  status_.dhtEnabled = config.features.enableDht;

#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_) {
    impl_->validationOnly = true;
    impl_->recheckRequested = false;
    impl_->recheckObserved = false;
    impl_->handle = lt::torrent_handle();
    impl_->validationCancelRequested = false;
    impl_->validationPauseRequested = false;
  }
  if (config.torrent.type == TorrentSourceType::Magnet) {
    SetFailure("Local validation requires a .torrent file.");
    return;
  }
  status_.stage = DownloadStage::Checking;
  status_.stateText = "Validating local files";
  status_.trackerCount = ValidationWorkerCount();
  impl_->validationRunning = true;
  impl_->validationThread = std::thread(&LibtorrentDownloader::RunLocalValidation, this, config);
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
  if (impl_) {
    impl_->StopValidation();
  }
#endif
  status_.stage = DownloadStage::Cancelled;
  status_.stateText = "Cancelled";
}

void LibtorrentDownloader::Pause() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->validationRunning) {
    impl_->validationPauseRequested = true;
    status_.stage = DownloadStage::Paused;
    status_.stateText = "Paused";
  } else if (impl_ && impl_->handle.is_valid()) {
    impl_->handle.pause();
    status_.stage = DownloadStage::Paused;
    status_.stateText = "Paused";
  }
#endif
}

void LibtorrentDownloader::Resume() {
  std::lock_guard<std::mutex> lock(mutex_);
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->validationRunning) {
    impl_->validationPauseRequested = false;
    impl_->validationPauseCv.notify_all();
    status_.stage = DownloadStage::Checking;
    status_.stateText = "Validating local files";
  } else if (impl_ && impl_->handle.is_valid()) {
    impl_->handle.resume();
    status_.stage = DownloadStage::Downloading;
    status_.stateText = "Downloading";
  }
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
#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_) {
    impl_->StopValidation();
    impl_->JoinValidation();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  impl_.reset();
  impl_ = std::make_unique<Impl>();
#else
  std::lock_guard<std::mutex> lock(mutex_);
#endif
}

void LibtorrentDownloader::RunLocalValidation(DownloadConfig config) {
#if defined(MODLIST_HAVE_LIBTORRENT)
  lt::error_code ec;
  lt::torrent_info info(config.torrent.source, ec);
  if (ec) {
    std::lock_guard<std::mutex> lock(mutex_);
    SetFailure("Unable to load torrent: " + ec.message());
    if (impl_) {
      impl_->validationRunning = false;
    }
    return;
  }

  const int totalPieces = info.num_pieces();
  const auto totalBytes = info.total_size();
  if (totalPieces <= 0 || totalBytes <= 0) {
    std::lock_guard<std::mutex> lock(mutex_);
    SetFailure("Torrent metadata reported an invalid payload.");
    if (impl_) {
      impl_->validationRunning = false;
    }
    return;
  }

  const int workerCount = std::min(ValidationWorkerCount(), totalPieces);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.stage = DownloadStage::Checking;
    status_.stateText = "Validating local files";
    status_.progress = 0.0f;
    status_.downloadedBytes = 0;
    status_.totalBytes = totalBytes;
    status_.trackerCount = workerCount;
  }

  std::atomic<int> nextPiece{0};
  std::atomic<std::int64_t> validatedBytes{0};
  std::atomic_bool failed{false};
  std::vector<std::thread> workers;
  workers.reserve(static_cast<size_t>(workerCount));

  auto failValidation = [&](const std::string& message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      std::lock_guard<std::mutex> lock(mutex_);
      SetFailure("Local package validation failed. " + message);
    }
    if (impl_) {
      impl_->validationCancelRequested = true;
      impl_->validationPauseRequested = false;
      impl_->validationPauseCv.notify_all();
    }
  };

  auto worker = [&]() {
    while (true) {
      if (impl_ == nullptr || impl_->validationCancelRequested || failed) {
        return;
      }
      {
        std::unique_lock<std::mutex> pauseLock(impl_->validationPauseMutex);
        impl_->validationPauseCv.wait(pauseLock, [&]() {
          return !impl_->validationPauseRequested || impl_->validationCancelRequested || failed.load();
        });
      }
      if (impl_->validationCancelRequested || failed) {
        return;
      }

      const int pieceIndex = nextPiece.fetch_add(1);
      if (pieceIndex >= totalPieces) {
        return;
      }

      std::string error;
      if (!ValidatePiece(info, pieceIndex, config.downloadFolder, error)) {
        failValidation(error);
        return;
      }

      const auto done = validatedBytes.fetch_add(info.piece_size(lt::piece_index_t(pieceIndex))) +
                        info.piece_size(lt::piece_index_t(pieceIndex));
      std::lock_guard<std::mutex> lock(mutex_);
      if (status_.stage == DownloadStage::Checking || status_.stage == DownloadStage::Paused) {
        status_.stage = impl_->validationPauseRequested ? DownloadStage::Paused : DownloadStage::Checking;
        status_.stateText = impl_->validationPauseRequested ? "Paused" : "Validating local files";
        status_.downloadedBytes = std::min<std::int64_t>(done, totalBytes);
        status_.totalBytes = totalBytes;
        status_.progress = static_cast<float>(
            static_cast<double>(status_.downloadedBytes) / static_cast<double>(totalBytes));
      }
    }
  };

  for (int i = 0; i < workerCount; ++i) {
    workers.emplace_back(worker);
  }
  for (auto& thread : workers) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  std::lock_guard<std::mutex> lock(mutex_);
  if (impl_) {
    impl_->validationRunning = false;
  }
  if (failed || status_.stage == DownloadStage::Failed) {
    return;
  }
  if (impl_ && impl_->validationCancelRequested) {
    status_.stage = DownloadStage::Cancelled;
    status_.stateText = "Cancelled";
    return;
  }

  status_.stage = DownloadStage::Completed;
  status_.stateText = "Validated";
  status_.progress = 1.0f;
  status_.downloadedBytes = totalBytes;
  status_.totalBytes = totalBytes;
  status_.etaSeconds = 0;
#else
  (void)config;
#endif
}

DownloadStatus LibtorrentDownloader::GetStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  DownloadStatus current = status_;

#if defined(MODLIST_HAVE_LIBTORRENT)
  if (impl_ && impl_->handle.is_valid() && !impl_->validationOnly) {
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
