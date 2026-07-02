#include "app/PackageDiscovery.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Json.h"
#include "manifest/Manifest.h"
#include "paths/PathValidator.h"
#include "tracker/TrackerProvider.h"
#include "verifier/Sha256.h"
#include "verifier/Verifier.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

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
}

void TestPackerManifestLoader() {
  ManifestLoader loader;
  auto manifest = loader.LoadFromString(PackerManifestJson(Sha256::HexDigest("abc")));
  Expect(manifest.ok(), manifest.error().c_str());
  Expect(manifest.value().version == "modlist-manifest-chunks-v1", "Packer manifest schema mismatch");
  Expect(manifest.value().files.size() == 1, "Packer manifest file count mismatch");
  Expect(manifest.value().files[0].path == "MyPack.7z.001", "Packer manifest file path mismatch");
  Expect(manifest.value().extract.firstArchivePart == "MyPack.7z.001", "Packer manifest archive mismatch");
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

}  // namespace

int main() {
  try {
    TestSha256();
    TestManifestLoader();
    TestPackerManifestLoader();
    TestManifestRejectsTraversal();
    TestJsonUnicodeEscapes();
    TestTrackerParsing();
    TestVerifier();
    TestExtractorCommand();
    TestPackageDiscovery();
    TestPackageDiscoveryWithoutTorrent();
  } catch (const std::exception& ex) {
    std::cerr << "Test failed: " << ex.what() << "\n";
    return 1;
  }
  std::cout << "All tests passed\n";
  return 0;
}
