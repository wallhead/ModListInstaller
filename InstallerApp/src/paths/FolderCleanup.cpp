#include "paths/FolderCleanup.h"
#include "app/PackageDiscovery.h"
#include "app/RuntimePaths.h"

#define NOMINMAX
#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <cwctype>
#include <cstddef>
#include <sstream>
#include <string_view>
#include <vector>

namespace modlist {
namespace {
namespace fs = std::filesystem;

struct Handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  Handle() = default;
  explicit Handle(HANDLE handle) : value(handle) {}
  ~Handle() { Close(); }
  Handle(Handle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) { Close(); value = std::exchange(other.value, INVALID_HANDLE_VALUE); }
    return *this;
  }
  void Close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(std::exchange(value, INVALID_HANDLE_VALUE)); }
  explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};

std::string WinError(const char* operation) {
  return std::string(operation) + " (Windows error " + std::to_string(GetLastError()) + ")";
}
std::wstring Fold(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return std::towlower(c); });
  while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
  return text;
}
fs::path AccessPath(const fs::path& path) {
  const auto text = path.native();
  if (text.starts_with(L"\\\\?\\")) return path;
  return text.starts_with(L"\\\\") ? fs::path(L"\\\\?\\UNC\\" + text.substr(2)) : fs::path(L"\\\\?\\" + text);
}
Result<fs::path> Normalize(const fs::path& path) {
  const auto text = path.native();
  if (!path.is_absolute() || text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\") ||
      text.find(L'\0') != std::wstring::npos) {
    return Result<fs::path>::Error("Cleanup requires an ordinary absolute drive or share path");
  }
  for (const auto& part : path.relative_path()) {
    const auto name = part.native();
    if (!name.empty() && (name == L"." || name == L".." || name.back() == L'.' ||
        name.back() == L' ' || name.find(L':') != std::wstring::npos)) {
      return Result<fs::path>::Error("Ambiguous cleanup path component");
    }
  }
  auto normalized = path.lexically_normal();
  if (normalized.filename().empty()) normalized = normalized.parent_path();
  return Result<fs::path>::Ok(normalized.make_preferred());
}

struct Identity {
  DWORD volume = 0, high = 0, low = 0;
  std::wstring resolved;
  bool operator==(const Identity&) const = default;
};
Result<Identity> Identify(HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) return Result<Identity>::Error(WinError("Read folder identity"));
  if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return Result<Identity>::Error("Reparse points/junctions are not allowed in cleanup paths");
  }
  std::vector<wchar_t> name(32768);
  const DWORD size = GetFinalPathNameByHandleW(handle, name.data(), static_cast<DWORD>(name.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!size || size >= name.size()) return Result<Identity>::Error(WinError("Resolve folder identity"));
  return Result<Identity>::Ok({info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
      Fold(std::wstring(name.data(), size))});
}
struct PinnedFolder {
  fs::path folder;
  Identity identity;
  std::vector<Handle> chain;
};

enum class RelativeOpenMode { Synchronous, AsynchronousWatch };

Result<Handle> OpenRelative(HANDLE parent, const std::wstring& name, ACCESS_MASK access,
    bool directory, ULONG disposition = FILE_OPEN, ULONG attributes = 0,
    RelativeOpenMode mode = RelativeOpenMode::Synchronous) {
  if (name.empty() || name == L"." || name == L".." || name.size() > 32767 ||
      name.find_first_of(L"\\/:") != std::wstring::npos || name.find(L'\0') != std::wstring::npos) {
    return Result<Handle>::Error("Native cleanup opens require exactly one child name");
  }
  using NativeCreate = decltype(&NtCreateFile);
  static const auto create = reinterpret_cast<NativeCreate>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  if (!create) return Result<Handle>::Error("Native no-follow file opening is unavailable");
  UNICODE_STRING child{};
  child.Buffer = const_cast<wchar_t*>(name.data());
  child.Length = child.MaximumLength = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  OBJECT_ATTRIBUTES object{};
  InitializeObjectAttributes(&object, &child, OBJ_CASE_INSENSITIVE | OBJ_DONT_REPARSE, parent, nullptr);
  IO_STATUS_BLOCK statusBlock{};
  HANDLE raw = INVALID_HANDLE_VALUE;
  const bool watch = mode == RelativeOpenMode::AsynchronousWatch;
  // RootDirectory is the pinned object, never a path that can resolve through a changed ancestor.
  // Attribute-only writes bypass sharing: reparse checks alone cannot anchor traversal.
  const NTSTATUS status = create(&raw, access | SYNCHRONIZE, &object, &statusBlock, nullptr, attributes,
      FILE_SHARE_READ | (watch ? FILE_SHARE_DELETE : 0), disposition,
      FILE_OPEN_REPARSE_POINT | (watch ? 0 : FILE_SYNCHRONOUS_IO_NONALERT) |
      (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE), nullptr, 0);
  if (status < 0) return Result<Handle>::Error("Open pinned child without following reparses (NTSTATUS " + std::to_string(status) + ")");
  return Result<Handle>::Ok(Handle(raw));
}

Result<void> CheckPlainDirectory(HANDLE handle) {
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(handle, &info)) return Result<void>::Error(WinError("Inspect pinned directory"));
  if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return Result<void>::Error("Pinned directory became a reparse point or is not a directory");
  }
  return Result<void>::Ok();
}
Result<void> CheckChain(const PinnedFolder& pinned) {
  for (const auto& handle : pinned.chain) {
    const auto checked = CheckPlainDirectory(handle.value);
    if (!checked.ok()) return checked;
  }
  return Result<void>::Ok();
}

struct DirectoryEntry {
  std::wstring name;
  DWORD attributes;
  ULONGLONG id;
};
template <typename Visitor>
Result<void> VisitDirectory(HANDLE directory, Visitor&& visit) {
  const auto checked = CheckPlainDirectory(directory);
  if (!checked.ok()) return checked;
  // Heap storage avoids multiplying a 64 KiB stack buffer by the recursion depth.
  std::vector<ULONGLONG> storage(8192);
  constexpr size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
  const size_t capacity = storage.size() * sizeof(ULONGLONG);
  bool restart = true;
  for (;;) {
    if (!GetFileInformationByHandleEx(directory,
        restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo,
        storage.data(), static_cast<DWORD>(capacity))) {
      if (GetLastError() == ERROR_NO_MORE_FILES) return CheckPlainDirectory(directory);
      return Result<void>::Error(WinError("Enumerate pinned directory handle"));
    }
    restart = false;
    size_t offset = 0;
    for (;;) {
      if (offset > capacity - header) return Result<void>::Error("Invalid native directory record offset");
      const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(
          reinterpret_cast<const unsigned char*>(storage.data()) + offset);
      if (entry->FileNameLength % sizeof(wchar_t) || entry->FileNameLength > capacity - offset - header) {
        return Result<void>::Error("Invalid native directory record name");
      }
      std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        auto next = visit(DirectoryEntry{std::move(name), entry->FileAttributes,
            static_cast<ULONGLONG>(entry->FileId.QuadPart)});
        if (!next.ok()) return Result<void>::Error(next.error());
        if (!next.value()) return CheckPlainDirectory(directory);
      }
      if (!entry->NextEntryOffset) break;
      if (entry->NextEntryOffset < header || entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) ||
          entry->NextEntryOffset > capacity - offset) {
        return Result<void>::Error("Invalid native directory record chain");
      }
      offset += entry->NextEntryOffset;
    }
  }
}
Result<bool> DirectoryIsEmpty(HANDLE directory) {
  bool empty = true;
  const auto read = VisitDirectory(directory, [&](const DirectoryEntry&) {
    empty = false;
    return Result<bool>::Ok(false);
  });
  if (!read.ok()) return Result<bool>::Error(read.error());
  return Result<bool>::Ok(empty);
}

class StagingChangeWatch {
 public:
  ~StagingChangeWatch() { if (armed_) Stop(); }

  Result<void> Start(const PinnedFolder& pinned) {
    // An independent asynchronous handle to the same pinned directory. It must
    // share delete access with the original session handle, which remains pinned.
    const auto parent = pinned.chain[pinned.chain.size() - 2].value;
    auto opened = OpenRelative(parent, pinned.folder.filename().native(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        true, FILE_OPEN, 0, RelativeOpenMode::AsynchronousWatch);
    if (!opened.ok()) return Result<void>::Error(opened.error());
    directory_ = std::move(opened.value());
    const auto identity = Identify(directory_.value);
    if (!identity.ok()) return Result<void>::Error(identity.error());
    if (!(identity.value() == pinned.identity)) return Result<void>::Error("Staging watch identity changed");

    const HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return Result<void>::Error(WinError("Create staging change event"));
    event_ = Handle(event);
    change_.hEvent = event;
    armed_ = ReadDirectoryChangesW(directory_.value, buffer_, sizeof(buffer_), TRUE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY,
        nullptr, &change_, nullptr) != FALSE;
    if (!armed_) return Result<void>::Error(WinError("Monitor staging contents"));
    return Result<void>::Ok();
  }

  Result<void> CheckUnchanged() {
    if (Stop()) return Result<void>::Ok();
    return Result<void>::Error("Staging changed or could not be verified; ownership retained for recovery");
  }

 private:
  bool Stop() noexcept {
    // Reap the request before its event and buffer go out of scope. A normal
    // completion, overflow or other error must not be mistaken for cancellation.
    const BOOL canceled = CancelIoEx(directory_.value, &change_);
    DWORD bytes = 0;
    const BOOL completed = GetOverlappedResult(directory_.value, &change_, &bytes, TRUE);
    const DWORD completionError = completed ? ERROR_SUCCESS : GetLastError();
    armed_ = false;
    directory_.Close();
    return canceled && completionError == ERROR_OPERATION_ABORTED;
  }

  Handle directory_, event_;
  OVERLAPPED change_{};
  DWORD buffer_[1024]{};
  bool armed_{false};
};

Result<PinnedFolder> PinFolder(const fs::path& folder, bool deleteRoot = false,
    bool createMissing = false, bool* createdLeaf = nullptr) {
  const auto normalized = Normalize(folder);
  if (!normalized.ok()) return Result<PinnedFolder>::Error(normalized.error());
  const auto path = normalized.value();
  fs::path base = path.root_path();
  auto relative = path.relative_path();
  if (path.root_name().native().starts_with(L"\\\\")) {
    if (relative.empty()) return Result<PinnedFolder>::Error("Share root is not a cleanup folder");
    base /= *relative.begin();
    relative = path.lexically_relative(base);
  }
  if (Fold(path.native()) == Fold(base.native())) return Result<PinnedFolder>::Error("Volume/share roots cannot be cleared");
  PinnedFolder pinned;
  pinned.folder = path;
  Handle baseHandle(CreateFileW(AccessPath(base).c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
      FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
  if (!baseHandle) return Result<PinnedFolder>::Error(WinError("Pin cleanup volume/share root"));
  const auto baseIdentity = Identify(baseHandle.value);
  if (!baseIdentity.ok()) return Result<PinnedFolder>::Error(baseIdentity.error());
  pinned.chain.push_back(std::move(baseHandle));
  const std::vector<fs::path> components(relative.begin(), relative.end());
  for (size_t i = 0; i < components.size(); ++i) {
    const auto access = FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY |
        ((deleteRoot && i + 1 == components.size()) ? DELETE : 0);
    auto opened = OpenRelative(pinned.chain.back().value, components[i].native(), access, true);
    if (!opened.ok() && createMissing) {
      opened = OpenRelative(pinned.chain.back().value, components[i].native(), access, true, FILE_CREATE);
      if (opened.ok()) {
        if (createdLeaf && i + 1 == components.size()) *createdLeaf = true;
      } else {
        opened = OpenRelative(pinned.chain.back().value, components[i].native(), access, true);
      }
    }
    if (!opened.ok()) return Result<PinnedFolder>::Error(opened.error());
    auto handle = std::move(opened.value());
    const auto identity = Identify(handle.value);
    if (!identity.ok()) return Result<PinnedFolder>::Error(identity.error());
    const auto plain = CheckPlainDirectory(handle.value);
    if (!plain.ok()) return Result<PinnedFolder>::Error(plain.error());
    pinned.identity = identity.value();
    pinned.chain.push_back(std::move(handle));
  }
  std::vector<wchar_t> volumePath(32768);
  if (!GetVolumePathNameW(pinned.identity.resolved.c_str(), volumePath.data(), static_cast<DWORD>(volumePath.size()))) {
    return Result<PinnedFolder>::Error(WinError("Resolve cleanup volume/share root"));
  }
  const auto& a = pinned.identity;
  const auto& b = baseIdentity.value();
  if ((a.volume == b.volume && a.high == b.high && a.low == b.low) || a.resolved == Fold(volumePath.data())) {
    return Result<PinnedFolder>::Error("Resolved volume/share roots cannot be cleared");
  }
  const auto checked = CheckChain(pinned);
  if (!checked.ok()) return Result<PinnedFolder>::Error(checked.error());
  return Result<PinnedFolder>::Ok(std::move(pinned));
}

Result<std::string> OwnerBytes(const PinnedFolder& folder, const fs::path& package, const std::string& packageId) {
  if (packageId.empty() || packageId.size() > 256 || packageId.find_first_of("\r\n") != std::string::npos) {
    return Result<std::string>::Error("A package identity is required for staging ownership");
  }
  std::error_code ec;
  const auto canonical = Fold(fs::canonical(package, ec).make_preferred().native());
  if (ec) return Result<std::string>::Error("Cannot resolve staging package identity");
  std::ostringstream bytes;
  bytes << "ModlistInstaller staging v2\n" << folder.identity.volume << ':' << folder.identity.high << ':'
        << folder.identity.low << '\n' << packageId << '\n';
  // Encode UTF-16 code units deterministically without locale-dependent path conversion.
  for (wchar_t c : canonical) bytes << std::hex << static_cast<unsigned>(c) << ',';
  return Result<std::string>::Ok(bytes.str());
}
constexpr wchar_t kOwnerMarkerName[] = L"owner";
struct Ownership {
  PinnedFolder metadata;
  Handle marker;
  bool pending = false;
};
std::string OwnerRecord(const std::string& binding, bool pending) {
  return (pending ? "PENDING\n" : "COMMITTED\n") + binding;
}
Result<void> WriteOwnerRecord(HANDLE marker, const std::string& record) {
  LARGE_INTEGER start{};
  DWORD written = 0;
  if (!SetFilePointerEx(marker, start, nullptr, FILE_BEGIN) ||
      !WriteFile(marker, record.data(), static_cast<DWORD>(record.size()), &written, nullptr) ||
      written != record.size() || !SetEndOfFile(marker) || !FlushFileBuffers(marker)) {
    return Result<void>::Error("Cannot persist staging ownership state");
  }
  return Result<void>::Ok();
}
Result<void> CheckOwnershipDirectory(HANDLE directory) {
  size_t markerCount = 0;
  const auto read = VisitDirectory(directory, [&](const DirectoryEntry& entry) {
    if (entry.name != kOwnerMarkerName || (entry.attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
      return Result<bool>::Error("Ownership directory contains an unexpected entry; files were not adopted");
    }
    ++markerCount;
    return Result<bool>::Ok(true);
  });
  if (!read.ok()) return read;
  return markerCount == 1 ? Result<void>::Ok() : Result<void>::Error("Ownership directory has no marker");
}
Result<Ownership> ReadOwnerMetadata(PinnedFolder metadata, const std::string& expected, bool allowPending) {
  const auto checked = CheckOwnershipDirectory(metadata.chain.back().value);
  if (!checked.ok()) return Result<Ownership>::Error(checked.error());
  auto markerFile = OpenRelative(metadata.chain.back().value, kOwnerMarkerName,
      GENERIC_READ | DELETE | (allowPending ? GENERIC_WRITE : 0), false);
  if (!markerFile.ok()) return Result<Ownership>::Error("Staging ownership marker is unavailable or active");
  auto marker = std::move(markerFile.value());
  BY_HANDLE_FILE_INFORMATION info{};
  if (!GetFileInformationByHandle(marker.value, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
      info.nFileSizeHigh || info.nFileSizeLow > expected.size() + 32 || info.nNumberOfLinks != 1) {
    return Result<Ownership>::Error("Invalid staging ownership marker; no files were cleared");
  }
  std::string bytes(info.nFileSizeLow, '\0');
  DWORD read = 0;
  if (!ReadFile(marker.value, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
      read != bytes.size()) {
    return Result<Ownership>::Error("Cannot read the complete staging ownership record");
  }
  const bool pending = bytes == OwnerRecord(expected, true);
  if (bytes != OwnerRecord(expected, false) && !(pending && allowPending)) {
    if (pending) return Result<Ownership>::Error("Pending staging ownership cannot authorize cleanup; inspect nonempty contents manually");
    return Result<Ownership>::Error("Staging ownership does not match this package and folder identity");
  }
  return Result<Ownership>::Ok({std::move(metadata), std::move(marker), pending});
}
Result<Ownership> ReadOwner(const PinnedFolder& folder, const std::string& expected) {
  auto metadata = PinFolder(StagingOwnerPath(folder.folder), true);
  if (!metadata.ok()) return Result<Ownership>::Error("Staging ownership is missing, redirected, or active; no files were cleared");
  return ReadOwnerMetadata(std::move(metadata.value()), expected, false);
}
Result<void> DeleteOpened(Handle& handle) {
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(handle.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
    return Result<void>::Error(WinError("Delete pinned cleanup entry"));
  }
  handle.Close();
  return Result<void>::Ok();
}
enum class TreeInspection { ForDeletion, EmptyDirectoriesOnly };

Result<void> CollectTree(HANDLE folder, std::vector<Handle>& entries,
    const FolderCleanupLimits& limits, TreeInspection inspection, size_t depth = 0) {
  if (depth > limits.maxDepth) return Result<void>::Error("Cleanup tree exceeds safe depth limit");
  return VisitDirectory(folder, [&](const DirectoryEntry& entry) {
    if (entries.size() >= limits.maxEntries) return Result<bool>::Error("Cleanup tree exceeds safe entry limit");
    if (entry.attributes & FILE_ATTRIBUTE_REPARSE_POINT) return Result<bool>::Error("Nested reparse point/junction found; cleanup refused");
    const bool directory = (entry.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (inspection == TreeInspection::EmptyDirectoriesOnly && !directory) {
      return Result<bool>::Error("Staging still contains files; ownership retained for recovery");
    }
    const auto access = FILE_READ_ATTRIBUTES | (directory ? FILE_LIST_DIRECTORY : 0) |
        (inspection == TreeInspection::ForDeletion ? DELETE : 0);
    auto opened = OpenRelative(folder, entry.name, access, directory);
    if (!opened.ok()) return Result<bool>::Error(opened.error());
    auto handle = std::move(opened.value());
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle.value, &info)) return Result<bool>::Error(WinError("Inspect cleanup entry"));
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      return Result<bool>::Error("Nested reparse point/junction found; cleanup refused without traversing it");
    }
    if (((static_cast<ULONGLONG>(info.nFileIndexHigh) << 32) | info.nFileIndexLow) != entry.id) {
      return Result<bool>::Error("Cleanup child identity changed after enumeration");
    }
    // Every ancestor and child stays pinned until the entire tree passes inspection.
    const HANDLE child = handle.value;
    entries.push_back(std::move(handle));
    if (directory) {
      const auto collected = CollectTree(child, entries, limits, inspection, depth + 1);
      if (!collected.ok()) return Result<bool>::Error(collected.error());
    }
    return Result<bool>::Ok(true);
  });
}
Result<fs::path> ResolvedOverlapPath(const fs::path& folder) {
  std::error_code ec;
  auto probe = fs::absolute(folder, ec).lexically_normal().make_preferred();
  if (folder.empty() || ec) return Result<fs::path>::Error("Cannot resolve runtime overlap path");
  std::vector<fs::path> suffix;
  for (;;) {
    // Read-only resolution deliberately follows aliases for the containment guard.
    // Actual mutation still requires the separate anchored, no-follow traversal.
    Handle handle(CreateFileW(AccessPath(probe).c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (handle) {
      std::vector<wchar_t> name(32768);
      const DWORD size = GetFinalPathNameByHandleW(handle.value, name.data(), static_cast<DWORD>(name.size()), VOLUME_NAME_DOS);
      if (!size || size >= name.size()) return Result<fs::path>::Error(WinError("Resolve runtime overlap identity"));
      std::wstring text(name.data(), size);
      if (text.starts_with(L"\\\\?\\UNC\\")) text = L"\\\\" + text.substr(8);
      else if (text.starts_with(L"\\\\?\\")) text = text.substr(4);
      fs::path resolved(text);
      for (auto it = suffix.rbegin(); it != suffix.rend(); ++it) resolved /= *it;
      return Result<fs::path>::Ok(resolved.lexically_normal().make_preferred());
    }
    const DWORD error = GetLastError();
    if ((error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) || probe == probe.root_path()) {
      return Result<fs::path>::Error("Cannot resolve runtime overlap identity");
    }
    suffix.push_back(probe.filename());
    probe = probe.parent_path();
  }
}
} // namespace

struct FolderCleanupTicket::Details {
  fs::path folder, package;
  FolderCleanupKind kind;
  std::string packageId;
  Identity identity;
};

bool InstallationOverlapsRuntime(const fs::path& folder) {
  const auto targetPath = ResolvedOverlapPath(folder);
  const auto runtimePath = ResolvedOverlapPath(UserRuntimeFolder(folder).parent_path());
  if (!targetPath.ok() || !runtimePath.ok()) return true;
  const auto& target = targetPath.value();
  const auto& runtime = runtimePath.value();
  auto a = target.begin();
  auto b = runtime.begin();
  while (a != target.end() && b != runtime.end()) {
    const auto left = a->native();
    const auto right = b->native();
    if (CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
        static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) return false;
    ++a;
    ++b;
  }
  return true; // Either folder contains the other. The entire runtime tree is reserved.
}

Result<FolderCleanupTicket> PrepareFolderCleanup(const fs::path& folder, const fs::path& package,
    FolderCleanupKind kind, const std::string& packageId) {
  if (InstallationOverlapsRuntime(folder)) return Result<FolderCleanupTicket>::Error("Cleanup overlaps installer runtime/cache space");
  auto pinned = PinFolder(folder);
  if (!pinned.ok()) return Result<FolderCleanupTicket>::Error(pinned.error());
  if (InstallationOverlapsPackage(folder, package) || InstallationOverlapsPackage(pinned.value().identity.resolved, package)) {
    return Result<FolderCleanupTicket>::Error("Cleanup overlaps installer package inputs");
  }
  if (kind == FolderCleanupKind::OwnedStaging) {
    const auto bytes = OwnerBytes(pinned.value(), package, packageId);
    if (!bytes.ok()) return Result<FolderCleanupTicket>::Error(bytes.error());
    const auto owner = ReadOwner(pinned.value(), bytes.value());
    if (!owner.ok()) return Result<FolderCleanupTicket>::Error(owner.error());
  }
  FolderCleanupTicket ticket;
  ticket.details = std::make_shared<FolderCleanupTicket::Details>(FolderCleanupTicket::Details{
      pinned.value().folder, package, kind, packageId, pinned.value().identity});
  return Result<FolderCleanupTicket>::Ok(std::move(ticket));
}

Result<void> ClearFolderContents(const FolderCleanupTicket& ticket, const FolderCleanupLimits& limits) {
  if (!ticket.details) return Result<void>::Error("No prepared cleanup identity");
  const auto& request = *ticket.details;
  if (InstallationOverlapsRuntime(request.folder)) return Result<void>::Error("Cleanup overlaps installer runtime/cache space");
  auto pinned = PinFolder(request.folder);
  if (!pinned.ok()) return Result<void>::Error(pinned.error());
  if (!(pinned.value().identity == request.identity)) return Result<void>::Error("Cleanup folder identity changed; prepare and confirm again");
  if (InstallationOverlapsPackage(request.folder, request.package) ||
      InstallationOverlapsPackage(pinned.value().identity.resolved, request.package)) {
    return Result<void>::Error("Cleanup overlaps installer package inputs");
  }
  Ownership ownership;
  if (request.kind == FolderCleanupKind::OwnedStaging) {
    const auto bytes = OwnerBytes(pinned.value(), request.package, request.packageId);
    if (!bytes.ok()) return Result<void>::Error(bytes.error());
    auto owner = ReadOwner(pinned.value(), bytes.value());
    if (!owner.ok()) return Result<void>::Error(owner.error());
    ownership = std::move(owner.value());
  }
  std::vector<Handle> entries;
  const FolderCleanupLimits bounded{std::min(limits.maxEntries, size_t{1000000}),
      std::min(limits.maxDepth, size_t{512})};
  const auto collected = CollectTree(pinned.value().chain.back().value, entries, bounded, TreeInspection::ForDeletion);
  if (!collected.ok()) return collected;
  const auto checked = CheckChain(pinned.value());
  if (!checked.ok()) return checked;
  for (const auto& entry : entries) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(entry.value, &info)) return Result<void>::Error(WinError("Revalidate cleanup entry"));
    if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return Result<void>::Error("Cleanup entry became a reparse point");
  }
  // No path-based remove_all: child identities cannot be swapped before deletion.
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    const auto removed = DeleteOpened(*it);
    if (!removed.ok()) return removed;
  }
  const auto empty = DirectoryIsEmpty(pinned.value().chain.back().value);
  if (!empty.ok()) return Result<void>::Error(empty.error());
  if (!empty.value()) {
    return Result<void>::Error("Cleanup folder changed or is not empty; no newly added entries were removed");
  }
  return Result<void>::Ok();
}

struct StagingSession::Impl {
  PinnedFolder pinned;
  Ownership ownership;
};
StagingSession::StagingSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
StagingSession::~StagingSession() = default;
Result<StagingCompletion> StagingSession::Complete() {
  using Outcome = Result<StagingCompletion>;
  if (!impl_ || !impl_->ownership.marker) return Outcome::Error("Staging session is already closed");
  auto& root = impl_->pinned.chain.back();
  const auto empty = DirectoryIsEmpty(root.value);
  if (!empty.ok()) return Outcome::Error(empty.error());

  StagingChangeWatch watch;
  std::vector<Handle> retainedDirectories;
  if (!empty.value()) {
    // The move can finish even when empty source directories cannot be deleted.
    // Inspect pinned objects without requesting delete access or changing ownership.
    const auto watching = watch.Start(impl_->pinned);
    if (!watching.ok()) return Outcome::Error(watching.error());
    const auto collected = CollectTree(root.value, retainedDirectories, {}, TreeInspection::EmptyDirectoriesOnly);
    if (!collected.ok()) return Outcome::Error(collected.error());
  }
  const auto cacheChain = CheckChain(impl_->ownership.metadata);
  if (!cacheChain.ok()) return Outcome::Error(cacheChain.error());
  const auto metadata = CheckOwnershipDirectory(impl_->ownership.metadata.chain.back().value);
  if (!metadata.ok()) return Outcome::Error(metadata.error());
  for (const auto& directory : retainedDirectories) {
    const auto plain = CheckPlainDirectory(directory.value);
    if (!plain.ok()) return Outcome::Error(plain.error());
  }
  // The subtree watch does not cover its root or ancestors.
  const auto checked = CheckChain(impl_->pinned);
  if (!checked.ok()) return Outcome::Error(checked.error());
  const auto identity = Identify(root.value);
  if (!identity.ok()) return Outcome::Error(identity.error());
  if (!(identity.value() == impl_->pinned.identity)) return Outcome::Error("Staging identity changed");
  if (!empty.value()) {
    const auto unchanged = watch.CheckUnchanged();
    if (!unchanged.ok()) return Outcome::Error(unchanged.error());
  }
  if (!retainedDirectories.empty()) return Outcome::Ok(StagingCompletion::EmptyDirectoriesRetained);

  const auto removedRoot = DeleteOpened(root);
  if (!removedRoot.ok()) return Outcome::Error(removedRoot.error());
  const auto removedMarker = DeleteOpened(impl_->ownership.marker);
  if (!removedMarker.ok()) return Outcome::Error(removedMarker.error());
  const auto removedMetadata = DeleteOpened(impl_->ownership.metadata.chain.back());
  if (!removedMetadata.ok()) return Outcome::Error(removedMetadata.error());
  return Outcome::Ok(StagingCompletion::Removed);
}
fs::path StagingOwnerPath(const fs::path& folder) {
  auto normalized = folder.lexically_normal();
  return UserRuntimeFolder(normalized.make_preferred()) / L"staging-owner";
}
Result<std::unique_ptr<StagingSession>> BeginOwnedStaging(const fs::path& folder,
    const fs::path& package, const std::string& packageId) {
  if (InstallationOverlapsRuntime(folder)) return Result<std::unique_ptr<StagingSession>>::Error("Staging overlaps installer runtime/cache space");
  auto pinned = PinFolder(folder, true);
  if (!pinned.ok()) return Result<std::unique_ptr<StagingSession>>::Error(pinned.error());
  if (InstallationOverlapsPackage(folder, package)) {
    return Result<std::unique_ptr<StagingSession>>::Error("Staging overlaps installer package inputs");
  }
  const auto bytes = OwnerBytes(pinned.value(), package, packageId);
  if (!bytes.ok()) return Result<std::unique_ptr<StagingSession>>::Error(bytes.error());
  bool newOwnership = false;
  auto metadataFolder = PinFolder(StagingOwnerPath(pinned.value().folder), true, true, &newOwnership);
  if (!metadataFolder.ok()) return Result<std::unique_ptr<StagingSession>>::Error(metadataFolder.error());
  Ownership ownership;
  if (!newOwnership) {
    auto existing = ReadOwnerMetadata(std::move(metadataFolder.value()), bytes.value(), true);
    if (!existing.ok()) return Result<std::unique_ptr<StagingSession>>::Error(existing.error());
    ownership = std::move(existing.value());
  } else {
    ownership.metadata = std::move(metadataFolder.value());
    ownership.pending = true;
    auto marker = OpenRelative(ownership.metadata.chain.back().value, kOwnerMarkerName, GENERIC_READ | GENERIC_WRITE | DELETE,
        false, FILE_CREATE, FILE_ATTRIBUTE_HIDDEN);
    if (!marker.ok()) {
      DeleteOpened(ownership.metadata.chain.back());
      return Result<std::unique_ptr<StagingSession>>::Error("Cannot create marker in the new ownership directory: " + marker.error());
    }
    ownership.marker = std::move(marker.value());
    const auto persisted = WriteOwnerRecord(ownership.marker.value, OwnerRecord(bytes.value(), true));
    if (!persisted.ok()) {
      DeleteOpened(ownership.marker);
      DeleteOpened(ownership.metadata.chain.back());
      return Result<std::unique_ptr<StagingSession>>::Error("Cannot persist staging ownership sidecar");
    }
  }
  // The first durable record is NON-AUTHORIZING, including after abrupt process death.
  // Only an empty, identity-checked root may transition from pending to committed.
  const auto empty = DirectoryIsEmpty(pinned.value().chain.back().value);
  const auto checked = CheckChain(pinned.value());
  const auto cacheChain = CheckChain(ownership.metadata);
  const auto metadata = CheckOwnershipDirectory(ownership.metadata.chain.back().value);
  const auto identity = Identify(pinned.value().chain.back().value);
  if (!empty.ok() || !empty.value() || !checked.ok() || !cacheChain.ok() || !metadata.ok() ||
      !identity.ok() || !(identity.value() == pinned.value().identity)) {
    if (newOwnership) {
      LARGE_INTEGER start{};
      DWORD written = 0;
      const char invalid = '\0';
      if (SetFilePointerEx(ownership.marker.value, start, nullptr, FILE_BEGIN)) {
        if (WriteFile(ownership.marker.value, &invalid, 1, &written, nullptr)) FlushFileBuffers(ownership.marker.value);
      }
      const auto discarded = DeleteOpened(ownership.marker);
      if (!discarded.ok()) return Result<std::unique_ptr<StagingSession>>::Error(
          "Staging claim rejected; could not remove invalidated new ownership: " + discarded.error());
      const auto discardedDirectory = DeleteOpened(ownership.metadata.chain.back());
      if (!discardedDirectory.ok()) return Result<std::unique_ptr<StagingSession>>::Error(
          "Staging claim rejected; new ownership directory retained without its marker: " + discardedDirectory.error());
    }
    return Result<std::unique_ptr<StagingSession>>::Error(!empty.ok() ? empty.error() :
        !checked.ok() ? checked.error() : !cacheChain.ok() ? cacheChain.error() : !metadata.ok() ? metadata.error() :
        "Only empty staging can be claimed; raced/existing files are not adopted");
  }
  if (ownership.pending) {
    const auto committed = WriteOwnerRecord(ownership.marker.value, OwnerRecord(bytes.value(), false));
    if (!committed.ok()) return Result<std::unique_ptr<StagingSession>>::Error(committed.error());
    ownership.pending = false;
  }
  auto impl = std::make_unique<StagingSession::Impl>();
  impl->pinned = std::move(pinned.value());
  impl->ownership = std::move(ownership);
  return Result<std::unique_ptr<StagingSession>>::Ok(std::make_unique<StagingSession>(std::move(impl)));
}
} // namespace modlist
