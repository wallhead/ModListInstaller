#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include <aclapi.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <functional>
#include <stdexcept>
#include <vector>

#ifndef FOLDER_CLEANUP_LEGACY_REPRO
namespace native_barriers {
std::function<void(HANDLE, BY_HANDLE_FILE_INFORMATION&)> afterInspect;
std::function<void(const std::wstring&)> beforeOpen;
std::function<void()> afterFlush;
std::function<void(HANDLE)> beforeEnumerate;
using NtCreate = decltype(&NtCreateFile);
BOOL WINAPI Inspect(HANDLE handle, LPBY_HANDLE_FILE_INFORMATION info) {
  const BOOL ok = ::GetFileInformationByHandle(handle, info);
  if (ok && afterInspect) afterInspect(handle, *info);
  return ok;
}
HANDLE WINAPI Open(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
    DWORD disposition, DWORD flags, HANDLE templateFile) {
  if (beforeOpen) beforeOpen(std::filesystem::path(name).filename().wstring());
  return ::CreateFileW(name, access, share, security, disposition, flags, templateFile);
}
NTSTATUS NTAPI NativeOpen(PHANDLE handle, ACCESS_MASK access, POBJECT_ATTRIBUTES attributes,
    PIO_STATUS_BLOCK status, PLARGE_INTEGER allocation, ULONG fileAttributes, ULONG share,
    ULONG disposition, ULONG options, PVOID ea, ULONG eaLength) {
  if (beforeOpen && attributes && attributes->ObjectName) {
    beforeOpen(std::wstring(attributes->ObjectName->Buffer, attributes->ObjectName->Length / sizeof(wchar_t)));
  }
  const auto original = reinterpret_cast<NtCreate>(::GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile"));
  return original(handle, access, attributes, status, allocation, fileAttributes, share, disposition, options, ea, eaLength);
}
FARPROC WINAPI Resolve(HMODULE module, LPCSTR name) {
  if (reinterpret_cast<ULONG_PTR>(name) > 0xffff && std::string_view(name) == "NtCreateFile") {
    return reinterpret_cast<FARPROC>(&NativeOpen);
  }
  return ::GetProcAddress(module, name);
}
BOOL WINAPI Flush(HANDLE handle) {
  const BOOL ok = ::FlushFileBuffers(handle);
  if (ok && afterFlush) afterFlush();
  return ok;
}
BOOL WINAPI Query(HANDLE handle, FILE_INFO_BY_HANDLE_CLASS kind, LPVOID buffer, DWORD size) {
  if (beforeEnumerate && (kind == FileIdBothDirectoryInfo || kind == FileIdBothDirectoryRestartInfo)) beforeEnumerate(handle);
  return ::GetFileInformationByHandleEx(handle, kind, buffer, size);
}
struct Reset {
  ~Reset() { afterInspect = {}; beforeOpen = {}; afterFlush = {}; beforeEnumerate = {}; }
};
}
// Compile the real implementation with API-boundary barriers only in this test TU.
// Its symbols satisfy the static core library references, so no duplicate object is pulled in.
#define GetFileInformationByHandle native_barriers::Inspect
#define CreateFileW native_barriers::Open
#define GetProcAddress native_barriers::Resolve
#define FlushFileBuffers native_barriers::Flush
#define GetFileInformationByHandleEx native_barriers::Query
#include "../src/paths/FolderCleanup.cpp"
#undef GetFileInformationByHandleEx
#undef FlushFileBuffers
#undef GetProcAddress
#undef CreateFileW
#undef GetFileInformationByHandle
#endif

namespace {
namespace fs = std::filesystem;
void Expect(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct Fixture {
  fs::path root;
  std::wstring previousLocalAppData;
  bool hadLocalAppData = false;
  std::vector<fs::path> links;
  Fixture() {
    root = fs::temp_directory_path() / (L"modlist-f04-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
    Expect(fs::create_directory(root), "fixture must be exclusively created");
    fs::create_directory(root / "package");
    std::vector<wchar_t> previous(32768);
    const DWORD size = GetEnvironmentVariableW(L"LOCALAPPDATA", previous.data(), static_cast<DWORD>(previous.size()));
    hadLocalAppData = size > 0 && size < previous.size();
    if (hadLocalAppData) previousLocalAppData.assign(previous.data(), size);
    fs::create_directory(root / "localappdata");
    Expect(SetEnvironmentVariableW(L"LOCALAPPDATA", (root / "localappdata").c_str()) != FALSE, "private runtime environment setup failed");
  }
  ~Fixture() {
    SetEnvironmentVariableW(L"LOCALAPPDATA", hadLocalAppData ? previousLocalAppData.c_str() : nullptr);
    for (auto it = links.rbegin(); it != links.rend(); ++it) RemoveDirectoryW(it->c_str());
    std::error_code ec;
    fs::remove_all(root, ec); // Only this uniquely created fixture; junctions removed above.
  }
  fs::path Folder(const char* name) {
    const auto path = root / name;
    fs::create_directories(path);
    return path;
  }
  fs::path File(const fs::path& folder, const char* name = "sentinel") {
    const auto path = folder / name;
    std::ofstream out(path, std::ios::binary);
    out << "private sentinel bytes";
    out.close();
    Expect(out.good(), "fixture write failed");
    return path;
  }
  bool TryJunction(const fs::path& link, const fs::path& target, bool attributesOnly = false) {
    Expect(target.native().starts_with(root.native()), "junction target must be owned fixture");
    const auto substitute = L"\\??\\" + target.wstring();
    const auto print = target.wstring();
    struct MountPoint {
      DWORD tag;
      WORD dataLength, reserved, substituteOffset, substituteLength, printOffset, printLength;
      wchar_t names[16384];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.printOffset = data.substituteLength + sizeof(wchar_t);
    data.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
    std::copy(substitute.begin(), substitute.end(), data.names);
    std::copy(print.begin(), print.end(), data.names + substitute.size() + 1);
    data.dataLength = 8 + data.printOffset + data.printLength + sizeof(wchar_t);
    HANDLE handle = CreateFileW(link.c_str(), attributesOnly ? FILE_WRITE_ATTRIBUTES : GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Expect(handle != INVALID_HANDLE_VALUE, "junction open failed");
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
        data.dataLength + 8, nullptr, 0, &returned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (ok) links.push_back(link);
    SetLastError(error);
    return ok != FALSE;
  }
  void SetJunction(const fs::path& link, const fs::path& target, bool attributesOnly = false) {
    Expect(TryJunction(link, target, attributesOnly), "junction creation failed");
  }
  void Junction(const fs::path& link, const fs::path& target) {
    Expect(fs::create_directory(link), "junction path must be new");
    SetJunction(link, target);
  }
};

void TestRootJunction() {
  Fixture f;
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  const auto link = f.root / "Unpacking";
  f.Junction(link, outside);
#ifdef FOLDER_CLEANUP_LEGACY_REPRO
  // Exact vulnerable pattern: enumerate the root, then remove_all each entry.
  std::vector<fs::path> entries;
  for (const auto& entry : fs::directory_iterator(link)) entries.push_back(entry.path());
  for (const auto& entry : entries) fs::remove_all(entry);
#else
  const auto ticket = modlist::PrepareFolderCleanup(link, f.root / "package",
      modlist::FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(!ticket.ok(), "linked cleanup root must be rejected");
#endif
  Expect(fs::exists(sentinel), "external sentinel was deleted through root junction");
}

#ifndef FOLDER_CLEANUP_LEGACY_REPRO
using modlist::FolderCleanupKind;
void TestSafeFinalOverwrite() {
  Fixture f;
  const auto folder = f.Folder("final");
  f.File(f.Folder("final/sub"));
  f.File(folder);
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(ticket.ok(), "unmarked explicit final overwrite must remain supported");
  Expect(modlist::ClearFolderContents(ticket.value()).ok(), "safe clear failed");
  Expect(fs::is_empty(folder), "safe clear must retain empty root");
}
void TestUnownedStaging() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  const auto sentinel = f.File(folder);
  Expect(!modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::OwnedStaging, "package-v1").ok(), "unowned staging accepted");
  Expect(!modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1").ok(),
      "nonempty staging adopted");
  Expect(fs::exists(sentinel), "unowned staging changed");
}
void TestOwnedStaging() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "empty staging claim failed");
  const auto sentinel = f.File(folder);
  Expect(!modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::OwnedStaging, "package-v1").ok(), "active staging accepted");
  lease.value().reset();
  Expect(!modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::OwnedStaging, "different-package").ok(), "wrong package accepted");
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::OwnedStaging, "package-v1");
  Expect(ticket.ok(), "owned stopped staging rejected");
  Expect(modlist::ClearFolderContents(ticket.value()).ok(), "owned staging clear failed");
  Expect(!fs::exists(sentinel) && fs::is_empty(folder), "owned staging not cleared");
  auto next = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(next.ok(), "cleared staging cannot be reused");
  Expect(next.value()->Complete().ok(), "staging completion failed");
  next.value().reset();
  Expect(!fs::exists(folder) && !fs::exists(modlist::StagingOwnerPath(folder)),
      "owned empty staging root or sidecar leaked on completion");
}
void TestMarkerOnlyRecovery() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "marker-only initial claim failed");
  std::error_code ec;
  fs::rename(folder, f.root / "replacement", ec);
  Expect(static_cast<bool>(ec) && fs::exists(folder), "active staging root was not pinned");
  fs::rename(f.root, f.root.parent_path() / (f.root.filename().wstring() + L"-moved"), ec);
  Expect(static_cast<bool>(ec) && fs::exists(f.root), "active staging ancestor was not pinned");
  lease.value().reset();
  auto recovered = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(recovered.ok(), "owned marker-only staging should be reusable without payload cleanup");
  Expect(recovered.value()->Complete().ok(), "recovered staging completion failed");
}
void TestOwnershipNameIsPayload() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "payload collision setup failed");
  Expect(fs::is_empty(folder), "ownership metadata must stay outside the payload namespace");
  const auto payload = f.File(folder, ".modlist-staging-owner");
  const auto installed = f.Folder("final") / payload.filename();
  fs::rename(payload, installed);
  Expect(lease.value()->Complete().ok(), "payload collision completion failed");
  lease.value().reset();
  Expect(fs::is_regular_file(installed) && fs::file_size(installed) == 22,
      "legitimate ownership-like payload was changed or removed");
}
void TestForeignSidecar() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  const auto sidecar = modlist::StagingOwnerPath(folder);
  fs::create_directories(sidecar.parent_path());
  f.File(sidecar.parent_path(), sidecar.filename().string().c_str());
  Expect(!modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1").ok(),
      "unrelated sidecar was adopted");
  Expect(fs::file_size(sidecar) == 22 && fs::is_empty(folder), "unrelated sidecar changed");
}
void TestOwnedRootReplacement() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "owned replacement setup failed");
  lease.value().reset();
  fs::rename(folder, f.root / "previous-staging");
  fs::create_directory(folder);
  const auto sentinel = f.File(folder);
  Expect(!modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::OwnedStaging, "package-v1").ok(), "stale sidecar adopted replacement root");
  Expect(fs::exists(sentinel), "stale ownership deleted replacement sentinel");
}
void TestIncompleteSessionPreservesPayload() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "incomplete session setup failed");
  const auto payload = f.File(f.Folder("Unpacking/nested"));
  Expect(!lease.value()->Complete().ok(), "nonempty staging completion accepted");
  lease.value().reset();
  Expect(fs::exists(payload) && fs::exists(modlist::StagingOwnerPath(folder)),
      "incomplete staging lost payload or recovery ownership");
}
void TestIncompleteSessionRejectsLinkedRemainder() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "linked staging fixture setup failed");
  const auto outside = f.Folder("outside");
  const auto nested = f.Folder("Unpacking/nested");
  f.Junction(nested / "linked", outside);
  Expect(!lease.value()->Complete().ok(), "a linked directory was treated as an empty staging remainder");
  lease.value().reset();
  Expect(fs::is_directory(nested / "linked") && fs::is_empty(outside) &&
      fs::exists(modlist::StagingOwnerPath(folder)), "refusing a linked remainder changed the owned tree");
}
bool IsOwnershipDirectory(HANDLE handle) {
  wchar_t name[1024]{};
  const DWORD length = GetFinalPathNameByHandleW(handle, name, 1024, VOLUME_NAME_DOS);
  return length > 0 && length < 1024 && fs::path(name).filename() == L"staging-owner";
}
void TestRemainderChangesAfterScan() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "stage remainder race setup failed");
  const auto nested = f.Folder("Unpacking/nested");
  const auto lateFile = nested / "late.txt";
  native_barriers::Reset reset;
  native_barriers::beforeEnumerate = [&](HANDLE handle) {
    if (IsOwnershipDirectory(handle) && !fs::exists(lateFile)) {
      f.File(nested, "late.txt");
    }
  };
  const auto completed = lease.value()->Complete();
  if (!fs::is_regular_file(lateFile)) throw std::runtime_error("the late staging file was not added: " + completed.error());
  Expect(!completed.ok(), "staging changed after inspection but reported success");
  lease.value().reset();
  Expect(fs::exists(modlist::StagingOwnerPath(folder)), "staging race lost the ownership record");
}
void TestStagingAncestorRecheckedAfterOwnership() {
  Fixture f;
  const auto folder = f.Folder("parent/Unpacking");
  const auto parent = folder.parent_path();
  auto lease = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(lease.ok(), "staging ancestor setup failed");
  f.Folder("parent/Unpacking/nested");

  const HANDLE parentHandle = CreateFileW(parent.c_str(), FILE_READ_ATTRIBUTES,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  BY_HANDLE_FILE_INFORMATION parentInfo{};
  const bool identified = parentHandle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(parentHandle, &parentInfo);
  if (parentHandle != INVALID_HANDLE_VALUE) CloseHandle(parentHandle);
  Expect(identified, "could not identify the staging parent");

  native_barriers::Reset reset;
  bool checkedOwnership = false;
  native_barriers::beforeEnumerate = [&](HANDLE handle) {
    checkedOwnership = checkedOwnership || IsOwnershipDirectory(handle);
  };
  // Model a changed ancestor attribute at the query boundary without redirecting any path.
  native_barriers::afterInspect = [&](HANDLE, BY_HANDLE_FILE_INFORMATION& info) {
    if (checkedOwnership && info.dwVolumeSerialNumber == parentInfo.dwVolumeSerialNumber &&
        info.nFileIndexHigh == parentInfo.nFileIndexHigh && info.nFileIndexLow == parentInfo.nFileIndexLow) {
      info.dwFileAttributes |= FILE_ATTRIBUTE_REPARSE_POINT;
    }
  };
  const auto completed = lease.value()->Complete();
  Expect(checkedOwnership, "staging completion did not check ownership");
  Expect(!completed.ok(), "staging did not recheck a changed ancestor after ownership validation");
  lease.value().reset();
  Expect(fs::exists(modlist::StagingOwnerPath(folder)), "ancestor refusal lost ownership");
}
void TestLinkedAncestor() {
  Fixture f;
  const auto outside = f.Folder("unrelated/sub");
  const auto sentinel = f.File(outside);
  f.Junction(f.root / "linked-parent", outside.parent_path());
  Expect(!modlist::PrepareFolderCleanup(f.root / "linked-parent/sub", f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite).ok(), "linked ancestor accepted");
  Expect(fs::exists(sentinel), "ancestor sentinel changed");
}
void TestChangedIdentity() {
  Fixture f;
  const auto folder = f.Folder("final");
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(ticket.ok(), "identity preparation failed");
  fs::rename(folder, f.root / "old-final");
  fs::create_directory(folder);
  const auto sentinel = f.File(folder);
  Expect(!modlist::ClearFolderContents(ticket.value()).ok(), "replacement root accepted");
  Expect(fs::exists(sentinel), "replacement sentinel changed");
}
void TestNestedJunction() {
  Fixture f;
  const auto folder = f.Folder("final");
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  const auto local = f.File(folder, "local-sentinel");
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(ticket.ok(), "nested preparation failed");
  f.Junction(folder / "linked-child", outside);
  Expect(!modlist::ClearFolderContents(ticket.value()).ok(), "nested junction accepted");
  Expect(fs::exists(sentinel) && fs::exists(local), "nested preflight mutated files");
}
void TestPackageOverlap() {
  Fixture f;
  const auto sentinel = f.File(f.root / "package");
  Expect(!modlist::PrepareFolderCleanup(f.root, f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite).ok(), "package ancestor accepted");
  Expect(!modlist::PrepareFolderCleanup(f.root / "package", f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite).ok(), "package itself accepted");
  f.Junction(f.root / "package-alias", f.root / "package");
  Expect(!modlist::PrepareFolderCleanup(f.root / "package-alias", f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite).ok(), "package link overlap accepted");
  Expect(fs::exists(sentinel), "package sentinel changed");
}
void TestVolumeRoot() {
  Fixture f;
  // Read-only preparation only; no deletion and no junction to a real drive root.
  Expect(!modlist::PrepareFolderCleanup(f.root.root_path(), f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite).ok(), "volume root accepted");
  Expect(!modlist::PrepareFolderCleanup(L"\\\\modlist-f04-nonexistent-server\\share", f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite).ok(), "share root accepted");
}
void TestPinnedRootAttributeMutation() {
  Fixture f;
  const auto folder = f.Folder("final");
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(ticket.ok(), "root mutation setup failed");
  BY_HANDLE_FILE_INFORMATION rootInfo{};
  HANDLE root = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  Expect(root != INVALID_HANDLE_VALUE && GetFileInformationByHandle(root, &rootInfo), "root identity setup failed");
  CloseHandle(root);
  native_barriers::Reset reset;
  int inspected = 0;
  bool mutated = false;
  native_barriers::afterInspect = [&](HANDLE, const auto& info) {
    if (info.dwVolumeSerialNumber == rootInfo.dwVolumeSerialNumber && info.nFileIndexHigh == rootInfo.nFileIndexHigh &&
        info.nFileIndexLow == rootInfo.nFileIndexLow && ++inspected == 2) {
      f.SetJunction(folder, outside, true);
      mutated = true;
    }
  };
  const auto cleared = modlist::ClearFolderContents(ticket.value());
  Expect(mutated, "root FILE_WRITE_ATTRIBUTES mutation barrier did not run");
  Expect(fs::exists(sentinel), "pinned root mutation deleted an external sentinel");
  Expect(!cleared.ok(), "mutated root cleanup should fail closed");
}
void TestPinnedChildAttributeMutation() {
  Fixture f;
  const auto folder = f.Folder("final");
  const auto child = f.Folder("final/empty-child");
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(ticket.ok(), "child mutation setup failed");
  BY_HANDLE_FILE_INFORMATION childInfo{};
  HANDLE handle = CreateFileW(child.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  Expect(handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &childInfo), "child identity setup failed");
  CloseHandle(handle);
  native_barriers::Reset reset;
  bool mutated = false;
  native_barriers::afterInspect = [&](HANDLE, const auto& info) {
    if (!mutated && info.dwVolumeSerialNumber == childInfo.dwVolumeSerialNumber &&
        info.nFileIndexHigh == childInfo.nFileIndexHigh && info.nFileIndexLow == childInfo.nFileIndexLow) {
      mutated = true;
      f.SetJunction(child, outside, true);
    }
  };
  const auto cleared = modlist::ClearFolderContents(ticket.value());
  Expect(mutated, "child FILE_WRITE_ATTRIBUTES mutation barrier did not run");
  Expect(fs::exists(sentinel), "pinned child mutation deleted an external sentinel");
  Expect(!cleared.ok(), "mutated child cleanup should fail closed");
}
void TestPinnedAncestorAttributeMutation() {
  Fixture f;
  const auto parent = f.Folder("parent");
  const auto outside = f.Folder("unrelated/victim");
  const auto sentinel = f.File(outside);
  native_barriers::Reset reset;
  bool mutated = false;
  native_barriers::beforeOpen = [&](const std::wstring& name) {
    if (!mutated && name == L"victim") {
      mutated = true;
      f.SetJunction(parent, outside.parent_path(), true);
    }
  };
  const auto ticket = modlist::PrepareFolderCleanup(parent / "victim", f.root / "package",
      FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(mutated, "ancestor mutation barrier did not run");
  Expect(!ticket.ok(), "absolute ancestor reopen followed a newly installed junction");
  Expect(fs::exists(sentinel), "ancestor mutation sentinel changed");
}
void TestStagingClaimRace() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  native_barriers::Reset reset;
  fs::path sentinel;
  native_barriers::afterFlush = [&] { if (sentinel.empty()) sentinel = f.File(folder); };
  auto session = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(!sentinel.empty(), "staging persistence barrier did not run");
  Expect(!session.ok(), "staging claim adopted content arriving before ownership completed");
  Expect(fs::exists(sentinel), "raced staging content was deleted");
  Expect(!fs::exists(modlist::StagingOwnerPath(folder)), "failed new claim left valid ownership for unrelated content");
}
void TestMutationAtHandleEnumeration() {
  Fixture f;
  const auto folder = f.Folder("final");
  const auto child = f.Folder("final/empty-child");
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(ticket.ok(), "enumeration mutation setup failed");
  BY_HANDLE_FILE_INFORMATION childInfo{};
  HANDLE handle = CreateFileW(child.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  Expect(handle != INVALID_HANDLE_VALUE && GetFileInformationByHandle(handle, &childInfo), "enumeration identity setup failed");
  CloseHandle(handle);
  native_barriers::Reset reset;
  bool mutated = false;
  native_barriers::beforeEnumerate = [&](HANDLE current) {
    BY_HANDLE_FILE_INFORMATION info{};
    Expect(GetFileInformationByHandle(current, &info) != FALSE, "enumeration barrier query failed");
    if (!mutated && info.dwVolumeSerialNumber == childInfo.dwVolumeSerialNumber &&
        info.nFileIndexHigh == childInfo.nFileIndexHigh && info.nFileIndexLow == childInfo.nFileIndexLow) {
      mutated = true;
      f.SetJunction(child, outside, true);
    }
  };
  const auto cleared = modlist::ClearFolderContents(ticket.value());
  Expect(mutated, "native handle enumeration barrier did not run");
  Expect(fs::exists(sentinel), "handle enumeration followed a converted directory");
  Expect(!cleared.ok(), "enumeration-time reparse mutation should fail closed");
}
void TestMutationAtSidecarOpen() {
  Fixture f;
  const auto folder = f.Folder("parent/Unpacking");
  const auto parent = modlist::StagingOwnerPath(folder).parent_path();
  fs::create_directories(parent);
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  native_barriers::Reset reset;
  bool mutated = false;
  native_barriers::beforeOpen = [&](const std::wstring& name) {
    if (!mutated && name == modlist::StagingOwnerPath(folder).filename()) {
      mutated = true;
      f.SetJunction(parent, outside, true);
    }
  };
  auto session = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(mutated, "native cache parent mutation barrier did not run");
  Expect(!session.ok(), "staging with a converted cache parent was accepted");
  Expect(fs::exists(sentinel), "sidecar parent mutation changed external sentinel");
  Expect(!fs::exists(outside / modlist::StagingOwnerPath(folder).filename()), "sidecar was created through a redirected parent");
}
void TestFlatEntryBudget() {
  Fixture f;
  const auto folder = f.Folder("final");
  for (int i = 0; i < 5; ++i) f.File(folder, ("file-" + std::to_string(i)).c_str());
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::ExplicitFinalOverwrite);
  Expect(ticket.ok(), "entry budget setup failed");
  modlist::FolderCleanupLimits limits;
  limits.maxEntries = 3;
  native_barriers::Reset reset;
  size_t acquired = 0;
  native_barriers::beforeOpen = [&](const std::wstring& name) { if (name.starts_with(L"file-")) ++acquired; };
  Expect(!modlist::ClearFolderContents(ticket.value(), limits).ok(), "flat directory exceeded the per-entry budget");
  Expect(acquired == 3, "entry budget was not enforced before each native acquisition");
  Expect(std::distance(fs::directory_iterator(folder), fs::directory_iterator{}) == 5,
      "entry-budget failure deleted files during preflight");
}
struct PrivateRootAcl {
  fs::path path;
  PSECURITY_DESCRIPTOR previous = nullptr;
  PACL previousDacl = nullptr;
  explicit PrivateRootAcl(const fs::path& ownedPath) : path(ownedPath) {
    Expect(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &previousDacl, nullptr, &previous) == ERROR_SUCCESS, "private ACL snapshot failed");
    HANDLE token = nullptr;
    Expect(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE, "private ACL token open failed");
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> user(bytes);
    const BOOL read = GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes);
    CloseHandle(token);
    Expect(read != FALSE, "private ACL user query failed");
    const auto sid = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
    EXPLICIT_ACCESSW rules[3]{};
    for (auto& rule : rules) {
      rule.Trustee.TrusteeForm = TRUSTEE_IS_SID;
      rule.Trustee.TrusteeType = TRUSTEE_IS_USER;
      rule.Trustee.ptstrName = static_cast<LPWSTR>(sid);
    }
    rules[0].grfAccessMode = DENY_ACCESS;
    rules[0].grfAccessPermissions = FILE_ADD_FILE;
    rules[1].grfAccessMode = GRANT_ACCESS;
    rules[1].grfAccessPermissions = FILE_ALL_ACCESS & ~FILE_ADD_FILE;
    rules[2].grfAccessMode = GRANT_ACCESS;
    rules[2].grfAccessPermissions = FILE_ALL_ACCESS;
    rules[2].grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT | INHERIT_ONLY;
    PACL acl = nullptr;
    Expect(SetEntriesInAclW(3, rules, nullptr, &acl) == ERROR_SUCCESS, "private ACL construction failed");
    const DWORD applied = SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    Expect(applied == ERROR_SUCCESS, "private ACL apply failed");
  }
  ~PrivateRootAcl() {
    SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, previousDacl, nullptr);
    LocalFree(previous);
  }
};
void TestCreateDirectoriesOnlyParentAcl() {
  Fixture f;
  const auto parent = f.Folder("restricted-parent");
  PrivateRootAcl acl(parent);
  HANDLE file = CreateFileW((parent / "denied-file").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  const DWORD denied = GetLastError();
  if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
  Expect(file == INVALID_HANDLE_VALUE && denied == ERROR_ACCESS_DENIED, "controlled parent must deny regular file creation");
  const auto folder = parent / "Unpacking";
  Expect(CreateDirectoryW(folder.c_str(), nullptr) != FALSE, "controlled parent must permit subdirectory creation");
  const auto child = f.File(folder, "child-access-proof");
  Expect(fs::remove(child), "inherited child write/delete permission missing");
  auto session = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(session.ok(), "staging ownership fails when parent permits directories but denies regular files");
  const auto payload = f.File(folder, "interrupted-payload");
  session.value().reset();
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::OwnedStaging, "package-v1");
  Expect(ticket.ok(), "restricted-parent owned recovery preparation failed");
  Expect(modlist::ClearFolderContents(ticket.value()).ok() && !fs::exists(payload), "restricted-parent owned recovery clear failed");
  auto recovered = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(recovered.ok() && recovered.value()->Complete().ok(), "restricted-parent staging completion failed");
  recovered.value().reset();
  Expect(!fs::exists(folder) && !fs::exists(modlist::StagingOwnerPath(folder)), "restricted-parent staging metadata leaked");
}
void TestForeignMetadataDirectory() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  const auto metadata = modlist::StagingOwnerPath(folder);
  fs::create_directories(metadata.parent_path());
  Expect(fs::create_directory(metadata), "foreign metadata setup failed");
  const auto sentinel = f.File(metadata);
  Expect(!modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1").ok(), "unowned metadata directory was adopted");
  Expect(fs::file_size(sentinel) == 22 && !fs::exists(metadata / "owner"), "foreign metadata directory was changed");
}
void TestLinkedMetadataDirectory() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  fs::create_directories(modlist::StagingOwnerPath(folder).parent_path());
  f.Junction(modlist::StagingOwnerPath(folder), outside);
  Expect(!modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1").ok(), "linked metadata directory accepted");
  Expect(fs::file_size(sentinel) == 22 && !fs::exists(outside / "owner"), "linked metadata target changed");
}
void TestMetadataContentRace() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  const auto metadata = modlist::StagingOwnerPath(folder);
  native_barriers::Reset reset;
  fs::path sentinel;
  native_barriers::beforeOpen = [&](const std::wstring& name) {
    if (name == L"owner" && sentinel.empty()) sentinel = f.File(metadata, "raced-content");
  };
  auto session = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(!sentinel.empty(), "metadata creation race barrier did not run");
  Expect(!session.ok(), "raced metadata contents were adopted");
  Expect(fs::file_size(sentinel) == 22, "raced metadata contents were deleted");
  Expect(!fs::exists(metadata / "owner"), "failed new metadata claim left valid ownership");
}
void TestMetadataAttributeMutation() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  const auto metadata = modlist::StagingOwnerPath(folder);
  const auto outside = f.Folder("unrelated");
  const auto sentinel = f.File(outside);
  native_barriers::Reset reset;
  bool mutated = false;
  native_barriers::beforeOpen = [&](const std::wstring& name) {
    if (name == L"owner" && !mutated) {
      mutated = true;
      f.SetJunction(metadata, outside, true);
    }
  };
  auto session = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(mutated, "new metadata attribute mutation barrier did not run");
  Expect(!session.ok(), "converted metadata directory was accepted");
  Expect(fs::file_size(sentinel) == 22 && !fs::exists(outside / "owner"), "metadata mutation escaped its anchored handle");
}
int PendingCrashChild(const fs::path& folder, const fs::path& package, const wchar_t* eventName, bool inject) {
  HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
  if (!ready) return 10;
  bool first = true;
  native_barriers::afterFlush = [&] {
    if (!std::exchange(first, false)) return;
    if (inject) {
      std::ofstream out(folder / "unowned-sentinel", std::ios::binary);
      out << "private sentinel bytes";
      out.close();
      if (!out.good()) ExitProcess(11);
    }
    if (!SetEvent(ready)) ExitProcess(12);
    Sleep(INFINITE); // The parent MUST terminate us here; no destructors/invalidation run.
  };
  const auto session = modlist::BeginOwnedStaging(folder, package, "package-v1");
  CloseHandle(ready);
  return session.ok() ? 13 : 14;
}
void KillAfterInitialFlush(const fs::path& folder, const fs::path& package, bool inject) {
  std::vector<wchar_t> module(32768);
  Expect(GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size())) != 0, "crash child executable lookup failed");
  const auto eventName = L"Local\\modlist-f04-crash-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
      std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count());
  HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, eventName.c_str());
  Expect(ready != nullptr, "crash child event creation failed");
  auto command = L"\"" + std::wstring(module.data()) + L"\" --pending-crash-child \"" + folder.wstring() +
      L"\" \"" + package.wstring() + L"\" \"" + eventName + L"\" " + (inject ? L"inject" : L"empty");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION child{};
  const BOOL started = CreateProcessW(module.data(), command.data(), nullptr, nullptr, FALSE,
      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child);
  if (!started) { CloseHandle(ready); throw std::runtime_error("crash child launch failed"); }
  CloseHandle(child.hThread);
  const HANDLE waits[]{ready, child.hProcess};
  const DWORD reached = WaitForMultipleObjects(2, waits, FALSE, 15000);
  const BOOL terminated = TerminateProcess(child.hProcess, 109);
  const DWORD stopped = WaitForSingleObject(child.hProcess, 10000);
  CloseHandle(child.hProcess);
  CloseHandle(ready);
  Expect(reached == WAIT_OBJECT_0, "crash child never reached the initial durable-record barrier");
  Expect(terminated != FALSE && stopped == WAIT_OBJECT_0, "crash child was not terminated at the barrier");
}
void TestKilledPendingClaimPreservesForeignData() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  KillAfterInitialFlush(folder, f.root / "package", true);
  const auto sentinel = folder / "unowned-sentinel";
  Expect(fs::file_size(sentinel) == 22, "kill regression did not inject private unowned data");
  const auto ticket = modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::OwnedStaging, "package-v1");
  Expect(!ticket.ok(), "killed initial claim left valid ownership authorizing deletion of foreign data");
  Expect(!modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1").ok(), "nonempty pending claim was adopted");
  Expect(fs::file_size(sentinel) == 22, "nonempty pending recovery changed foreign data");
}
void TestKilledPendingEmptyClaimRecovery() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  KillAfterInitialFlush(folder, f.root / "package", false);
  Expect(!modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::OwnedStaging, "package-v1").ok(),
      "pending record authorizes cleanup before committed ownership");
  auto session = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(session.ok() && session.value()->Complete().ok(), "empty pending claim cannot be safely reclaimed");
}
void TestOldMetadataNameIsFinalDestination() {
  Fixture f;
  const auto parent = f.Folder("install-root");
  const auto folder = f.Folder("install-root/Unpacking");
  const auto final = parent / ".Unpacking.modlist-staging-owner";
  Expect(modlist::StagingOwnerPath(folder) != final, "staging metadata collides with a valid final archive name");
  auto session = modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1");
  Expect(session.ok(), "valid old-metadata-name setup failed");
  Expect(fs::create_directory(final), "valid final archive folder was occupied by metadata");
  Expect(session.value()->Complete().ok(), "valid old-metadata-name staged completion failed");
  Expect(fs::is_directory(final), "final destination was deleted as staging metadata");
}
void TestTruncatedPendingCannotAuthorizeCleanup() {
  Fixture f;
  const auto folder = f.Folder("Unpacking");
  KillAfterInitialFlush(folder, f.root / "package", false);
  const auto marker = modlist::StagingOwnerPath(folder) / "owner";
  fs::resize_file(marker, 4);
  Expect(!modlist::PrepareFolderCleanup(folder, f.root / "package", FolderCleanupKind::OwnedStaging, "package-v1").ok(),
      "truncated pending record authorized cleanup");
  Expect(!modlist::BeginOwnedStaging(folder, f.root / "package", "package-v1").ok(), "truncated unbound pending record was adopted");
  Expect(fs::file_size(marker) == 4, "unrecognized pending record was changed");
}
void TestRuntimeNamespaceOverlap() {
  Fixture f;
  const auto runtime = f.Folder("localappdata/ModlistInstaller");
  const auto sentinel = f.File(runtime);
  Expect(modlist::InstallationOverlapsRuntime(runtime), "exact runtime root accepted");
  Expect(modlist::InstallationOverlapsRuntime(runtime / "nested/final"), "runtime descendant accepted");
  Expect(modlist::InstallationOverlapsRuntime(runtime.parent_path()), "runtime ancestor accepted");
  Expect(!modlist::InstallationOverlapsRuntime(runtime.parent_path() / "ModlistInstaller-payload"), "runtime prefix sibling falsely blocked");
  Expect(!modlist::InstallationOverlapsRuntime(f.root / ".Unpacking.modlist-staging-owner"), "valid old metadata archive name falsely reserved");
  Expect(!modlist::PrepareFolderCleanup(runtime, f.root / "package", FolderCleanupKind::ExplicitFinalOverwrite).ok(),
      "explicit overwrite can clear reserved runtime data");
  const auto nested = f.Folder("localappdata/ModlistInstaller/nested");
  Expect(!modlist::BeginOwnedStaging(nested, f.root / "package", "package-v1").ok(), "staging can occupy runtime space");
  f.Junction(f.root / "runtime-alias", runtime);
  Expect(modlist::InstallationOverlapsRuntime(f.root / "runtime-alias/nested"), "runtime namespace alias accepted");
  Expect(fs::file_size(sentinel) == 22, "runtime namespace sentinel changed");
}
#endif
} // namespace

int wmain(int argc, wchar_t** argv) {
  try {
#ifndef FOLDER_CLEANUP_LEGACY_REPRO
    if (argc == 6 && std::wstring_view(argv[1]) == L"--pending-crash-child") {
      return PendingCrashChild(argv[2], argv[3], argv[4], std::wstring_view(argv[5]) == L"inject");
    }
#endif
    int failures = 0;
    auto run = [&](const char* name, auto test) {
      try { test(); std::cout << "PASS: " << name << '\n'; }
      catch (const std::exception& e) { ++failures; std::cerr << "FAILED: " << name << ": " << e.what() << '\n'; }
    };
    run("root junction sentinel", TestRootJunction);
#ifndef FOLDER_CLEANUP_LEGACY_REPRO
    run("safe explicit final overwrite without ownership", TestSafeFinalOverwrite);
    run("unowned staging refusal", TestUnownedStaging);
    run("owned staging active/wrong-package/recovery cleanup", TestOwnedStaging);
    run("sidecar-only recovery and root/ancestor pinning", TestMarkerOnlyRecovery);
    run("ownership-like filename remains payload", TestOwnershipNameIsPayload);
    run("foreign sidecar preservation", TestForeignSidecar);
    run("stale sidecar rejects replaced root", TestOwnedRootReplacement);
    run("incomplete session retains payload and ownership", TestIncompleteSessionPreservesPayload);
    run("linked staging remainder is not a successful cleanup warning", TestIncompleteSessionRejectsLinkedRemainder);
    run("staging change after scan refuses completion", TestRemainderChangesAfterScan);
    run("staging ancestor rechecked after ownership validation", TestStagingAncestorRecheckedAfterOwnership);
    run("linked ancestor sentinel", TestLinkedAncestor);
    run("identity replacement sentinel", TestChangedIdentity);
    run("nested junction all-or-nothing preflight", TestNestedJunction);
    run("canonical package overlap", TestPackageOverlap);
    run("volume root read-only refusal", TestVolumeRoot);
    run("pinned root attribute mutation sentinel", TestPinnedRootAttributeMutation);
    run("pinned child attribute mutation sentinel", TestPinnedChildAttributeMutation);
    run("pinned ancestor attribute mutation no-follow", TestPinnedAncestorAttributeMutation);
    run("staging claim race refuses adoption", TestStagingClaimRace);
    run("mutation immediately before handle enumeration", TestMutationAtHandleEnumeration);
    run("mutation immediately before relative sidecar open", TestMutationAtSidecarOpen);
    run("flat directory entry budget", TestFlatEntryBudget);
    run("create-directories-only parent ACL", TestCreateDirectoriesOnlyParentAcl);
    run("foreign metadata directory preservation", TestForeignMetadataDirectory);
    run("linked metadata directory no-follow", TestLinkedMetadataDirectory);
    run("metadata creation race preservation", TestMetadataContentRace);
    run("metadata attribute mutation no-follow", TestMetadataAttributeMutation);
    run("subprocess kill pending claim preserves foreign data", TestKilledPendingClaimPreservesForeignData);
    run("subprocess kill pending empty claim recovery", TestKilledPendingEmptyClaimRecovery);
    run("old metadata name remains valid final destination", TestOldMetadataNameIsFinalDestination);
    run("truncated pending record refuses cleanup and adoption", TestTruncatedPendingCannotAuthorizeCleanup);
    run("runtime namespace exact ancestor descendant alias protection", TestRuntimeNamespaceOverlap);
#endif
    if (failures) return 1;
    std::cout << "Folder cleanup tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAILED: " << e.what() << '\n';
    return 1;
  }
}
