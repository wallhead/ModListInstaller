#include "extractor/PipelinedSevenZipExtractor.h"

#include "extractor/SevenZipSdkInterfaces.h"
#include "manifest/Manifest.h"
#include "resource.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace modlist {

namespace {

using namespace sevenzip_sdk;

constexpr size_t kReadAheadBlocks = 4;
constexpr size_t kMaxCachedBlocks = 6;
constexpr uint64_t kMaxPipelineChunkSize = 256ull * 1024ull * 1024ull;

template <typename T>
class ComPtr {
public:
  ComPtr() = default;
  explicit ComPtr(T* value) : value_(value) {}
  ComPtr(const ComPtr&) = delete;
  ComPtr& operator=(const ComPtr&) = delete;
  ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      Reset();
      value_ = std::exchange(other.value_, nullptr);
    }
    return *this;
  }
  ~ComPtr() { Reset(); }

  T* Get() const { return value_; }
  T* operator->() const { return value_; }
  explicit operator bool() const { return value_ != nullptr; }
  void Reset(T* value = nullptr) {
    if (value_ != nullptr) {
      value_->Release();
    }
    value_ = value;
  }

private:
  T* value_{nullptr};
};

class ComRefCount {
protected:
  ULONG AddRefImpl() { return ++references_; }
  ULONG ReleaseImpl() {
    const ULONG remaining = --references_;
    if (remaining == 0) {
      delete this;
    }
    return remaining;
  }
  virtual ~ComRefCount() = default;

private:
  std::atomic<ULONG> references_{1};
};

bool EqualInterface(REFIID left, const GUID& right) {
  return IsEqualIID(left, right) != FALSE;
}

std::string HexDigest(const std::vector<uint8_t>& digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const uint8_t byte : digest) {
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

std::string PathToUtf8(const std::filesystem::path& path) {
  const auto utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

std::string WindowsErrorMessage(DWORD error) {
  LPWSTR raw = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, error, 0, reinterpret_cast<LPWSTR>(&raw), 0, nullptr);
  std::wstring message = length > 0 && raw != nullptr ? std::wstring(raw, length) : L"Windows error";
  if (raw != nullptr) {
    LocalFree(raw);
  }
  while (!message.empty() &&
         (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
    message.pop_back();
  }
  const int utf8Length = WideCharToMultiByte(
      CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(std::max(0, utf8Length)), '\0');
  if (utf8Length > 0) {
    WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()),
                        utf8.data(), utf8Length, nullptr, nullptr);
  }
  return utf8 + " (" + std::to_string(error) + ")";
}

std::filesystem::path ModuleFolder() {
  std::wstring buffer(MAX_PATH, L'\0');
  while (true) {
    const DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return std::filesystem::current_path();
    }
    if (size < buffer.size() - 1) {
      buffer.resize(size);
      return std::filesystem::path(buffer).parent_path();
    }
    buffer.resize(buffer.size() * 2);
  }
}

Result<std::filesystem::path> ExtractEmbeddedLibrary(const std::filesystem::path& appRoot) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(
      module, MAKEINTRESOURCEW(IDR_7ZIP_DLL), MAKEINTRESOURCEW(10));
  if (resource == nullptr) {
    return Result<std::filesystem::path>::Error("Embedded 7-Zip SDK library was not found");
  }
  HGLOBAL loaded = LoadResource(module, resource);
  const DWORD size = SizeofResource(module, resource);
  const void* data = LockResource(loaded);
  if (loaded == nullptr || data == nullptr || size == 0) {
    return Result<std::filesystem::path>::Error("Embedded 7-Zip SDK library could not be loaded");
  }

  const auto folder = appRoot / "data" / "tools" / "7zip";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (ec) {
    return Result<std::filesystem::path>::Error(
        "Unable to create 7-Zip SDK cache folder: " + ec.message());
  }
  const auto output = folder / "7z.dll";
  bool write = true;
  if (std::filesystem::exists(output, ec) && !ec) {
    write = std::filesystem::file_size(output, ec) != size || ec;
  }
  if (write) {
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return Result<std::filesystem::path>::Error("Unable to write embedded 7z.dll");
    }
    stream.write(static_cast<const char*>(data), size);
    if (!stream) {
      return Result<std::filesystem::path>::Error("Unable to finish writing embedded 7z.dll");
    }
  }

  HRSRC licenseResource = FindResourceW(
      module, MAKEINTRESOURCEW(IDR_7ZIP_LICENSE), MAKEINTRESOURCEW(10));
  if (licenseResource != nullptr) {
    HGLOBAL licenseLoaded = LoadResource(module, licenseResource);
    const DWORD licenseSize = SizeofResource(module, licenseResource);
    const void* licenseData = LockResource(licenseLoaded);
    if (licenseLoaded != nullptr && licenseData != nullptr && licenseSize > 0) {
      std::ofstream license(folder / "License.txt", std::ios::binary | std::ios::trunc);
      if (license) {
        license.write(static_cast<const char*>(licenseData), licenseSize);
      }
    }
  }
  return Result<std::filesystem::path>::Ok(output);
}

bool EndsWithDigits(const std::string& value) {
  return value.size() >= 3 &&
         std::all_of(value.end() - 3, value.end(), [](unsigned char c) {
           return c >= '0' && c <= '9';
         });
}

bool IsSafeOutputPath(const std::filesystem::path& path) {
  if (!IsSafeManifestRelativePath(path)) {
    return false;
  }
  for (const auto& part : path) {
    const auto utf8 = part.u8string();
    const std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
    if (!IsSafeArchiveFolderName(name)) {
      return false;
    }
  }
  return true;
}

std::vector<const ManifestFile*> ArchiveVolumes(const Manifest& manifest) {
  std::vector<const ManifestFile*> volumes;
  const std::string first = manifest.extract.firstArchivePart.generic_string();
  if (first.size() >= 4 && first[first.size() - 4] == '.' && EndsWithDigits(first)) {
    const std::string prefix = first.substr(0, first.size() - 3);
    for (const auto& file : manifest.files) {
      const std::string path = file.path.generic_string();
      if (path.size() == prefix.size() + 3 && path.starts_with(prefix) && EndsWithDigits(path)) {
        volumes.push_back(&file);
      }
    }
    std::sort(volumes.begin(), volumes.end(), [](const auto* left, const auto* right) {
      return left->path.generic_string() < right->path.generic_string();
    });
    for (size_t index = 0; index < volumes.size(); ++index) {
      const std::string path = volumes[index]->path.generic_string();
      const unsigned int number = static_cast<unsigned int>(std::stoul(path.substr(path.size() - 3)));
      if (number != index + 1) {
        return {};
      }
    }
    return volumes;
  }

  for (const auto& file : manifest.files) {
    if (file.path == manifest.extract.firstArchivePart) {
      volumes.push_back(&file);
      break;
    }
  }
  return volumes;
}

class ProgressReporter {
public:
  ProgressReporter(uint64_t validationTotal,
                   PipelinedSevenZipExtractor::ProgressCallback callback)
      : callback_(std::move(callback)) {
    progress_.validationTotalBytes = validationTotal;
  }

  void SetValidated(uint64_t bytes,
                    std::filesystem::path archive,
                    size_t archiveIndex,
                    size_t archiveCount,
                    uint64_t archiveBytes,
                    uint64_t archiveTotalBytes) {
    PipelineExtractionProgress snapshot;
    {
      std::lock_guard lock(mutex_);
      progress_.validatedBytes = bytes;
      progress_.validationArchive = std::move(archive);
      progress_.validationArchiveIndex = archiveIndex;
      progress_.validationArchiveCount = archiveCount;
      progress_.validationArchiveBytes = archiveBytes;
      progress_.validationArchiveTotalBytes = archiveTotalBytes;
      snapshot = progress_;
    }
    Notify(snapshot);
  }

  void SetExtractionArchive(std::filesystem::path archive,
                            size_t archiveIndex,
                            size_t archiveCount) {
    PipelineExtractionProgress snapshot;
    {
      std::lock_guard lock(mutex_);
      if (progress_.extractionArchiveIndex == archiveIndex &&
          progress_.extractionArchiveCount == archiveCount) {
        return;
      }
      progress_.extractionArchive = std::move(archive);
      progress_.extractionArchiveIndex = archiveIndex;
      progress_.extractionArchiveCount = archiveCount;
      snapshot = progress_;
    }
    Notify(snapshot);
  }

  void SetExtractionTotal(uint64_t total) {
    std::lock_guard lock(mutex_);
    extractionTotal_ = total;
  }

  void SetExtractionCompleted(uint64_t completed) {
    PipelineExtractionProgress snapshot;
    {
      std::lock_guard lock(mutex_);
      progress_.extractionPercent = extractionTotal_ > 0
          ? static_cast<int>(std::min<uint64_t>(100, completed * 100 / extractionTotal_))
          : progress_.extractionPercent;
      snapshot = progress_;
    }
    Notify(snapshot);
  }

  void FinishExtraction() {
    PipelineExtractionProgress snapshot;
    {
      std::lock_guard lock(mutex_);
      progress_.extractionPercent = 100;
      snapshot = progress_;
    }
    Notify(snapshot);
  }

private:
  void Notify(const PipelineExtractionProgress& snapshot) {
    if (callback_) {
      std::lock_guard lock(callbackMutex_);
      callback_(snapshot);
    }
  }

  std::mutex mutex_;
  PipelineExtractionProgress progress_;
  uint64_t extractionTotal_{0};
  PipelinedSevenZipExtractor::ProgressCallback callback_;
  std::mutex callbackMutex_;
};

struct VolumeHandle {
  std::filesystem::path path;
  uint64_t size{0};
  HANDLE handle{INVALID_HANDLE_VALUE};
};

struct ChunkLocation {
  size_t volumeIndex{0};
  size_t fileChunkIndex{0};
  uint64_t fileOffset{0};
  uint64_t virtualOffset{0};
  uint32_t size{0};
  std::string expectedHash;
};

struct CachedBlock {
  std::vector<uint8_t> data;
  uint64_t accessStamp{0};
};

class VerifiedArchiveStream final : public IInStream, private ComRefCount {
public:
  VerifiedArchiveStream(const PipelinedExtractionConfig& config,
                        std::shared_ptr<ProgressReporter> reporter)
      : cancelRequested_(config.cancelRequested), reporter_(std::move(reporter)) {
    const auto manifestVolumes = ArchiveVolumes(*config.manifest);
    uint64_t virtualOffset = 0;
    for (const ManifestFile* file : manifestVolumes) {
      VolumeHandle volume;
      volume.path = config.archiveFolder / file->path;
      volume.size = file->size;
      volume.handle = CreateFileW(
          volume.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OVERLAPPED, nullptr);
      if (volume.handle == INVALID_HANDLE_VALUE) {
        error_ = "Unable to open archive volume " + volume.path.string() + ": " +
                 WindowsErrorMessage(GetLastError());
        break;
      }
      LARGE_INTEGER actualSize{};
      if (!GetFileSizeEx(volume.handle, &actualSize) ||
          static_cast<uint64_t>(actualSize.QuadPart) != volume.size) {
        error_ = "Archive volume size mismatch: " + volume.path.string();
        CloseHandle(volume.handle);
        volume.handle = INVALID_HANDLE_VALUE;
        break;
      }
      volumes_.push_back(std::move(volume));

      for (size_t index = 0; index < file->chunkSha256.size(); ++index) {
        const uint64_t fileOffset = index * config.manifest->hashChunkSize;
        const uint64_t remaining = file->size - fileOffset;
        const uint64_t chunkBytes = std::min(config.manifest->hashChunkSize, remaining);
        ChunkLocation chunk;
        chunk.volumeIndex = volumes_.size() - 1;
        chunk.fileChunkIndex = index;
        chunk.fileOffset = fileOffset;
        chunk.virtualOffset = virtualOffset + fileOffset;
        chunk.size = static_cast<uint32_t>(chunkBytes);
        chunk.expectedHash = file->chunkSha256[index];
        chunks_.push_back(std::move(chunk));
      }
      virtualOffset += file->size;
    }
    totalSize_ = virtualOffset;
    verified_.resize(chunks_.size(), false);
    validatedVolumeBytes_.resize(volumes_.size(), 0);
    if (error_.empty()) {
      worker_ = std::thread(&VerifiedArchiveStream::WorkerLoop, this);
    }
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (EqualInterface(iid, IID_IUnknown) || EqualInterface(iid, kIidSequentialInStream) ||
        EqualInterface(iid, kIidInStream)) {
      *object = static_cast<IInStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }

  HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processedSize) override {
    std::lock_guard streamLock(streamMutex_);
    ++readCalls_;
    lastReadStart_ = position_;
    lastReadRequest_ = size;
    if (processedSize == nullptr) {
      return E_POINTER;
    }
    *processedSize = 0;
    if (size == 0 || position_ >= totalSize_) {
      return S_OK;
    }
    auto* destination = static_cast<uint8_t*>(data);
    while (*processedSize < size && position_ < totalSize_) {
      if (IsCancelled()) {
        return E_ABORT;
      }
      const auto chunkIndex = FindChunk(position_);
      if (!chunkIndex.has_value()) {
        SetError("Unable to map archive stream position to a manifest chunk");
        return E_FAIL;
      }
      auto block = GetBlock(*chunkIndex);
      if (!block) {
        return IsCancelled() ? E_ABORT : HRESULT_FROM_WIN32(ERROR_CRC);
      }
      const auto& chunk = chunks_[*chunkIndex];
      reporter_->SetExtractionArchive(
          volumes_[chunk.volumeIndex].path.filename(), chunk.volumeIndex + 1, volumes_.size());
      const uint64_t offset = position_ - chunk.virtualOffset;
      const UInt32 available = static_cast<UInt32>(chunk.size - offset);
      const UInt32 requested = size - *processedSize;
      const UInt32 copyBytes = std::min(available, requested);
      std::memcpy(destination + *processedSize, block->data.data() + offset, copyBytes);
      position_ += copyBytes;
      *processedSize += copyBytes;
    }
    bytesServed_ += *processedSize;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 seekOrigin, UInt64* newPosition) override {
    std::lock_guard streamLock(streamMutex_);
    ++seekCalls_;
    lastSeekOffset_ = offset;
    lastSeekOrigin_ = seekOrigin;
    Int64 base = 0;
    if (seekOrigin == STREAM_SEEK_SET) {
      base = 0;
    } else if (seekOrigin == STREAM_SEEK_CUR) {
      base = static_cast<Int64>(position_);
    } else if (seekOrigin == STREAM_SEEK_END) {
      base = static_cast<Int64>(totalSize_);
    } else {
      return STG_E_INVALIDFUNCTION;
    }
    if ((offset < 0 && base < -offset) ||
        (offset > 0 && base > std::numeric_limits<Int64>::max() - offset)) {
      return HRESULT_FROM_WIN32(ERROR_NEGATIVE_SEEK);
    }
    const Int64 next = base + offset;
    if (next < 0) {
      return HRESULT_FROM_WIN32(ERROR_NEGATIVE_SEEK);
    }
    position_ = static_cast<UInt64>(next);
    if (newPosition != nullptr) {
      *newPosition = position_;
    }
    return S_OK;
  }

  std::string Diagnostics() const {
    std::lock_guard streamLock(streamMutex_);
    std::ostringstream out;
    out << "reads=" << readCalls_ << ", seeks=" << seekCalls_
        << ", bytes served=" << bytesServed_ << ", position=" << position_
        << ", last read=" << lastReadStart_ << "+" << lastReadRequest_
        << ", last seek=" << lastSeekOffset_ << "/" << lastSeekOrigin_;
    return out.str();
  }

  bool Ready() const {
    std::lock_guard lock(mutex_);
    return error_.empty() && !volumes_.empty() && !chunks_.empty();
  }

  bool VerifyAll() {
    std::unique_lock lock(mutex_);
    if (!error_.empty()) {
      return false;
    }
    for (size_t index = 0; index < chunks_.size(); ++index) {
      if (!verified_[index]) {
        QueueLocked(index, false);
      }
    }
    condition_.notify_all();
    condition_.wait(lock, [&]() {
      return !error_.empty() || IsCancelled() || verifiedCount_ == chunks_.size();
    });
    return error_.empty() && !IsCancelled() && verifiedCount_ == chunks_.size();
  }

  std::string Error() const {
    std::lock_guard lock(mutex_);
    return error_;
  }

private:
  ~VerifiedArchiveStream() override {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    condition_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    for (auto& volume : volumes_) {
      if (volume.handle != INVALID_HANDLE_VALUE) {
        CloseHandle(volume.handle);
      }
    }
  }

  bool IsCancelled() const {
    return cancelRequested_ != nullptr && cancelRequested_->load();
  }

  void SetError(std::string message) {
    {
      std::lock_guard lock(mutex_);
      if (error_.empty()) {
        error_ = std::move(message);
      }
    }
    condition_.notify_all();
  }

  std::optional<size_t> FindChunk(uint64_t position) const {
    const auto iterator = std::upper_bound(
        chunks_.begin(), chunks_.end(), position,
        [](uint64_t value, const ChunkLocation& chunk) {
          return value < chunk.virtualOffset;
        });
    if (iterator == chunks_.begin()) {
      return std::nullopt;
    }
    const size_t index = static_cast<size_t>((iterator - chunks_.begin()) - 1);
    const auto& chunk = chunks_[index];
    if (position >= chunk.virtualOffset + chunk.size) {
      return std::nullopt;
    }
    return index;
  }

  void QueueLocked(size_t index, bool priority) {
    if (index >= chunks_.size() || cache_.contains(index) || queued_.contains(index)) {
      return;
    }
    queued_.insert(index);
    if (priority) {
      tasks_.push_front(index);
    } else {
      tasks_.push_back(index);
    }
  }

  std::shared_ptr<CachedBlock> GetBlock(size_t index) {
    std::unique_lock lock(mutex_);
    requestedChunk_ = index;
    auto cached = cache_.find(index);
    if (cached != cache_.end()) {
      cached->second->accessStamp = ++accessStamp_;
      return cached->second;
    }
    QueueLocked(index, true);
    for (size_t ahead = 1; ahead <= kReadAheadBlocks && index + ahead < chunks_.size(); ++ahead) {
      QueueLocked(index + ahead, false);
    }
    condition_.notify_all();
    condition_.wait(lock, [&]() {
      return cache_.contains(index) || !error_.empty() || IsCancelled();
    });
    if (!error_.empty() || IsCancelled()) {
      return {};
    }
    auto result = cache_.at(index);
    result->accessStamp = ++accessStamp_;
    return result;
  }

  std::shared_ptr<CachedBlock> LoadBlock(size_t index,
                                        BCRYPT_ALG_HANDLE algorithm,
                                        bool verifyHash,
                                        const std::array<HANDLE, 2>& readEvents,
                                        std::string& error) {
    const auto& chunk = chunks_[index];
    auto block = std::make_shared<CachedBlock>();
    block->data.resize(chunk.size);
    const HANDLE file = volumes_[chunk.volumeIndex].handle;

    DWORD objectLength = 0;
    DWORD returned = 0;
    std::vector<uint8_t> hashObject;
    std::vector<uint8_t> digest(32);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (verifyHash) {
      if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                            reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                            &returned, 0) != 0) {
        error = "Unable to query Windows SHA-256 provider";
        return {};
      }
      hashObject.resize(objectLength);
      if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength,
                           nullptr, 0, 0) != 0) {
        error = "Unable to initialize SHA-256 for archive block";
        return {};
      }
    }

    struct ReadSlot {
      OVERLAPPED overlapped{};
      size_t offset{0};
      DWORD requested{0};
      bool active{false};
    };
    std::array<ReadSlot, 2> slots;
    for (size_t slot = 0; slot < slots.size(); ++slot) {
      slots[slot].overlapped.hEvent = readEvents[slot];
    }

    auto cancelActive = [&]() {
      for (auto& slot : slots) {
        if (slot.active) {
          CancelIoEx(file, &slot.overlapped);
          WaitForSingleObject(slot.overlapped.hEvent, INFINITE);
          slot.active = false;
        }
      }
    };
    auto issueRead = [&](size_t slotIndex, size_t offset) -> bool {
      auto& slot = slots[slotIndex];
      slot.overlapped = {};
      slot.overlapped.hEvent = readEvents[slotIndex];
      slot.offset = offset;
      slot.requested = static_cast<DWORD>(std::min<size_t>(
          block->data.size() - offset, 8 * 1024 * 1024));
      const uint64_t absoluteOffset = chunk.fileOffset + offset;
      slot.overlapped.Offset = static_cast<DWORD>(absoluteOffset & 0xffffffffu);
      slot.overlapped.OffsetHigh = static_cast<DWORD>(absoluteOffset >> 32);
      ResetEvent(slot.overlapped.hEvent);
      const BOOL started = ReadFile(file, block->data.data() + offset,
                                    slot.requested, nullptr, &slot.overlapped);
      if (!started && GetLastError() != ERROR_IO_PENDING) {
        error = "Unable to start archive read for " +
                volumes_[chunk.volumeIndex].path.string() + ": " +
                WindowsErrorMessage(GetLastError());
        return false;
      }
      slot.active = true;
      return true;
    };
    auto waitRead = [&](size_t slotIndex) -> bool {
      auto& slot = slots[slotIndex];
      while (WaitForSingleObject(slot.overlapped.hEvent, 100) == WAIT_TIMEOUT) {
        if (IsCancelled()) {
          error = "Validation was cancelled";
          cancelActive();
          return false;
        }
      }
      DWORD read = 0;
      if (!GetOverlappedResult(file, &slot.overlapped, &read, FALSE) ||
          read != slot.requested) {
        error = "Unable to read archive volume " +
                volumes_[chunk.volumeIndex].path.string() + ": " +
                WindowsErrorMessage(GetLastError());
        slot.active = false;
        cancelActive();
        return false;
      }
      slot.active = false;
      return true;
    };

    size_t currentSlot = 0;
    size_t nextOffset = 0;
    if (!issueRead(currentSlot, nextOffset)) {
      if (hash != nullptr) {
        BCryptDestroyHash(hash);
      }
      return {};
    }
    nextOffset += slots[currentSlot].requested;
    while (true) {
      if (!waitRead(currentSlot)) {
        if (hash != nullptr) {
          BCryptDestroyHash(hash);
        }
        return {};
      }
      const size_t nextSlot = 1 - currentSlot;
      const bool hasNext = nextOffset < block->data.size();
      if (hasNext && !issueRead(nextSlot, nextOffset)) {
        if (hash != nullptr) {
          BCryptDestroyHash(hash);
        }
        return {};
      }
      if (hasNext) {
        nextOffset += slots[nextSlot].requested;
      }
      if (verifyHash &&
          BCryptHashData(hash, block->data.data() + slots[currentSlot].offset,
                         slots[currentSlot].requested, 0) != 0) {
        error = "Unable to hash archive block";
        cancelActive();
        BCryptDestroyHash(hash);
        return {};
      }
      if (!hasNext) {
        break;
      }
      currentSlot = nextSlot;
    }

    if (verifyHash) {
      const NTSTATUS finish = BCryptFinishHash(
          hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
      BCryptDestroyHash(hash);
      if (finish != 0) {
        error = "Unable to finish archive block SHA-256";
        return {};
      }
    }
    if (verifyHash && HexDigest(digest) != chunk.expectedHash) {
      error = "SHA256 mismatch in " + volumes_[chunk.volumeIndex].path.filename().string() +
              ", block " + std::to_string(chunk.fileChunkIndex + 1);
      return {};
    }
    return block;
  }

  void EvictLocked() {
    while (cache_.size() >= kMaxCachedBlocks) {
      auto victim = cache_.end();
      for (auto iterator = cache_.begin(); iterator != cache_.end(); ++iterator) {
        if (iterator->first == requestedChunk_ || iterator->second.use_count() > 1) {
          continue;
        }
        if (victim == cache_.end() ||
            iterator->second->accessStamp < victim->second->accessStamp) {
          victim = iterator;
        }
      }
      if (victim == cache_.end()) {
        break;
      }
      cache_.erase(victim);
    }
  }

  void WorkerLoop() {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
      SetError("Unable to initialize Windows SHA-256 provider");
      return;
    }
    std::array<HANDLE, 2> readEvents = {
        CreateEventW(nullptr, TRUE, FALSE, nullptr),
        CreateEventW(nullptr, TRUE, FALSE, nullptr),
    };
    if (readEvents[0] == nullptr || readEvents[1] == nullptr) {
      if (readEvents[0] != nullptr) {
        CloseHandle(readEvents[0]);
      }
      if (readEvents[1] != nullptr) {
        CloseHandle(readEvents[1]);
      }
      BCryptCloseAlgorithmProvider(algorithm, 0);
      SetError("Unable to create asynchronous archive read events");
      return;
    }
    while (true) {
      size_t index = 0;
      bool verifyHash = true;
      {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [&]() {
          return stopping_ || IsCancelled() || !error_.empty() || !tasks_.empty();
        });
        if (stopping_ || IsCancelled() || !error_.empty()) {
          break;
        }
        index = tasks_.front();
        tasks_.pop_front();
        queued_.erase(index);
        if (cache_.contains(index)) {
          continue;
        }
        verifyHash = !verified_[index];
      }

      std::string loadError;
      auto block = LoadBlock(index, algorithm, verifyHash, readEvents, loadError);
      uint64_t validatedBytes = 0;
      uint64_t validatedVolumeBytes = 0;
      size_t volumeIndex = 0;
      {
        std::lock_guard lock(mutex_);
        if (!block) {
          if (error_.empty()) {
            error_ = std::move(loadError);
          }
          condition_.notify_all();
          break;
        }
        if (!verified_[index]) {
          verified_[index] = true;
          ++verifiedCount_;
          validatedBytes_ += chunks_[index].size;
          validatedVolumeBytes_[chunks_[index].volumeIndex] += chunks_[index].size;
        }
        EvictLocked();
        block->accessStamp = ++accessStamp_;
        cache_[index] = std::move(block);
        validatedBytes = validatedBytes_;
        volumeIndex = chunks_[index].volumeIndex;
        validatedVolumeBytes = validatedVolumeBytes_[volumeIndex];
      }
      condition_.notify_all();
      reporter_->SetValidated(
          validatedBytes, volumes_[volumeIndex].path.filename(), volumeIndex + 1,
          volumes_.size(), validatedVolumeBytes, volumes_[volumeIndex].size);
    }
    CloseHandle(readEvents[0]);
    CloseHandle(readEvents[1]);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    condition_.notify_all();
  }

  const std::atomic_bool* cancelRequested_{nullptr};
  std::shared_ptr<ProgressReporter> reporter_;
  std::vector<VolumeHandle> volumes_;
  std::vector<ChunkLocation> chunks_;
  std::vector<bool> verified_;
  std::vector<uint64_t> validatedVolumeBytes_;
  uint64_t totalSize_{0};
  uint64_t position_{0};
  mutable std::mutex streamMutex_;
  uint64_t readCalls_{0};
  uint64_t seekCalls_{0};
  uint64_t bytesServed_{0};
  uint64_t lastReadStart_{0};
  UInt32 lastReadRequest_{0};
  Int64 lastSeekOffset_{0};
  UInt32 lastSeekOrigin_{0};

  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::deque<size_t> tasks_;
  std::unordered_set<size_t> queued_;
  std::unordered_map<size_t, std::shared_ptr<CachedBlock>> cache_;
  std::thread worker_;
  std::string error_;
  size_t requestedChunk_{std::numeric_limits<size_t>::max()};
  size_t verifiedCount_{0};
  uint64_t validatedBytes_{0};
  uint64_t accessStamp_{0};
  bool stopping_{false};
};

class ArchiveOpenCallback final : public IArchiveOpenCallback, private ComRefCount {
public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (EqualInterface(iid, IID_IUnknown) || EqualInterface(iid, kIidArchiveOpenCallback)) {
      *object = static_cast<IArchiveOpenCallback*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }
  HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) override { return S_OK; }
  HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64*) override { return S_OK; }

private:
  ~ArchiveOpenCallback() override = default;
};

class FileOutStream final : public ISequentialOutStream, private ComRefCount {
public:
  explicit FileOutStream(const std::filesystem::path& path,
                         const std::atomic_bool* cancelRequested)
      : cancelRequested_(cancelRequested) {
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    openError_ = handle_ == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  }

  bool Ready() const { return handle_ != INVALID_HANDLE_VALUE; }
  DWORD OpenError() const { return openError_; }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (EqualInterface(iid, IID_IUnknown) || EqualInterface(iid, kIidSequentialOutStream)) {
      *object = static_cast<ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }

  HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* processedSize) override {
    if (processedSize == nullptr) {
      return E_POINTER;
    }
    *processedSize = 0;
    if (cancelRequested_ != nullptr && cancelRequested_->load()) {
      return E_ABORT;
    }
    DWORD written = 0;
    if (!WriteFile(handle_, data, size, &written, nullptr)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    *processedSize = written;
    return S_OK;
  }

  void Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      FlushFileBuffers(handle_);
      CloseHandle(handle_);
      handle_ = INVALID_HANDLE_VALUE;
    }
  }

private:
  ~FileOutStream() override { Close(); }

  HANDLE handle_{INVALID_HANDLE_VALUE};
  DWORD openError_{ERROR_SUCCESS};
  const std::atomic_bool* cancelRequested_{nullptr};
};

class ArchiveExtractCallback final : public IArchiveExtractCallback, private ComRefCount {
public:
  ArchiveExtractCallback(IInArchive* archive,
                         std::filesystem::path outputFolder,
                         const std::atomic_bool* cancelRequested,
                         std::shared_ptr<ProgressReporter> reporter)
      : archive_(archive), outputFolder_(std::move(outputFolder)),
        cancelRequested_(cancelRequested), reporter_(std::move(reporter)) {
    archive_->AddRef();
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (EqualInterface(iid, IID_IUnknown) || EqualInterface(iid, kIidProgress) ||
        EqualInterface(iid, kIidArchiveExtractCallback)) {
      *object = static_cast<IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
  ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }

  HRESULT STDMETHODCALLTYPE SetTotal(UInt64 total) override {
    reporter_->SetExtractionTotal(total);
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64* completeValue) override {
    if (completeValue != nullptr) {
      reporter_->SetExtractionCompleted(*completeValue);
    }
    return IsCancelled() ? E_ABORT : S_OK;
  }

  HRESULT STDMETHODCALLTYPE GetStream(
      UInt32 index, ISequentialOutStream** outStream, Int32 askExtractMode) override {
    if (outStream == nullptr) {
      return E_POINTER;
    }
    *outStream = nullptr;
    ReleaseCurrent();
    if (IsCancelled()) {
      return E_ABORT;
    }

    PROPVARIANT pathProperty{};
    PropVariantInit(&pathProperty);
    const HRESULT pathResult = archive_->GetProperty(index, kPropPath, &pathProperty);
    if (FAILED(pathResult) || pathProperty.vt != VT_BSTR || pathProperty.bstrVal == nullptr) {
      PropVariantClear(&pathProperty);
      error_ = "7-Zip archive item has no valid path";
      return E_FAIL;
    }
    const std::filesystem::path relative(pathProperty.bstrVal);
    PropVariantClear(&pathProperty);
    if (!IsSafeOutputPath(relative)) {
      error_ = "7-Zip archive contains an unsafe output path: " + PathToUtf8(relative);
      return E_FAIL;
    }

    PROPVARIANT directoryProperty{};
    PropVariantInit(&directoryProperty);
    const HRESULT directoryResult = archive_->GetProperty(index, kPropIsDir, &directoryProperty);
    const bool isDirectory = SUCCEEDED(directoryResult) && directoryProperty.vt == VT_BOOL &&
                             directoryProperty.boolVal != VARIANT_FALSE;
    PropVariantClear(&directoryProperty);

    currentPath_ = outputFolder_ / relative;
    std::error_code ec;
    if (isDirectory) {
      std::filesystem::create_directories(currentPath_, ec);
      if (ec) {
        error_ = "Unable to create extracted directory: " + ec.message();
        return E_FAIL;
      }
      return S_OK;
    }
    if (askExtractMode != kAskExtract) {
      return S_OK;
    }
    std::filesystem::create_directories(currentPath_.parent_path(), ec);
    if (ec) {
      error_ = "Unable to create extracted file directory: " + ec.message();
      return E_FAIL;
    }
    auto* stream = new FileOutStream(currentPath_, cancelRequested_);
    if (!stream->Ready()) {
      error_ = "Unable to create extracted file " + PathToUtf8(currentPath_) + ": " +
               WindowsErrorMessage(stream->OpenError());
      stream->Release();
      return E_FAIL;
    }
    current_ = stream;
    current_->AddRef();
    currentFile_ = stream;
    *outStream = stream;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) override {
    return IsCancelled() ? E_ABORT : S_OK;
  }

  HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 operationResult) override {
    if (currentFile_ != nullptr) {
      currentFile_->Close();
    }
    ReleaseCurrent();
    if (operationResult != kOperationOk) {
      ++errors_;
      if (error_.empty()) {
        error_ = "7-Zip reported extraction error " + std::to_string(operationResult) +
                 " for " + PathToUtf8(currentPath_);
      }
    }
    return S_OK;
  }

  size_t Errors() const { return errors_; }
  const std::string& Error() const { return error_; }

private:
  ~ArchiveExtractCallback() override {
    ReleaseCurrent();
    archive_->Release();
  }

  bool IsCancelled() const {
    return cancelRequested_ != nullptr && cancelRequested_->load();
  }

  void ReleaseCurrent() {
    currentFile_ = nullptr;
    if (current_ != nullptr) {
      current_->Release();
      current_ = nullptr;
    }
  }

  IInArchive* archive_{nullptr};
  std::filesystem::path outputFolder_;
  const std::atomic_bool* cancelRequested_{nullptr};
  std::shared_ptr<ProgressReporter> reporter_;
  ISequentialOutStream* current_{nullptr};
  FileOutStream* currentFile_{nullptr};
  std::filesystem::path currentPath_;
  size_t errors_{0};
  std::string error_;
};

uint64_t ManifestBytes(const Manifest& manifest) {
  uint64_t total = 0;
  for (const ManifestFile* file : ArchiveVolumes(manifest)) {
    total += file->size;
  }
  return total;
}

}  // namespace

bool PipelinedSevenZipExtractor::CanUse(const Manifest& manifest) {
  if (manifest.hashChunkSize == 0 || manifest.hashChunkSize > kMaxPipelineChunkSize) {
    return false;
  }
  const auto volumes = ArchiveVolumes(manifest);
  if (volumes.empty()) {
    return false;
  }
  return std::all_of(volumes.begin(), volumes.end(), [&](const ManifestFile* file) {
    const uint64_t expected = file->size / manifest.hashChunkSize +
        (file->size % manifest.hashChunkSize == 0 ? 0 : 1);
    return file->chunkSha256.size() == expected;
  });
}

Result<std::filesystem::path> PipelinedSevenZipExtractor::LocateLibrary(
    const std::filesystem::path& appRoot) const {
  auto embedded = ExtractEmbeddedLibrary(appRoot);
  if (embedded.ok()) {
    return embedded;
  }
  const std::vector<std::filesystem::path> candidates = {
      appRoot / "data" / "tools" / "7zip" / "7z.dll",
      appRoot / "resources" / "7z.dll",
      appRoot / ".." / "resources" / "7z.dll",
      std::filesystem::current_path() / "resources" / "7z.dll",
  };
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(candidate, ec) && !ec) {
      return Result<std::filesystem::path>::Ok(candidate);
    }
  }
  return Result<std::filesystem::path>::Error(
      "7z.dll is unavailable: " + embedded.error());
}

ExtractionResult PipelinedSevenZipExtractor::Extract(
    const PipelinedExtractionConfig& config,
    ProgressCallback progressCallback) const {
  ExtractionResult result;
  result.command = "7z.dll verified asynchronous pipeline";
  result.exitCode = 2;
  if (config.manifest == nullptr || !CanUse(*config.manifest)) {
    result.message = "Manifest does not support pipelined chunk validation";
    return result;
  }
  std::error_code ec;
  std::filesystem::create_directories(config.installFolder, ec);
  if (ec) {
    result.message = "Unable to create extraction folder: " + ec.message();
    return result;
  }

  HMODULE library = LoadLibraryExW(
      config.sevenZipLibrary.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
  if (library == nullptr) {
    library = LoadLibraryW(config.sevenZipLibrary.c_str());
  }
  if (library == nullptr) {
    result.message = "Unable to load 7z.dll: " + WindowsErrorMessage(GetLastError());
    return result;
  }
  const auto createObject = reinterpret_cast<CreateObjectFn>(
      GetProcAddress(library, "CreateObject"));
  if (createObject == nullptr) {
    result.message = "7z.dll does not export CreateObject";
    FreeLibrary(library);
    return result;
  }

  IInArchive* rawArchive = nullptr;
  const HRESULT createResult = createObject(
      &kClsid7zFormat, &kIidInArchive, reinterpret_cast<void**>(&rawArchive));
  ComPtr<IInArchive> archive(rawArchive);
  if (FAILED(createResult) || !archive) {
    result.message = "Unable to create the 7-Zip archive handler";
    FreeLibrary(library);
    return result;
  }

  const auto reporter = std::make_shared<ProgressReporter>(
      ManifestBytes(*config.manifest), std::move(progressCallback));
  auto* rawStream = new VerifiedArchiveStream(config, reporter);
  ComPtr<IInStream> stream(rawStream);
  if (!rawStream->Ready()) {
    result.message = rawStream->Error();
    archive.Reset();
    stream.Reset();
    FreeLibrary(library);
    return result;
  }

  ComPtr<IArchiveOpenCallback> openCallback(new ArchiveOpenCallback());
  const UInt64 maxCheckStartPosition = 0;
  HRESULT operation = archive->Open(stream.Get(), &maxCheckStartPosition, openCallback.Get());
  if (FAILED(operation)) {
    result.message = rawStream->Error().empty()
        ? "7-Zip could not open the verified virtual archive"
        : rawStream->Error();
  } else {
    ISetProperties* rawProperties = nullptr;
    const HRESULT propertiesInterface = archive->QueryInterface(
        kIidSetProperties, reinterpret_cast<void**>(&rawProperties));
    ComPtr<ISetProperties> properties(rawProperties);
    if (FAILED(propertiesInterface) || !properties) {
      result.message = "7-Zip SDK does not expose decoder configuration";
      operation = E_NOINTERFACE;
    } else {
      const wchar_t* names[] = {L"mt"};
      PROPVARIANT value{};
      PropVariantInit(&value);
      value.vt = VT_UI4;
      value.ulVal = std::max<uint32_t>(1, config.decoderThreads);
      operation = properties->SetProperties(names, &value, 1);
      PropVariantClear(&value);
      if (FAILED(operation)) {
        result.message = "Unable to configure 7-Zip decoder threads (HRESULT " +
                         std::to_string(static_cast<unsigned long>(operation)) + ")";
      }
    }

    if (SUCCEEDED(operation)) {
    auto* rawExtractCallback = new ArchiveExtractCallback(
        archive.Get(), config.installFolder, config.cancelRequested, reporter);
    ComPtr<IArchiveExtractCallback> extractCallback(rawExtractCallback);
    operation = archive->Extract(
        nullptr, std::numeric_limits<UInt32>::max(), 0, extractCallback.Get());
    const bool allValidated = SUCCEEDED(operation) && rawStream->VerifyAll();
    if (SUCCEEDED(operation) && allValidated && rawExtractCallback->Errors() == 0) {
      reporter->FinishExtraction();
      result.ok = true;
      result.exitCode = 0;
      result.message = "Verified asynchronous extraction completed";
    } else if (!rawStream->Error().empty()) {
      result.message = rawStream->Error();
    } else if (!rawExtractCallback->Error().empty()) {
      result.message = rawExtractCallback->Error();
    } else if (config.cancelRequested != nullptr && config.cancelRequested->load()) {
      result.message = "Extraction was stopped by the user";
      result.exitCode = 255;
    } else {
      result.message = "7-Zip SDK extraction failed (HRESULT " +
                       std::to_string(static_cast<unsigned long>(operation)) + "; " +
                       rawStream->Diagnostics() + ")";
    }
    }
  }

  archive->Close();
  archive.Reset();
  stream.Reset();
  FreeLibrary(library);
  result.output = result.message;
  return result;
}

}  // namespace modlist
