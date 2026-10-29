#include "app/PackageDiscovery.h"
#include "app/RuntimePaths.h"
#include "ui/NativeStrings.h"
#include "common/EmbeddedResource.h"
#include "common/PathText.h"
#include "common/ProgressThrottle.h"
#include "extractor/PipelinedSevenZipExtractor.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Json.h"
#include "manifest/Manifest.h"
#include "paths/WindowsFileMove.h"
#include "paths/PathValidator.h"
#include "tracker/TrackerProvider.h"
#include "verifier/Sha256.h"
#include "verifier/Verifier.h"

#include <filesystem>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace modlist;

namespace {

void Expect(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

std::string ValidManifestJson(const std::string& hash) {
  return std::string(R"({
    "version": "1.0.0",
    "torrent": { "type": "magnet", "source": "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567" },
    "trackers": {
      "use_online_tracker_list": true,
      "tracker_list_url": "https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_all.txt",
      "continue_if_tracker_list_fails": true
    },
    "torrent_features": { "enable_dht": true, "enable_pex": true, "enable_lsd": true },
    "install": {
      "ask_user": true,
      "prefer_short_root_path": true,
      "example_path": "D:\\Sky",
      "max_recommended_path_length": 20,
      "same_disk_temp_only": true
    },
    "files": [
      { "path": "modpack.7z.001", "size": 3, "sha256": ")") + hash + R"(" }
    ],
    "extract": {
      "first_archive_part": "modpack.7z.001",
      "target_subfolder": "",
      "cleanup_after_success": false,
      "use_same_disk_temp": true
    }
  })";
}

std::string PackerManifestJson(const std::string& hash) {
  return std::string(R"({
    "schema": "modlist-manifest-chunks-v1",
    "archive_name": "MyPack",
    "unpacked_size": 7,
    "hash": {
      "algorithm": "sha256",
      "chunk_size": 67108864
    },
    "files": [
      {
        "path": "MyPack.7z.001",
        "size": 3,
        "sha256": ")") + hash + R"(",
        "chunks": [
          ")" + hash + R"("
        ]
      }
    ]
  })";
}

void TestSha256() {
  Expect(Sha256::HexDigest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
         "SHA256 digest mismatch");
}

void TestProgressThrottle() {
  ProgressThrottle throttle(std::chrono::milliseconds(100));
  const auto start = std::chrono::steady_clock::time_point{};
  Expect(throttle.ShouldReport(false, start), "First progress update must be delivered");
  for (int i = 0; i < 1000; ++i) {
    Expect(!throttle.ShouldReport(false, start + std::chrono::milliseconds(99)),
           "Progress callbacks within 100 ms must be coalesced");
  }
  Expect(throttle.ShouldReport(false, start + std::chrono::milliseconds(100)),
         "Progress should refresh at ten updates per second");
  Expect(throttle.ShouldReport(true, start + std::chrono::milliseconds(101)),
         "Final progress updates must bypass the time limit");
  Expect(PathTextEqual(L"D:\\\u0421\u0411\u041e\u0420\u041a\u0410", L"d:\\\u0441\u0431\u043e\u0440\u043a\u0430"),
         "Windows path comparison must handle Cyrillic case without locale dependence");
}

void TestManifestLoader() {
  ManifestLoader loader;
  auto manifest = loader.LoadFromString(ValidManifestJson(Sha256::HexDigest("abc")));
  Expect(manifest.ok(), manifest.error().c_str());
  Expect(manifest.value().files.size() == 1, "Manifest file count mismatch");
  Expect(manifest.value().extract.firstArchivePart == "modpack.7z.001", "Manifest extract archive mismatch");
  Expect(manifest.value().archiveName == "modpack", "Legacy manifest archive name should be inferred");
}

void TestPackerManifestLoader() {
  ManifestLoader loader;
  auto manifest = loader.LoadFromString(PackerManifestJson(Sha256::HexDigest("abc")));
  Expect(manifest.ok(), manifest.error().c_str());
  Expect(manifest.value().version == "modlist-manifest-chunks-v1", "Packer manifest schema mismatch");
  Expect(manifest.value().files.size() == 1, "Packer manifest file count mismatch");
  Expect(manifest.value().files[0].path == "MyPack.7z.001", "Packer manifest file path mismatch");
  Expect(manifest.value().extract.firstArchivePart == "MyPack.7z.001", "Packer manifest archive mismatch");
  Expect(manifest.value().archiveName == "MyPack", "Packer manifest archive name mismatch");
  Expect(manifest.value().unpackedSize == 7, "Packer manifest unpacked size mismatch");
  Expect(manifest.value().hashChunkSize == 67108864, "Packer manifest chunk size mismatch");
  Expect(manifest.value().files[0].chunkSha256.size() == 1,
         "Packer manifest chunk hash should be retained");
  Expect(manifest.value().files[0].chunkSha256[0] == Sha256::HexDigest("abc"),
         "Packer manifest chunk hash mismatch");
}

void TestUtf8ManifestArchivePaths() {
  std::string json = PackerManifestJson(Sha256::HexDigest("abc"));
  const std::string escapedName = R"(\u0421\u0431\u043e\u0440\u043a\u0430_\uD83D\uDE00)";
  size_t position = 0;
  while ((position = json.find("MyPack", position)) != std::string::npos) {
    json.replace(position, 6, escapedName);
    position += escapedName.size();
  }
  const auto expected = std::filesystem::path(L"\u0421\u0431\u043e\u0440\u043a\u0430_\U0001F600.7z.001");
  auto manifest = ManifestLoader().LoadFromString(json);
  Expect(manifest.ok(), manifest.error().c_str());
  Expect(manifest.value().files.front().path == expected &&
             manifest.value().extract.firstArchivePart == expected,
         "Manifest archive paths must decode UTF-8 independently of the Windows code page");
  const auto root = std::filesystem::temp_directory_path() / "modlist_utf8_archive_test";
  std::filesystem::create_directories(root);
  { std::ofstream out(root / expected, std::ios::binary); out << "abc"; }
  Expect(Verifier().Verify(root, manifest.value().files).ok,
         "The UTF-8 manifest should verify the actual Unicode archive filename");
  auto package = DiscoverPackageNear(root);
  Expect(package.ok() && package.value().firstArchivePart == root / expected,
         "Archive discovery must support filenames outside the active ANSI code page");
  { std::ofstream file(root / "one.torrent"); file << "torrent"; }
  { std::ofstream file(root / "two.torrent"); file << "torrent"; }
  Expect(DiscoverPackageNear(root, false).ok(),
         "The local installer must ignore unrelated torrent files");
  std::filesystem::remove_all(root);
}

void TestExecutableCacheRepairsSameSizeTampering() {
  SevenZipExtractor extractor;
  const auto root = std::filesystem::temp_directory_path() / "modlist_cache_tampering_test";
  const auto original = extractor.LocateExecutable(root);
  Expect(original.ok(), original.error().c_str());
  const auto originalHash = Sha256::FileHexDigest(original.value());
  {
    std::fstream file(original.value(), std::ios::binary | std::ios::in | std::ios::out);
    file.put('X');
  }
  const auto repaired = extractor.LocateExecutable(root);
  Expect(repaired.ok() && Sha256::FileHexDigest(repaired.value()).value() == originalHash.value(),
         "A same-size altered cached 7z.exe must be replaced with embedded bytes");
}

void TestPackerManifestRejectsInvalidChunks() {
  ManifestLoader loader;
  std::string missingChunk = PackerManifestJson(Sha256::HexDigest("abc"));
  const std::string chunkLine = "          \"" + Sha256::HexDigest("abc") + "\"\n";
  const auto chunk = missingChunk.find(chunkLine);
  missingChunk.erase(chunk, chunkLine.size());
  Expect(!loader.LoadFromString(missingChunk).ok(),
         "Packer manifest with a wrong chunk count should fail");

  std::string invalidHash = PackerManifestJson(Sha256::HexDigest("abc"));
  const auto chunks = invalidHash.find("\"chunks\"");
  const auto hash = invalidHash.find(Sha256::HexDigest("abc"), chunks);
  invalidHash.replace(hash, 64, std::string(64, 'z'));
  Expect(!loader.LoadFromString(invalidHash).ok(),
         "Packer manifest with an invalid chunk SHA256 should fail");
}

void TestManifestRejectsUnsafeArchiveName() {
  ManifestLoader loader;
  std::string json = PackerManifestJson(Sha256::HexDigest("abc"));
  const auto pos = json.find("\"MyPack\"");
  json.replace(pos, std::string("\"MyPack\"").size(), "\"..\\\\evil\"");
  auto manifest = loader.LoadFromString(json);
  Expect(!manifest.ok(), "Unsafe archive folder name should fail");
}

void TestPackerManifestInfersArchiveName() {
  ManifestLoader loader;
  std::string json = PackerManifestJson(Sha256::HexDigest("abc"));
  const auto field = json.find("    \"archive_name\": \"MyPack\",\n");
  json.erase(field, std::string("    \"archive_name\": \"MyPack\",\n").size());
  auto manifest = loader.LoadFromString(json);
  Expect(manifest.ok(), manifest.error().c_str());
  Expect(manifest.value().archiveName == "MyPack",
         "Older packer manifest archive name should be inferred");
}

void TestArchiveInstallFolder() {
  auto folder = ResolveArchiveInstallFolder("D:\\Games", "Sky");
  Expect(folder.ok(), folder.error().c_str());
  Expect(folder.value().lexically_normal() ==
             std::filesystem::path("D:\\Games\\Sky").lexically_normal(),
         "Archive should install into a named folder below the selected root");
}

void TestAutomaticUnpackFolder() {
  const auto folder = AutomaticUnpackFolder(
      std::filesystem::path(L"D:\\Folder1\\folder2\\folder3"));
  Expect(folder.lexically_normal() ==
             std::filesystem::path(L"D:\\Unpacking").lexically_normal(),
         "Automatic unpack folder should be at the selected install drive root");
  Expect(AutomaticUnpackFolder(std::filesystem::path(L"relative\\folder")).empty(),
         "Relative install paths must not produce an automatic unpack folder");
  Expect(AutomaticUnpackFolder(std::filesystem::path(L"D:relative\\folder")).empty(),
         "Drive-relative install paths must not produce an automatic unpack folder");
  Expect(AutomaticUnpackFolder(std::filesystem::path(L"\\Games")).empty(),
         "Root-relative install paths must not produce an automatic unpack folder");
}

void TestArchiveExtractionPlan() {
  const std::filesystem::path finalFolder = L"D:\\Games\\Sky";
  const std::filesystem::path staging = L"D:\\Unpacking";
  const size_t prefix = finalFolder.wstring().size() + 1;
  std::vector<std::filesystem::path> entries{
      std::filesystem::path(std::wstring(240 - prefix, L'a'))};
  auto plan = PlanArchiveExtraction(finalFolder, staging, entries);
  Expect(plan.ok() && plan.value().direct && plan.value().folder == finalFolder,
         "A full extracted path of exactly 240 characters should extract directly");
  Expect(plan.value().longestFinalPath == 240, "Plan should include the final folder and separator");
  Expect(!PlanArchiveExtraction(L"\\Games\\Sky", staging, entries).ok(),
         "Root-relative destinations must not underestimate the full path length");
  entries.emplace_back(std::wstring(241 - prefix, L'b'));
  plan = PlanArchiveExtraction(finalFolder, staging, entries);
  Expect(plan.ok() && !plan.value().direct && plan.value().folder == staging,
         "Any entry over 240 characters should use staging");
  entries = {std::filesystem::path(std::wstring(240 - prefix, L'\u0416'))};
  plan = PlanArchiveExtraction(finalFolder, staging, entries);
  Expect(plan.ok() && plan.value().direct,
         "Unicode path length should count UTF-16 units rather than UTF-8 bytes");
  entries = {"../escape.bin"};
  Expect(!PlanArchiveExtraction(finalFolder, staging, entries).ok(),
         "Unsafe archive paths must fail before extraction");
  entries.clear();
  plan = PlanArchiveExtraction(finalFolder, staging, entries);
  Expect(plan.ok() && !plan.value().direct, "An empty listing should use staging");
}

void TestManifestRejectsTraversal() {
  ManifestLoader loader;
  std::string json = ValidManifestJson(Sha256::HexDigest("abc"));
  const auto pos = json.find("modpack.7z.001");
  json.replace(pos, std::string("modpack.7z.001").size(), "../evil.7z.001");
  auto manifest = loader.LoadFromString(json);
  Expect(!manifest.ok(), "Manifest traversal path should fail");
}

void TestJsonUnicodeEscapes() {
  auto parsed = ParseJson(R"({"name":"Sky \u0414\u0440\u0430\u0433\u043e\u043d \ud834\udd1e"})");
  Expect(parsed.ok(), parsed.error().c_str());
  const auto* name = parsed.value().Find("name");
  Expect(name != nullptr && name->IsString(), "JSON unicode string missing");
  const std::string expected = std::string("Sky ") +
      "\xD0\x94\xD1\x80\xD0\xB0\xD0\xB3\xD0\xBE\xD0\xBD " +
      "\xF0\x9D\x84\x9E";
  Expect(name->AsString() == expected, "JSON unicode escapes should decode to UTF-8");
}

void TestTrackerParsing() {
  const auto trackers = TrackerProvider::ParseTrackers(" udp://tracker.example:80/announce \n\nbad://x\nhttps://tracker.example/a\nudp://tracker.example:80/announce\nwss://tracker.example/ws\n");
  Expect(trackers.size() == 3, "Tracker parsing should trim, validate, and deduplicate");
}

void TestVerifier() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_installer_tests";
  std::filesystem::create_directories(root);
  {
    std::ofstream out(root / "modpack.7z.001", std::ios::binary);
    out << "abc";
  }

  ManifestFile file;
  file.path = "modpack.7z.001";
  file.size = 3;
  file.sha256 = Sha256::HexDigest("abc");

  Verifier verifier;
  auto summary = verifier.Verify(root, {file});
  Expect(summary.ok, "Verifier should accept matching file");
  Expect(summary.files.size() == 1 && summary.files[0].hashMatches, "Verifier hash should match");
}

void TestExtractorCommand() {
  ExtractionConfig config;
  config.sevenZipExe = "C:/Tools/7z.exe";
  config.archiveFirstPart = "D:/Downloads/modpack.7z.001";
  config.installFolder = "D:/Sky";
  config.useSameDiskTemp = true;
  const auto command = SevenZipExtractor::BuildCommand(config);
  Expect(command.find(" -o\"D:/Sky\"") != std::string::npos, "7-Zip command should target install folder directly");
  Expect(command.find(" -bsp1") != std::string::npos, "7-Zip command should emit progress to stdout");
  Expect(command.find(" -w\"") != std::string::npos && command.find(".install_temp\"") != std::string::npos,
         "7-Zip command should use same-disk temp folder");
}

void TestUnicodeManifestPathSafety() {
#ifdef _WIN32
  Expect(IsSafeManifestRelativePath(
             std::filesystem::path(L"mods\\Поддержка сборки\\файл.txt")),
         "Unicode Windows archive paths should be safe without narrow conversion");
#endif
}

#ifdef _WIN32
std::wstring QuoteCommandArgument(const std::filesystem::path& path) {
  return L"\"" + path.wstring() + L"\"";
}

bool RunProcess(const std::filesystem::path& executable,
                const std::wstring& arguments,
                const std::filesystem::path& workingDirectory) {
  std::wstring command = QuoteCommandArgument(executable) + L" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                      nullptr, workingDirectory.c_str(), &startup, &process)) {
    return false;
  }
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return exitCode == 0;
}

std::string DigestToHex(const std::array<uint8_t, 32>& digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    out << std::setw(2) << static_cast<unsigned int>(byte);
  }
  return out.str();
}

std::vector<std::string> ChunkHashes(const std::filesystem::path& path, uint64_t chunkSize) {
  std::ifstream input(path, std::ios::binary);
  std::vector<std::string> hashes;
  std::vector<uint8_t> buffer(256 * 1024);
  while (input) {
    Sha256 hash;
    uint64_t remaining = chunkSize;
    uint64_t chunkBytes = 0;
    while (remaining > 0 && input) {
      const auto request = static_cast<std::streamsize>(
          std::min<uint64_t>(remaining, buffer.size()));
      input.read(reinterpret_cast<char*>(buffer.data()), request);
      const auto read = input.gcount();
      if (read <= 0) {
        break;
      }
      hash.Update(buffer.data(), static_cast<size_t>(read));
      remaining -= static_cast<uint64_t>(read);
      chunkBytes += static_cast<uint64_t>(read);
    }
    if (chunkBytes > 0) {
      hashes.push_back(DigestToHex(hash.Final()));
    }
  }
  return hashes;
}

void TestPipelineDoesNotForcePerFileFlush() {
  const std::vector<std::filesystem::path> candidates = {
      std::filesystem::path(__FILE__).parent_path().parent_path() / "src" /
          "extractor" / "PipelinedSevenZipExtractor.cpp",
      std::filesystem::current_path() / "src" / "extractor" /
          "PipelinedSevenZipExtractor.cpp",
      std::filesystem::current_path() / ".." / "src" / "extractor" /
          "PipelinedSevenZipExtractor.cpp",
      std::filesystem::current_path() / ".." / ".." / "src" / "extractor" /
          "PipelinedSevenZipExtractor.cpp",
  };
  std::string source;
  for (const auto& candidate : candidates) {
    std::ifstream input(candidate, std::ios::binary);
    if (input) {
      source.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
      break;
    }
  }
  Expect(!source.empty(), "Unable to locate the pipelined extractor source");
  Expect(source.find("FlushFileBuffers(handle_)") == std::string::npos,
         "Pipeline must not force a durable disk flush after every extracted file");
}

void TestWindowsMoveRetriesTransientLock() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_move_retry";
  std::filesystem::remove_all(root);
  const auto source = root / "source";
  const auto target = root / "target";
  std::filesystem::create_directories(source);
  const auto lockedFile = source / "locked.txt";
  {
    std::ofstream output(lockedFile, std::ios::binary | std::ios::trunc);
    output << "locked during first move attempt";
  }

  HANDLE lock = CreateFileW(
      lockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  Expect(lock != INVALID_HANDLE_VALUE, "Unable to create a transient move lock");

  bool retryObserved = false;
  const auto result = MovePathWithRetry(
      source, target, false, 3, std::chrono::milliseconds(1),
      [&](uint32_t attempt, uint32_t maxAttempts, unsigned long error) {
        retryObserved = true;
        Expect(attempt == 1, "Unexpected move retry attempt number");
        Expect(maxAttempts == 3, "Unexpected move retry limit");
        Expect(IsTransientMoveError(error), "Move retry reported a non-transient error");
        CloseHandle(lock);
        lock = INVALID_HANDLE_VALUE;
      });

  if (lock != INVALID_HANDLE_VALUE) {
    CloseHandle(lock);
  }
  Expect(retryObserved, "Locked directory move did not request a retry");
  Expect(result.ok, "Directory move did not recover after the lock was released");
  Expect(result.attempts == 2, "Directory move used an unexpected number of attempts");
  Expect(std::filesystem::exists(target / "locked.txt"),
         "Retried directory move did not preserve its contents");
  Expect(!std::filesystem::exists(source),
         "Retried directory move left the source folder behind");
  std::filesystem::remove_all(root);
}

void TestWindowsMoveFallsBackToIndividualLockedEntry() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_move_fallback";
  std::filesystem::remove_all(root);
  const auto source = root / "source";
  const auto nested = source / "mods" / "example";
  const auto target = root / "target";
  std::filesystem::create_directories(nested);
  const auto lockedFile = nested / "locked.txt";
  {
    std::ofstream output(lockedFile, std::ios::binary | std::ios::trunc);
    output << "locked while directory fallback begins";
  }

  HANDLE lock = CreateFileW(
      lockedFile.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL, nullptr);
  Expect(lock != INVALID_HANDLE_VALUE, "Unable to create a directory fallback lock");

  bool fallbackObserved = false;
  bool fileRetryObserved = false;
  MoveTreeOptions options;
  options.maxAttempts = 3;
  options.retryDelay = std::chrono::milliseconds(1);
  options.directoryFallbackCallback =
      [&](const std::filesystem::path&, const std::filesystem::path&, unsigned long error) {
        fallbackObserved = true;
        Expect(IsTransientMoveError(error),
               "Directory fallback reported a non-transient error");
      };
  options.retryCallback =
      [&](const std::filesystem::path& retrySource, const std::filesystem::path&,
          uint32_t attempt, uint32_t maxAttempts, unsigned long error) {
        if (retrySource == lockedFile) {
          fileRetryObserved = true;
          Expect(attempt == 1, "Unexpected locked-file retry attempt number");
          Expect(maxAttempts == 3, "Unexpected locked-file retry limit");
          Expect(IsTransientMoveError(error),
                 "Locked-file retry reported a non-transient error");
          CloseHandle(lock);
          lock = INVALID_HANDLE_VALUE;
        }
      };

  const auto result = MovePathTreeWithRetry(source, target, options);
  if (lock != INVALID_HANDLE_VALUE) {
    CloseHandle(lock);
  }
  Expect(fallbackObserved, "Locked descendant did not trigger directory fallback");
  Expect(fileRetryObserved, "Directory fallback did not isolate the locked file");
  Expect(result.ok, "Directory fallback did not recover after the file lock was released");
  Expect(result.usedDirectoryFallback,
         "Directory fallback result did not report its recovery path");
  Expect(std::filesystem::exists(target / "mods" / "example" / "locked.txt"),
         "Directory fallback did not preserve the locked file");
  Expect(!std::filesystem::exists(source),
         "Directory fallback left the source directory behind");
  std::filesystem::remove_all(root);
}

void TestPathValidatorRejectsFolderWithoutDeleteAccess() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_delete_probe";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  const auto probeFolder =
      root / (".modlist_access_probe_" + std::to_string(GetCurrentProcessId()));
  std::filesystem::create_directories(probeFolder);
  const auto probe = probeFolder / "probe.tmp";
  {
    std::ofstream output(probe, std::ios::binary | std::ios::trunc);
    output << "pre-existing probe";
  }

  HANDLE lock = CreateFileW(
      probe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Expect(lock != INVALID_HANDLE_VALUE, "Unable to lock the path-validation probe");

  PathValidator validator;
  const auto result = validator.ValidateInstallFolder(root);
  CloseHandle(lock);
  Expect(!result.ok,
         "Path validator accepted a folder where its probe could not be deleted");
  std::filesystem::remove_all(root);
}

void TestPathValidatorAcceptsFolderWithDeleteAccess() {
  const auto root =
      std::filesystem::temp_directory_path() / "modlist_delete_probe_allowed";
  std::filesystem::remove_all(root);

  PathValidator validator;
  const auto result = validator.ValidateInstallFolder(root);
  Expect(result.ok,
         "Path validator rejected a folder with create, rename, and delete access");
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    const auto name = entry.path().filename().wstring();
    Expect(!name.starts_with(L".modlist_access_probe"),
           "Path validator left an access-probe artifact behind");
  }
  std::filesystem::remove_all(root);
}

void TestPathValidatorAcceptsLongWritableFolder() {
  const auto base = std::filesystem::temp_directory_path() /
      ("modlist_long_probe_" + std::to_string(GetCurrentProcessId()));
  auto folder = base;
  while (folder.native().size() < 219) {
    const auto remaining = 219 - folder.native().size();
    if (remaining == 1) folder += L"x";
    else folder /= std::wstring(std::min<size_t>(48, remaining - 1), L'x');
  }
  std::filesystem::create_directories(folder);
  const auto result = PathValidator().ValidateInstallFolder(folder);
  const bool empty = std::filesystem::is_empty(folder);
  std::filesystem::remove_all(base);
  Expect(result.ok,
         "A writable 219-character destination must not fail because its access probe exceeds MAX_PATH");
  Expect(empty, "Long-path validation left an access probe behind");
}

void TestWindowsMoveAcceptsLockedEmptySourceAfterFilesMove() {
  const auto root =
      std::filesystem::temp_directory_path() / "modlist_empty_source_lock";
  std::filesystem::remove_all(root);
  const auto source = root / "source";
  const auto target = root / "target";
  std::filesystem::create_directories(source);
  {
    std::ofstream output(source / "payload.txt", std::ios::binary | std::ios::trunc);
    output << "payload";
  }

  HANDLE directoryLock = CreateFileW(
      source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  Expect(directoryLock != INVALID_HANDLE_VALUE,
         "Unable to lock the source directory during fallback");

  bool cleanupWarningObserved = false;
  MoveTreeOptions options;
  options.maxAttempts = 3;
  options.retryDelay = std::chrono::milliseconds(1);
  options.sourceCleanupFailureCallback =
      [&](const std::filesystem::path& cleanupPath, unsigned long error) {
        cleanupWarningObserved = true;
        Expect(cleanupPath == source, "Cleanup warning reported the wrong directory");
        Expect(IsTransientMoveError(error),
               "Cleanup warning reported a non-transient directory error");
      };

  const auto result = MovePathTreeWithRetry(source, target, options);
  CloseHandle(directoryLock);
  Expect(result.ok, "Moved files were treated as failed because an empty source was locked");
  Expect(cleanupWarningObserved, "Locked empty source directory was not reported");
  Expect(std::filesystem::exists(target / "payload.txt"),
         "Fallback omitted the payload before source cleanup");
  Expect(std::filesystem::is_empty(source),
         "Locked source directory retained files after fallback");
  std::filesystem::remove_all(root);
}

void TestWindowsMoveAcceptsLockedEmptyNestedSourceAfterFilesMove() {
  const auto root =
      std::filesystem::temp_directory_path() / "modlist_empty_nested_source_lock";
  std::filesystem::remove_all(root);
  const auto source = root / "source";
  const auto nestedSource = source / "mods" / "example";
  const auto target = root / "target";
  std::filesystem::create_directories(nestedSource);
  std::filesystem::create_directories(target);
  {
    std::ofstream output(nestedSource / "payload.txt",
                         std::ios::binary | std::ios::trunc);
    output << "payload";
  }

  HANDLE directoryLock = CreateFileW(
      nestedSource.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  Expect(directoryLock != INVALID_HANDLE_VALUE,
         "Unable to lock the nested source directory during fallback");

  bool cleanupWarningObserved = false;
  MoveTreeOptions options;
  options.maxAttempts = 3;
  options.retryDelay = std::chrono::milliseconds(1);
  options.sourceCleanupFailureCallback =
      [&](const std::filesystem::path&, unsigned long) {
        cleanupWarningObserved = true;
      };

  const auto result = MovePathTreeWithRetry(source, target, options);
  CloseHandle(directoryLock);
  Expect(result.ok,
         "An empty locked nested directory made a completed move fail");
  Expect(cleanupWarningObserved,
         "Locked empty nested source directory was not reported");
  Expect(std::filesystem::exists(target / "mods" / "example" / "payload.txt"),
         "Nested fallback omitted the payload");
  Expect(std::filesystem::exists(source / "mods" / "example"),
         "Test did not preserve the locked empty nested directory");
  Expect(std::filesystem::is_empty(source / "mods" / "example"),
         "Locked nested source retained payload files");
  std::filesystem::remove_all(root);
}

void TestInstallMoveFailureProvidesManualRecoveryPaths() {
  const std::vector<std::filesystem::path> sourceCandidates = {
      std::filesystem::path(__FILE__).parent_path().parent_path() / "src" / "ui" /
          "WinInstallerApp.cpp",
      std::filesystem::current_path() / "src" / "ui" / "WinInstallerApp.cpp",
  };
  std::string source;
  for (const auto& candidate : sourceCandidates) {
    std::ifstream input(candidate, std::ios::binary);
    if (input) {
      source.assign(std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>());
      break;
    }
  }
  Expect(!source.empty(), "Unable to locate the native installer source");
  Expect(source.find("PostManualInstallRequired(hwnd, unpackFolder, installFolder)") !=
             std::string::npos,
         "Move failure must post manual recovery paths to the UI");

  const std::vector<std::filesystem::path> stringCandidates = {
      std::filesystem::path(__FILE__).parent_path().parent_path() / "ui" / "strings.json",
      std::filesystem::current_path() / "ui" / "strings.json",
  };
  std::string strings;
  for (const auto& candidate : stringCandidates) {
    std::ifstream input(candidate, std::ios::binary);
    if (input) {
      strings.assign(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
      break;
    }
  }
  Expect(!strings.empty(), "Unable to locate the native UI text catalogue");
  Expect(strings.find("install_files_failed_message") != std::string::npos,
         "UI text catalogue is missing the manual recovery message");
  Expect(strings.find("{source}") != std::string::npos,
         "Manual recovery message is missing the unpack-folder placeholder");
  Expect(strings.find("{target}") != std::string::npos,
         "Manual recovery message is missing the destination-folder placeholder");
}

void TestInstallerHidesManualUnpackControls() {
  const auto sourceRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
  const auto loadSource = [&](const std::filesystem::path& relative) {
    const std::vector<std::filesystem::path> candidates = {
        sourceRoot / relative,
        std::filesystem::current_path() / relative,
    };
    for (const auto& candidate : candidates) {
      std::ifstream input(candidate, std::ios::binary);
      if (input) {
        return std::string(std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>());
      }
    }
    return std::string{};
  };

  const auto appSource = loadSource("src/ui/WinInstallerApp.cpp");
  const auto viewSource = loadSource("src/ui/NativeInstallerView.cpp");
  Expect(!appSource.empty() && !viewSource.empty(),
         "Unable to locate native installer UI sources");
  Expect(appSource.find("AutomaticUnpackFolder(") != std::string::npos,
         "Installer does not derive staging from the selected install root");
  Expect(appSource.find("ShowControl(hwnd, kDrivePickerButton, true)") ==
             std::string::npos,
         "Manual unpack-drive picker is still shown");
  Expect(viewSource.find("state.unpackDriveLabel") == std::string::npos,
         "Native view still paints the unpack-drive label");
  Expect(viewSource.find("state.unpackTarget") == std::string::npos,
         "Native view still paints the manual unpack target");
}

void TestPipelinedExtractor() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_pipeline_integration";
  std::filesystem::remove_all(root);
  const auto source = root / "source";
  const auto archives = root / "archives";
  const auto output = root / "output";
  std::filesystem::create_directories(source);
  std::filesystem::create_directories(archives);

  const auto payload = source / "payload.bin";
  {
    std::ofstream file(payload, std::ios::binary | std::ios::trunc);
    uint32_t state = 0x12345678u;
    std::vector<uint8_t> bytes(1024 * 1024);
    for (size_t block = 0; block < 8; ++block) {
      for (auto& byte : bytes) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        byte = static_cast<uint8_t>(state);
      }
      file.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    }
  }

  const auto smallFiles = source / "small-files";
  std::filesystem::create_directories(smallFiles);
  constexpr size_t smallFileCount = 512;
  for (size_t index = 0; index < smallFileCount; ++index) {
    std::ofstream file(smallFiles / ("file-" + std::to_string(index) + ".txt"),
                       std::ios::binary | std::ios::trunc);
    file << "small-file-" << index;
  }

  const auto sevenZip = std::filesystem::current_path() / "resources" / "7z.exe";
  const auto archive = archives / std::filesystem::path(L"\u0421\u0431\u043e\u0440\u043a\u0430_\U0001F600.7z");
  const std::wstring arguments = L"a " + QuoteCommandArgument(archive) + L" " +
      QuoteCommandArgument(payload.filename()) + L" " +
      QuoteCommandArgument(smallFiles.filename()) + L" -mx=1 -v1m";
  Expect(RunProcess(sevenZip, arguments, source), "Unable to create split integration archive");

  std::vector<std::filesystem::path> parts;
  for (const auto& entry : std::filesystem::directory_iterator(archives)) {
    if (entry.is_regular_file()) {
      parts.push_back(entry.path());
    }
  }
  std::sort(parts.begin(), parts.end());
  Expect(parts.size() > 1, "Integration archive should contain multiple volumes");

  Manifest manifest;
  manifest.version = "modlist-manifest-chunks-v1";
  manifest.archiveName = "pipeline";
  manifest.hashChunkSize = 256 * 1024;
  manifest.unpackedSize = std::filesystem::file_size(payload);
  for (const auto& part : parts) {
    ManifestFile file;
    file.path = part.filename();
    file.size = std::filesystem::file_size(part);
    const auto fullHash = Sha256::FileHexDigest(part);
    Expect(fullHash.ok(), "Unable to hash integration archive volume");
    file.sha256 = fullHash.value();
    file.chunkSha256 = ChunkHashes(part, manifest.hashChunkSize);
    manifest.files.push_back(std::move(file));
  }
  manifest.extract.firstArchivePart = manifest.files.front().path;

  PipelinedSevenZipExtractor extractor;
  const auto sdkCache = root / "sdk-cache";
  const auto library = extractor.LocateLibrary(sdkCache);
  Expect(library.ok(), library.error().c_str());
  const auto libraryHash = Sha256::FileHexDigest(library.value());
  { std::fstream file(library.value(), std::ios::binary | std::ios::in | std::ios::out); file.put('X'); }
  const auto repairedLibrary = extractor.LocateLibrary(sdkCache);
  Expect(repairedLibrary.ok() && Sha256::FileHexDigest(repairedLibrary.value()).value() == libraryHash.value(),
         "A same-size altered cached 7z.dll must be replaced before loading");
  Expect(library.value() == UserRuntimeFolder(sdkCache) / "tools" / "7zip" / "7z.dll",
         "Pipeline test should use the embedded 7-Zip SDK library");
  Expect(std::filesystem::is_regular_file(
             UserRuntimeFolder(sdkCache) / "tools" / "7zip" / "License.txt"),
         "Pipeline should extract the embedded 7-Zip license");
  PipelinedExtractionConfig config;
  config.sevenZipLibrary = library.value();
  config.archiveFolder = archives;
  config.installFolder = output;
  config.manifest = &manifest;
  const auto unusedStaging = root / "unused-staging";
  std::filesystem::create_directory(unusedStaging);
  {
    std::ofstream sentinel(unusedStaging / "keep.txt");
    sentinel << "unrelated staging content";
  }
  const auto directPlan = extractor.PlanExtraction(config, output, unusedStaging);
  Expect(directPlan.ok(), directPlan.error().c_str());
  Expect(directPlan.value().direct && directPlan.value().folder == output,
         "Real split archive should select direct extraction for a short destination");
  Expect(!std::filesystem::exists(output), "Inspecting archive paths must not create the destination");
  auto missingConfig = config;
  missingConfig.archiveFolder = root / "missing-archives";
  Expect(!extractor.PlanExtraction(missingConfig, output, root / "Unpacking").ok(),
         "Unavailable archive metadata must not produce a direct extraction plan");
  const auto longDestination = root / std::wstring(210, L'x');
  const auto stagedPlan = extractor.PlanExtraction(config, longDestination, root / "Unpacking");
  Expect(stagedPlan.ok() && !stagedPlan.value().direct &&
             stagedPlan.value().folder == root / "Unpacking",
         "Real archive paths should select staging for a long destination");
  auto legacyManifest = manifest;
  legacyManifest.hashChunkSize = 0;
  for (auto& file : legacyManifest.files) {
    file.chunkSha256.clear();
  }
  auto legacyConfig = config;
  legacyConfig.manifest = &legacyManifest;
  const auto legacyPlan = extractor.PlanExtraction(legacyConfig, output, root / "Unpacking");
  Expect(legacyPlan.ok() && legacyPlan.value().direct,
         "Path inspection should also support legacy split manifests without chunk hashes");
  std::atomic_bool cancelled{true};
  auto cancelledConfig = config;
  cancelledConfig.cancelRequested = &cancelled;
  Expect(!extractor.PlanExtraction(cancelledConfig, output, root / "Unpacking").ok(),
         "Cancelling path inspection must stop installation planning");
  auto rejectedConfig = config;
  rejectedConfig.installFolder = longDestination;
  rejectedConfig.maxOutputPathLength = 240;
  const auto rejectedResult = extractor.Extract(rejectedConfig);
  Expect(!rejectedResult.ok && !std::filesystem::exists(longDestination),
         "Direct extraction must recheck all paths before creating the output folder");
  config.maxOutputPathLength = 240;
  PipelineExtractionProgress lastProgress;
  std::vector<bool> reportedArchives(parts.size(), false);
  std::vector<bool> extractedArchives(parts.size(), false);
  const auto result = extractor.Extract(config, [&](const auto& progress) {
    lastProgress = progress;
    if (progress.validationArchiveIndex > 0 &&
        progress.validationArchiveIndex <= reportedArchives.size()) {
      reportedArchives[progress.validationArchiveIndex - 1] = true;
    }
    if (progress.extractionArchiveIndex > 0 &&
        progress.extractionArchiveIndex <= extractedArchives.size()) {
      extractedArchives[progress.extractionArchiveIndex - 1] = true;
    }
  });
  Expect(result.ok, result.message.c_str());
  {
    std::ifstream sentinel(unusedStaging / "keep.txt");
    std::string contents((std::istreambuf_iterator<char>(sentinel)), std::istreambuf_iterator<char>());
    Expect(contents == "unrelated staging content" &&
               std::distance(std::filesystem::directory_iterator(unusedStaging),
                             std::filesystem::directory_iterator{}) == 1,
           "Direct extraction must preserve a populated unused staging folder");
  }
  Expect(lastProgress.validatedBytes == lastProgress.validationTotalBytes,
         "Pipeline should validate every archive byte");
  Expect(std::all_of(reportedArchives.begin(), reportedArchives.end(), [](bool reported) {
           return reported;
         }),
         "Pipeline progress should identify every validated archive volume");
  Expect(std::all_of(extractedArchives.begin(), extractedArchives.end(), [](bool reported) {
           return reported;
         }),
         "Pipeline progress should identify every archive volume read by 7-Zip");
  const auto sourceHash = Sha256::FileHexDigest(payload);
  const auto outputHash = Sha256::FileHexDigest(output / payload.filename());
  Expect(sourceHash.ok() && outputHash.ok() && sourceHash.value() == outputHash.value(),
         "Pipelined extraction payload mismatch");
  for (size_t index = 0; index < smallFileCount; ++index) {
    const auto path = output / smallFiles.filename() /
        ("file-" + std::to_string(index) + ".txt");
    std::ifstream file(path, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    Expect(file.good() || file.eof(), "Pipelined extraction omitted a small file");
    Expect(contents.str() == "small-file-" + std::to_string(index),
           "Pipelined extraction small-file payload mismatch");
  }

  auto stagedConfig = config;
  stagedConfig.installFolder = stagedPlan.value().folder;
  stagedConfig.maxOutputPathLength = 0;
  const auto stagedResult = extractor.Extract(stagedConfig);
  Expect(stagedResult.ok, stagedResult.message.c_str());
  const auto stagedHash = Sha256::FileHexDigest(stagedConfig.installFolder / payload.filename());
  Expect(stagedHash.ok() && stagedHash.value() == sourceHash.value(),
         "Staging fallback should extract the same payload as direct extraction");
  Expect(!std::filesystem::exists(longDestination), "Staged extraction must leave the final folder untouched");

  const auto corruptedPart = parts[1];
  {
    std::fstream file(corruptedPart, std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(137);
    char value = 0;
    file.read(&value, 1);
    value ^= static_cast<char>(0x5a);
    file.seekp(137);
    file.write(&value, 1);
  }
  PipelinedExtractionConfig corruptedConfig = config;
  corruptedConfig.installFolder = root / "corrupted-output";
  const auto corruptedResult = extractor.Extract(corruptedConfig);
  Expect(!corruptedResult.ok, "Pipelined extraction should reject a corrupted volume");
  Expect(corruptedResult.message.find("SHA256 mismatch") != std::string::npos,
         "Pipelined corruption failure should identify the SHA256 mismatch");
  std::filesystem::remove_all(root);
}

#endif

void TestInstallerPackageLayout() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_flat_layout_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "data" / "package");
  { std::ofstream file(root / "data" / "package" / "manifest.json"); file << "{}"; }
  auto layout = LocateInstallerPackage(root);
  Expect(layout.manifestPath == root / "data" / "package" / "manifest.json" &&
             layout.archiveFolder == root / "data" / "downloads",
         "Older data packages must remain discoverable");
  const auto externalArchives = root.parent_path() / "modlist_flat_external_archives";
  std::filesystem::create_directories(externalArchives);
  std::error_code linkError;
  std::filesystem::create_directory_symlink(externalArchives, root / "data" / "downloads", linkError);
  if (!linkError) {
    Expect(InstallationOverlapsPackage(externalArchives, root),
           "Resolved legacy archive directories must be protected outside the EXE folder");
    std::filesystem::remove(root / "data" / "downloads");
  }
  std::filesystem::remove(externalArchives);
  { std::ofstream file(root / "manifest.json"); file << "{}"; }
  layout = LocateInstallerPackage(root);
  Expect(layout.manifestPath == root / "manifest.json" && layout.archiveFolder == root,
         "A flat manifest must take priority over an older data layout");
  Expect(InstallationOverlapsPackage(root, root) &&
             InstallationOverlapsPackage(root.parent_path(), root) &&
             InstallationOverlapsPackage(root / "installed", root) &&
             !InstallationOverlapsPackage(root.parent_path() / "modlist_flat_layout_tests_other", root),
         "Install and cleanup paths must never overlap the distributed package");
  const auto runtime = UserRuntimeFolder(root);
  Expect(runtime.is_absolute() && runtime != root &&
             runtime.wstring().find(root.wstring()) != 0,
         "Runtime tools must live outside the distributed package");
  NativeStrings strings;
  std::wstring warning;
  Expect(strings.LoadEmbedded(&warning), "Embedded translations should load without external files");
  const auto theme = EmbeddedTextResource(105);
  Expect(theme.ok() && theme.value().find(":root") != std::string::npos,
         "The installer theme must be embedded without an external stylesheet");
  Expect(strings.Get("button_install", L"missing") == L"\u0423\u0441\u0442\u0430\u043d\u043e\u0432\u0438\u0442\u044c",
         "Embedded translations should retain the Russian Install label");
  std::filesystem::remove_all(root);
}

void TestPackageDiscovery() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_installer_package_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
    std::ofstream out(root / "pack.torrent", std::ios::binary);
    out << "torrent";
  }
  {
    std::ofstream out(root / "modpack.7z.001", std::ios::binary);
    out << "archive";
  }

  auto package = DiscoverPackageNear(root);
  Expect(package.ok(), package.error().c_str());
  Expect(package.value().torrentFile.filename() == "pack.torrent", "Package discovery should find torrent");
  Expect(package.value().firstArchivePart.has_value(), "Package discovery should find .7z.001");
}

void TestPackageDiscoveryWithoutTorrent() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_installer_manifest_package_tests";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  {
    std::ofstream out(root / "modpack.7z.001", std::ios::binary);
    out << "archive";
  }

  auto package = DiscoverPackageNear(root);
  Expect(package.ok(), package.error().c_str());
  Expect(package.value().torrentFile.empty(), "Package discovery should not require torrent");
  Expect(package.value().firstArchivePart.has_value(), "Package discovery should still find archive");
}

void TestInstallSpacePlanning() {
  constexpr uintmax_t gib = 1024ull * 1024ull * 1024ull;
  const auto unknown = PlanInstallSpace(0, true);
  Expect(unknown.unpackRequiredBytes == 0 && unknown.installRequiredBytes == 0,
         "Unknown payload size should remain unknown");

  const auto sameVolume = PlanInstallSpace(10 * gib, true);
  Expect(sameVolume.unpackRequiredBytes == 10 * gib + 512ull * 1024ull * 1024ull,
         "Same-volume unpack requirement should include extraction overhead");
  Expect(sameVolume.installRequiredBytes == 0,
         "Same-volume install should not require a second payload copy");

  const auto crossVolume = PlanInstallSpace(20 * gib, false);
  Expect(crossVolume.unpackRequiredBytes == 21 * gib,
         "Large unpack requirement should include five percent overhead");
  Expect(crossVolume.installRequiredBytes == 20 * gib,
         "Cross-volume install should require one payload copy");
}

void TestSameVolumeDetection() {
  const auto root = std::filesystem::temp_directory_path() / "modlist_installer_volume_tests";
  const auto left = root / "left";
  const auto right = root / "right";
  std::filesystem::create_directories(left);
  std::filesystem::create_directories(right);

  PathValidator validator;
  Expect(validator.IsSameDrive(left, right), "Folders on the same Windows volume should match");
}

}  // namespace

int main() {
  try {
#ifdef _WIN32
    Expect(FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(24)) != nullptr,
           "Windows application manifest must be embedded as resource type 24");
#endif
    TestSha256();
    TestProgressThrottle();
    TestManifestLoader();
    TestPackerManifestLoader();
    TestUtf8ManifestArchivePaths();
    TestExecutableCacheRepairsSameSizeTampering();
    TestPackerManifestRejectsInvalidChunks();
    TestManifestRejectsUnsafeArchiveName();
    TestPackerManifestInfersArchiveName();
    TestArchiveInstallFolder();
    TestAutomaticUnpackFolder();
    TestArchiveExtractionPlan();
    TestManifestRejectsTraversal();
    TestUnicodeManifestPathSafety();
    TestJsonUnicodeEscapes();
    TestTrackerParsing();
    TestVerifier();
    TestExtractorCommand();
#ifdef _WIN32
    TestWindowsMoveRetriesTransientLock();
    TestWindowsMoveFallsBackToIndividualLockedEntry();
    TestPathValidatorRejectsFolderWithoutDeleteAccess();
    TestPathValidatorAcceptsFolderWithDeleteAccess();
    TestPathValidatorAcceptsLongWritableFolder();
    TestWindowsMoveAcceptsLockedEmptySourceAfterFilesMove();
    TestWindowsMoveAcceptsLockedEmptyNestedSourceAfterFilesMove();
    TestInstallMoveFailureProvidesManualRecoveryPaths();
    TestInstallerHidesManualUnpackControls();
    TestPipelineDoesNotForcePerFileFlush();
    TestPipelinedExtractor();
#endif
    TestPackageDiscovery();
    TestInstallerPackageLayout();
    TestPackageDiscoveryWithoutTorrent();
    TestInstallSpacePlanning();
    TestSameVolumeDetection();
  } catch (const std::exception& ex) {
    std::cerr << "Test failed: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "All tests passed\n";
  return 0;
}
