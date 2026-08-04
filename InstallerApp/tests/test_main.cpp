#include "app/PackageDiscovery.h"
#include "extractor/PipelinedSevenZipExtractor.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Json.h"
#include "manifest/Manifest.h"
#include "paths/PathValidator.h"
#include "tracker/TrackerProvider.h"
#include "verifier/Sha256.h"
#include "verifier/Verifier.h"

#include <filesystem>
#include <fstream>
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

  const auto sevenZip = std::filesystem::current_path() / "resources" / "7z.exe";
  const auto archive = archives / "pipeline.7z";
  const std::wstring arguments = L"a " + QuoteCommandArgument(archive) + L" " +
      QuoteCommandArgument(payload.filename()) + L" -mx=1 -v1m";
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
  Expect(library.value() == sdkCache / "data" / "tools" / "7zip" / "7z.dll",
         "Pipeline test should use the embedded 7-Zip SDK library");
  Expect(std::filesystem::is_regular_file(
             sdkCache / "data" / "tools" / "7zip" / "License.txt"),
         "Pipeline should extract the embedded 7-Zip license");
  PipelinedExtractionConfig config;
  config.sevenZipLibrary = library.value();
  config.archiveFolder = archives;
  config.installFolder = output;
  config.manifest = &manifest;
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
    TestSha256();
    TestManifestLoader();
    TestPackerManifestLoader();
    TestPackerManifestRejectsInvalidChunks();
    TestManifestRejectsUnsafeArchiveName();
    TestPackerManifestInfersArchiveName();
    TestArchiveInstallFolder();
    TestManifestRejectsTraversal();
    TestUnicodeManifestPathSafety();
    TestJsonUnicodeEscapes();
    TestTrackerParsing();
    TestVerifier();
    TestExtractorCommand();
#ifdef _WIN32
    TestPipelinedExtractor();
#endif
    TestPackageDiscovery();
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
