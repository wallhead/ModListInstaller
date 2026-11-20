#include "patch/PatchInstaller.h"

#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace modlist::patch;
using modlist::Result;

void Require(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

void Put(const fs::path& path, std::string_view data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  out.close();
  Require(out.good(), "Cannot write generated fixture file");
}

std::string Get(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(in.is_open(), "Missing fixture file");
  return {std::istreambuf_iterator<char>(in), {}};
}

struct Fixture {
  fs::path home, root, package;
  std::vector<fs::path> links;
  Fixture() {
    static unsigned sequence = 0;
    home = fs::temp_directory_path() / (L"mi-patch-tests-" + std::to_wstring(GetCurrentProcessId()) +
        L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence));
    Require(fs::create_directory(home), "Fixture directory must be new");
    root = home / L"Renamed TRUEAE Sample";
    package = home / L"Patch files";
    fs::create_directory(package);
    Put(root / L"ModOrganizer.exe", "dummy mod organizer");
    Put(root / L"Stock Game\\SkyrimSE.exe", "dummy game");
    Put(root / L"Stock Game\\Data\\obsolete.ini", "obsolete ini");
    Put(root / L"Stock Game\\Data\\unchanged.bsa", "unrelated game data");
    Put(root / L"profiles\\V5.4 NO-LORE\\saves\\keep.ess", "unrelated save");
    Put(root / L"mods\\Retired Mod\\textures\\old.dds", "old mod texture");
    Put(root / L"enblocal.ini", "old settings");
    Put(home / L"outside\\keep.txt", "outside");
  }
  ~Fixture() {
    for (auto it = links.rbegin(); it != links.rend(); ++it) {
      if (!RemoveDirectoryW(it->c_str())) return;
    }
    std::error_code ec;
    fs::remove_all(home, ec); // Only the exclusively created test directory.
  }
  std::unique_ptr<PatchFolder> Target() const {
    auto opened = OpenPatchFolder(root / L"Stock Game", package);
    Require(opened.ok(), opened.error());
    return std::move(opened.value());
  }
  void Junction(const fs::path& path) {
    Require(fs::create_directory(path), "Junction path already exists");
    const auto target = (home / L"outside").wstring();
    const auto substitute = L"\\??\\" + target;
    struct MountPoint {
      DWORD tag;
      WORD dataLength, reserved, substituteOffset, substituteLength, printOffset, printLength;
      wchar_t names[2048];
    } data{};
    data.tag = IO_REPARSE_TAG_MOUNT_POINT;
    data.substituteLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
    data.printOffset = data.substituteLength + sizeof(wchar_t);
    data.printLength = static_cast<WORD>(target.size() * sizeof(wchar_t));
    data.dataLength = 8 + data.printOffset + data.printLength + sizeof(wchar_t);
    std::copy(substitute.begin(), substitute.end(), data.names);
    std::copy(target.begin(), target.end(), data.names + substitute.size() + 1);
    const HANDLE directory = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    Require(directory != INVALID_HANDLE_VALUE, "Cannot open fixture junction");
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
        data.dataLength + 8, nullptr, 0, &returned, nullptr);
    CloseHandle(directory);
    Require(ok != FALSE, "Cannot create fixture junction");
    links.push_back(path);
  }
  void BaselineIntact() const {
    Require(Get(root / L"Stock Game\\Data\\obsolete.ini") == "obsolete ini", "Listed file was not restored");
    Require(Get(root / L"mods\\Retired Mod\\textures\\old.dds") == "old mod texture", "Listed folder was not restored");
    Require(Get(root / L"enblocal.ini") == "old settings", "Replaced file was not restored");
    Require(Get(home / L"outside\\keep.txt") == "outside", "Outside file changed");
  }
};

const std::vector<fs::path> kDeletions{L"mods\\Retired Mod", L"Stock Game\\Data\\obsolete.ini", L"missing.ini"};

void ParseQuotedPaths() {
  const auto paths = ParsePatchDeleteList(
      "\"\\mods\\2K Horse Shoulder Harness\\textures\\horse.dds\"\r\n"
      "\\somefile.exe\n\n \"\\Stock Game\\Data\\\xd1\x82\xd0\xb5\xd1\x81\xd1\x82.ini\" \r\n"
      "\\somefile.exe\n");
  Require(paths.ok(), paths.error());
  Require(paths.value() == std::vector<fs::path>{
      L"mods\\2K Horse Shoulder Harness\\textures\\horse.dds",
      L"somefile.exe", L"Stock Game\\Data\\\u0442\u0435\u0441\u0442.ini"}, "Parsed paths are not root-relative");
  Require(ParsePatchDeleteList("\r\n").ok(), "Blank delete list should work");
  const auto spaced = ParsePatchDeleteList(" leading space.txt\r\n");
  Require(spaced.ok() && spaced.value() == std::vector<fs::path>{L" leading space.txt"},
          "Leading filename space was lost");
  const auto unicodePrefix = ParsePatchDeleteList("\xef\xbb\xbf" "file.ini");
  Require(unicodePrefix.ok() && unicodePrefix.value() == std::vector<fs::path>{L"\ufefffile.ini"},
          "Clipboard text lost its first filename character");
}

void RejectUnsafePaths() {
  for (const auto* entry : {
      "\\\\outside\\file", "C:\\outside\\file", "C:relative", "/Windows", "\\", "\\..\\keep.txt",
      "\\mods\\..\\keep.txt", "\\file:stream", "\\mods\\*.dds", "\\foo.", "\\NUL.txt",
      "\"\\mods\\unclosed", "\"\"", "\"\\mods\\Some\"junk\"", "\\.modlist-patch\\state",
      "\\mods\\Some\n\\mods\\Some\\child.txt", "\\mods\\Case\n\\mods\\CASE", "\xff",
      "\\mods\\Retired Mod \n", "trailing tab.txt\t"}) {
    Require(!ParsePatchDeleteList(entry).ok(), std::string("Unsafe delete list accepted: ") + entry);
  }
}

void SelectTheModernRoot() {
  Fixture f;
  auto ticket = f.Target();
  Require(ticket->Root() == f.root, "Registry child did not resolve to the actual root");
  auto manual = OpenPatchFolder(f.root, f.package);
  Require(manual.ok() && manual.value()->Root() == f.root, "Manual root was not accepted");
  Require(!OpenPatchFolder(f.home, f.package).ok(), "An arbitrary parent was accepted");
  Require(!OpenPatchFolder(f.root, f.root / L"Stock Game").ok(), "Patch package inside the target accepted");
}

void DeleteBeforeExtractAndMerge() {
  Fixture f;
  auto ticket = f.Target();
  bool entered = false;
  bool logged = false;
  PatchOptions options;
  options.log = [&](std::string_view) {
    logged = true;
    throw std::runtime_error("simulated log failure");
  };
  auto result = ApplyPatchTransaction(*ticket, kDeletions, [&](const fs::path& output) {
    entered = true;
    Require(!fs::exists(f.root / L"mods\\Retired Mod"), "Archive extractor ran before folder removal");
    Require(!fs::exists(f.root / L"Stock Game\\Data\\obsolete.ini"), "Archive extractor ran before file removal");
    Require(Get(f.root / L"enblocal.ini") == "old settings", "A new file was published too early");
    Put(output / L"enblocal.ini", "new settings");
    Put(output / L"Stock Game\\Data\\patch.esp", "new plugin");
    Put(output / L"mods\\Retired Mod\\new.txt", "replacement subtree");
    fs::create_directories(output / L"mods\\Empty Mod");
    return Result<void>::Ok();
  }, options);
  Require(entered && result.succeeded && logged, "Logging must be attempted without interrupting the patch: " + result.detail);
  Require(Get(f.root / L"enblocal.ini") == "new settings", "Root file was not replaced");
  Require(Get(f.root / L"Stock Game\\Data\\patch.esp") == "new plugin", "New file was not merged");
  Require(Get(f.root / L"mods\\Retired Mod\\new.txt") == "replacement subtree", "Removed tree was not recreated");
  Require(!fs::exists(f.root / L"mods\\Retired Mod\\textures"), "Old subtree contents were not removed");
  Require(fs::is_directory(f.root / L"mods\\Empty Mod"), "Empty archive directory lost");
  Require(!fs::exists(f.root / L"Stock Game\\Data\\obsolete.ini"), "Deleted file came back");
  Require(Get(f.root / L"Stock Game\\Data\\unchanged.bsa") == "unrelated game data", "Unlisted file changed");
  Require(Get(f.root / L"profiles\\V5.4 NO-LORE\\saves\\keep.ess") == "unrelated save", "Save changed");
  Require(Get(f.home / L"outside\\keep.txt") == "outside", "Outside file changed");
  Require(!fs::exists(f.root / kWorkFolder), "Successful patch left temporary data");
}

void RestoreWhenExtractionFails() {
  Fixture f;
  auto ticket = f.Target();
  auto result = ApplyPatchTransaction(*ticket, kDeletions, [&](const fs::path& output) {
    Put(output / L"enblocal.ini", "partial archive output");
    return Result<void>::Error("simulated 7-Zip failure");
  });
  Require(!result.succeeded && result.detail.find("simulated 7-Zip failure") != std::string::npos, result.detail);
  f.BaselineIntact();
  Require(!fs::exists(f.root / kWorkFolder), "Rollback left staging behind");
}

void RestoreWhenReplacementIsLocked() {
  Fixture f;
  Put(f.root / L"z-locked.ini", "locked original");
  auto ticket = f.Target();
  const auto handle = CreateFileW((f.root / L"z-locked.ini").c_str(), GENERIC_READ, FILE_SHARE_READ,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(handle != INVALID_HANDLE_VALUE, "Cannot lock the fixture file");
  const auto result = ApplyPatchTransaction(*ticket, kDeletions, [&](const fs::path& output) {
    Put(output / L"enblocal.ini", "new settings");
    Put(output / L"a-new.txt", "new file");
    Put(output / L"z-locked.ini", "must not overwrite open file");
    return Result<void>::Ok();
  });
  CloseHandle(handle);
  Require(!result.succeeded, "A locked destination was overwritten");
  f.BaselineIntact();
  Require(Get(f.root / L"z-locked.ini") == "locked original", "Locked file changed");
  Require(!fs::exists(f.root / L"a-new.txt"), "Rollback left a newly published file");
  Require(!fs::exists(f.root / kWorkFolder), result.detail);
}

void LeaveRedirectedFilesAlone() {
  Fixture f;
  f.Junction(f.root / L"mods\\junction");
  auto ticket = f.Target();
  const std::vector<fs::path> paths{L"Stock Game\\Data\\obsolete.ini", L"mods\\junction\\keep.txt"};
  bool extracted = false;
  const auto result = ApplyPatchTransaction(*ticket, paths, [&](const fs::path&) {
    extracted = true;
    return Result<void>::Ok();
  });
  Require(!result.succeeded && !extracted, "Redirected delete path was accepted");
  f.BaselineIntact();
  const auto overlay = ApplyPatchTransaction(*ticket, kDeletions, [&](const fs::path& output) {
    Put(output / L"mods\\junction\\keep.txt", "attempted outside replacement");
    return Result<void>::Ok();
  });
  Require(!overlay.succeeded, "Redirected replacement path was accepted");
  f.BaselineIntact();
}

void PreserveOldWorkspaceAndCancelSafely() {
  Fixture f;
  auto ticket = f.Target();
  const auto existing = f.root / kWorkFolder / L"recovery.txt";
  Put(existing, "old recovery");
  bool extracted = false;
  auto result = ApplyPatchTransaction(*ticket, kDeletions, [&](const fs::path&) {
    extracted = true;
    return Result<void>::Ok();
  });
  Require(!result.succeeded && !extracted, "Existing workspace was adopted");
  Require(Get(existing) == "old recovery", "Existing workspace was altered");
  f.BaselineIntact();
  fs::remove(existing);
  fs::remove(existing.parent_path());
  std::atomic_bool cancel{false};
  PatchOptions options{&cancel, {}};
  result = ApplyPatchTransaction(*ticket, kDeletions, [&](const fs::path&) {
    cancel = true;
    return Result<void>::Ok();
  }, options);
  Require(!result.succeeded && result.cancelled, "Cancellation after removal was ignored");
  f.BaselineIntact();
}

void ReportRecoveryWithoutLayoutMarkers() {
  Fixture f;
  const auto workspace = f.root / kWorkFolder;
  Put(workspace / L"recovery.txt", "PENDING\n");
  fs::rename(f.root / L"Stock Game", workspace / L"saved game");
  auto ticket = f.Target();
  const auto result = ApplyPatch(*ticket);
  Require(!result.succeeded && result.recoveryFolder == workspace,
      "Interrupted patch was hidden by a missing layout marker");
  Require(Get(workspace / L"saved game\\SkyrimSE.exe") == "dummy game", "Original game marker was lost");
  Require(Get(workspace / L"recovery.txt") == "PENDING\n", "Recovery record changed");
}

void PreserveCommittedRecoveryOnCleanupFailure() {
  Fixture f;
  const auto settings = f.root / L"enblocal.ini";
  auto ticket = f.Target();
  Require(SetFileAttributesW(settings.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE,
      "Cannot create a read-only original");
  const auto outcome = ApplyPatchTransaction(*ticket, {}, [&](const fs::path& output) {
    Put(output / L"enblocal.ini", "new settings");
    return Result<void>::Ok();
  });
  const auto retained = f.root / kWorkFolder / L"originals\\1";
  if (fs::exists(retained)) SetFileAttributesW(retained.c_str(), FILE_ATTRIBUTE_NORMAL);
  if (fs::exists(settings)) SetFileAttributesW(settings.c_str(), FILE_ATTRIBUTE_NORMAL);
  Require(outcome.succeeded && outcome.recoveryFolder == f.root / kWorkFolder,
          "Cleanup error was mistaken for a failed merge: " + outcome.detail);
  Require(Get(settings) == "new settings", "Committed replacement was lost");
  Require(Get(outcome.recoveryFolder / L"recovery.txt").ends_with("COMMITTED\n"),
          "Commit record disappeared before the original could be cleaned up");
}

void PatchLogDoesNotFollowRedirectedPaths() {
  for (const bool junction : {true, false}) {
    Fixture f;
    struct RestoreLocalAppData {
      std::wstring value = std::wstring(32768, L'\0');
      RestoreLocalAppData() {
        const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), static_cast<DWORD>(value.size()));
        Require(length < value.size(), "LOCALAPPDATA is too long for the test");
        value.resize(length);
      }
      ~RestoreLocalAppData() { SetEnvironmentVariableW(L"LOCALAPPDATA", value.empty() ? nullptr : value.c_str()); }
    } restore;
    const auto cache = f.home / L"cache";
    Require(SetEnvironmentVariableW(L"LOCALAPPDATA", cache.c_str()) != FALSE, "Could not isolate fallback log");
    auto ticket = f.Target();
    const auto logs = f.package / L"logs";
    constexpr wchar_t name[] = L"modlist-patch-installer.log";
    if (junction) {
      f.Junction(logs);
    } else {
      fs::create_directory(logs);
      Require(CreateHardLinkW((logs / name).c_str(), (f.home / L"outside\\keep.txt").c_str(), nullptr) != FALSE,
              "Could not create fixture hard link");
    }
    auto log = PatchLogFile::Open(*ticket);
    Require(log.ok(), log.error());
    const auto path = log.value()->Path();
    const auto relative = path.lexically_relative(cache);
    Require(!relative.empty() && relative.begin()->native() == L"ModlistInstaller",
            "Unsafe package log was not replaced by a local log");
    Require(log.value()->Append("safe fallback log"), "Fallback log could not write");
    log.value().reset();
    Require(Get(path).ends_with(" safe fallback log\r\n"), "Fallback log did not persist its line");
    Require(!fs::exists(f.home / L"outside" / name), "Junction redirected a log write");
    f.BaselineIntact();
  }
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 3 || argc == 4) {
      Require(std::wstring_view(argv[1]) == L"--archive", "Usage: --archive <package> [failure|keep-unlisted]");
      const auto mode = argc == 4 ? std::wstring_view(argv[3]) : L"";
      Require(mode.empty() || mode == L"failure" || mode == L"keep-unlisted", "Unknown archive test mode");
      Fixture f;
      auto ticket = OpenPatchFolder(f.root / L"Stock Game", fs::path(argv[2]));
      Require(ticket.ok(), ticket.error());
      auto log = PatchLogFile::Open(*ticket.value());
      Require(log.ok(), log.error());
      bool saved = true;
      PatchOptions options;
      options.progress = [](int percent, const std::string& status) { std::cout << percent << "% " << status << '\n'; };
      options.log = [&](std::string_view line) {
        std::cout << "LOG " << line << '\n';
        if (!log.value()->Append(line)) saved = false;
      };
      options.diagnostics = [&](std::string_view line) { if (!log.value()->Append(line)) saved = false; };
      auto result = ApplyPatch(*ticket.value(), options);
      Require(saved, "A saved patch log write failed");
      if (mode == L"failure") {
        Require(!result.succeeded, "Bad package succeeded");
        f.BaselineIntact();
        Require(!fs::exists(f.root / kWorkFolder), "Rejected package left recovery unexpectedly");
        std::cout << "PASS bad package rejected: " << result.detail << '\n';
      } else {
        Require(result.succeeded, result.detail);
        Require(result.recoveryFolder.empty() && !fs::exists(f.root / kWorkFolder), "Real archive left workspace");
        Require(Get(f.root / L"enblocal.ini") == "packed settings", "Real archive did not replace root file");
        Require(Get(f.root / L"Stock Game\\Data\\patch.esp") == "packed plugin", "Real archive did not add plugin");
        Require(Get(f.root / L"mods\\Retired Mod\\new.txt") == "packed mod", "Real archive did not recreate mod");
        const bool keep = mode == L"keep-unlisted";
        Require(fs::exists(f.root / L"mods\\Retired Mod\\textures") == keep, "Unexpected removal state for obsolete tree");
        Require(fs::exists(f.root / L"Stock Game\\Data\\obsolete.ini") == keep, "Unexpected removal state for obsolete file");
        Require(Get(f.root / L"profiles\\V5.4 NO-LORE\\saves\\keep.ess") == "unrelated save", "Unlisted save changed");
        Require(Get(f.home / L"outside\\keep.txt") == "outside", "Outside file changed");
        std::cout << "PASS real archive installed and payload verified\n";
      }
      return 0;
    }
    Require(argc == 1, "Unexpected patch test arguments");
    const std::pair<const char*, void(*)()> cases[]{
      {"quoted and Unicode delete paths", ParseQuotedPaths},
      {"invalid delete paths", RejectUnsafePaths},
      {"new-layout root resolution", SelectTheModernRoot},
      {"delete first, then merge", DeleteBeforeExtractAndMerge},
      {"rollback after extraction failure", RestoreWhenExtractionFails},
      {"rollback after locked replacement", RestoreWhenReplacementIsLocked},
      {"junctions cannot redirect a patch", LeaveRedirectedFilesAlone},
      {"pending workspace and cancellation", PreserveOldWorkspaceAndCancelSafely},
      {"interrupted patch recovery without layout markers", ReportRecoveryWithoutLayoutMarkers},
      {"committed recovery retained on cleanup error", PreserveCommittedRecoveryOnCleanupFailure},
      {"saved patch log does not follow junctions or hard links", PatchLogDoesNotFollowRedirectedPaths},
    };
    for (const auto& [name, run] : cases) {
      run();
      std::cout << "PASS " << name << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Patch tests failed: " << error.what() << '\n';
    return 1;
  }
}
