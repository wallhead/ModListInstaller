#include "manifest/Manifest.h"

#include "manifest/Json.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace modlist {

namespace {

Result<void> RequireObject(const JsonValue* value, const std::string& name) {
  if (value == nullptr || !value->IsObject()) {
    return Result<void>::Error("Manifest field '" + name + "' must be an object");
  }
  return Result<void>::Ok();
}

Result<void> RequireString(const JsonValue* value, const std::string& name) {
  if (value == nullptr || !value->IsString() || value->AsString().empty()) {
    return Result<void>::Error("Manifest field '" + name + "' must be a non-empty string");
  }
  return Result<void>::Ok();
}

bool IsHexSha256(const std::string& value) {
  return value.size() == 64 &&
         std::all_of(value.begin(), value.end(), [](unsigned char c) {
           return std::isxdigit(c) != 0;
         });
}

std::string NormalizePathForCheck(std::filesystem::path path) {
  return path.generic_string();
}

Result<ManifestFile> ParseFile(const JsonValue& json, size_t index) {
  if (!json.IsObject()) {
    return Result<ManifestFile>::Error("Manifest file entry " + std::to_string(index) + " must be an object");
  }
  const JsonValue* path = json.Find("path");
  const JsonValue* size = json.Find("size");
  const JsonValue* hash = json.Find("sha256");
  if (auto required = RequireString(path, "files[].path"); !required.ok()) {
    return Result<ManifestFile>::Error(required.error());
  }
  if (size == nullptr || !size->IsNumber() || size->AsNumber(-1) < 0) {
    return Result<ManifestFile>::Error("Manifest field 'files[].size' must be a non-negative number");
  }
  if (auto required = RequireString(hash, "files[].sha256"); !required.ok()) {
    return Result<ManifestFile>::Error(required.error());
  }

  ManifestFile file;
  file.path = path->AsString();
  file.size = static_cast<uint64_t>(size->AsNumber());
  file.sha256 = hash->AsString();
  std::transform(file.sha256.begin(), file.sha256.end(), file.sha256.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (!IsSafeManifestRelativePath(file.path)) {
    return Result<ManifestFile>::Error("Manifest file path is unsafe: " + NormalizePathForCheck(file.path));
  }
  if (!IsHexSha256(file.sha256)) {
    return Result<ManifestFile>::Error("Manifest SHA256 must be 64 hexadecimal characters: " + NormalizePathForCheck(file.path));
  }
  return Result<ManifestFile>::Ok(std::move(file));
}

}  // namespace

Result<Manifest> ManifestLoader::LoadFromFile(const std::filesystem::path& path) const {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Result<Manifest>::Error("Unable to open manifest: " + path.string());
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return LoadFromString(buffer.str());
}

Result<Manifest> ManifestLoader::LoadFromString(const std::string& jsonText) const {
  auto parsed = ParseJson(jsonText);
  if (!parsed.ok()) {
    return Result<Manifest>::Error(parsed.error());
  }
  const JsonValue& root = parsed.value();
  if (!root.IsObject()) {
    return Result<Manifest>::Error("Manifest root must be an object");
  }

  Manifest manifest;
  const JsonValue* version = root.Find("version");
  if (auto required = RequireString(version, "version"); !required.ok()) {
    return Result<Manifest>::Error(required.error());
  }
  manifest.version = version->AsString();

  const JsonValue* torrent = root.Find("torrent");
  if (auto required = RequireObject(torrent, "torrent"); !required.ok()) {
    return Result<Manifest>::Error(required.error());
  }
  const JsonValue* torrentType = torrent->Find("type");
  const JsonValue* torrentSource = torrent->Find("source");
  if (auto required = RequireString(torrentType, "torrent.type"); !required.ok()) {
    return Result<Manifest>::Error(required.error());
  }
  if (auto required = RequireString(torrentSource, "torrent.source"); !required.ok()) {
    return Result<Manifest>::Error(required.error());
  }
  if (torrentType->AsString() == "magnet") {
    manifest.torrent.type = TorrentSourceType::Magnet;
    if (torrentSource->AsString().find("magnet:?") != 0) {
      return Result<Manifest>::Error("Magnet source must start with magnet:?");
    }
  } else if (torrentType->AsString() == "torrent") {
    manifest.torrent.type = TorrentSourceType::TorrentFile;
  } else {
    return Result<Manifest>::Error("torrent.type must be 'magnet' or 'torrent'");
  }
  manifest.torrent.source = torrentSource->AsString();

  if (const JsonValue* trackers = root.Find("trackers"); trackers != nullptr) {
    if (auto required = RequireObject(trackers, "trackers"); !required.ok()) {
      return Result<Manifest>::Error(required.error());
    }
    if (const JsonValue* value = trackers->Find("use_online_tracker_list"); value != nullptr) {
      manifest.trackers.useOnlineTrackerList = value->AsBool(manifest.trackers.useOnlineTrackerList);
    }
    if (const JsonValue* value = trackers->Find("tracker_list_url"); value != nullptr && value->IsString()) {
      manifest.trackers.trackerListUrl = value->AsString();
    }
    if (const JsonValue* value = trackers->Find("continue_if_tracker_list_fails"); value != nullptr) {
      manifest.trackers.continueIfTrackerListFails = value->AsBool(manifest.trackers.continueIfTrackerListFails);
    }
  }

  if (const JsonValue* features = root.Find("torrent_features"); features != nullptr) {
    if (auto required = RequireObject(features, "torrent_features"); !required.ok()) {
      return Result<Manifest>::Error(required.error());
    }
    if (const JsonValue* value = features->Find("enable_dht"); value != nullptr) {
      manifest.torrentFeatures.enableDht = value->AsBool(manifest.torrentFeatures.enableDht);
    }
    if (const JsonValue* value = features->Find("enable_pex"); value != nullptr) {
      manifest.torrentFeatures.enablePex = value->AsBool(manifest.torrentFeatures.enablePex);
    }
    if (const JsonValue* value = features->Find("enable_lsd"); value != nullptr) {
      manifest.torrentFeatures.enableLsd = value->AsBool(manifest.torrentFeatures.enableLsd);
    }
  }

  if (const JsonValue* install = root.Find("install"); install != nullptr) {
    if (auto required = RequireObject(install, "install"); !required.ok()) {
      return Result<Manifest>::Error(required.error());
    }
    if (const JsonValue* value = install->Find("ask_user"); value != nullptr) {
      manifest.install.askUser = value->AsBool(manifest.install.askUser);
    }
    if (const JsonValue* value = install->Find("prefer_short_root_path"); value != nullptr) {
      manifest.install.preferShortRootPath = value->AsBool(manifest.install.preferShortRootPath);
    }
    if (const JsonValue* value = install->Find("example_path"); value != nullptr && value->IsString()) {
      manifest.install.examplePath = value->AsString();
    }
    if (const JsonValue* value = install->Find("max_recommended_path_length"); value != nullptr && value->IsNumber()) {
      manifest.install.maxRecommendedPathLength = static_cast<size_t>(value->AsNumber());
    }
    if (const JsonValue* value = install->Find("same_disk_temp_only"); value != nullptr) {
      manifest.install.sameDiskTempOnly = value->AsBool(manifest.install.sameDiskTempOnly);
    }
  }

  const JsonValue* files = root.Find("files");
  if (files == nullptr || !files->IsArray() || files->AsArray().empty()) {
    return Result<Manifest>::Error("Manifest field 'files' must be a non-empty array");
  }
  for (size_t i = 0; i < files->AsArray().size(); ++i) {
    auto file = ParseFile(files->AsArray()[i], i);
    if (!file.ok()) {
      return Result<Manifest>::Error(file.error());
    }
    manifest.files.push_back(std::move(file.value()));
  }

  const JsonValue* extract = root.Find("extract");
  if (auto required = RequireObject(extract, "extract"); !required.ok()) {
    return Result<Manifest>::Error(required.error());
  }
  const JsonValue* firstArchivePart = extract->Find("first_archive_part");
  if (auto required = RequireString(firstArchivePart, "extract.first_archive_part"); !required.ok()) {
    return Result<Manifest>::Error(required.error());
  }
  manifest.extract.firstArchivePart = firstArchivePart->AsString();
  if (!IsSafeManifestRelativePath(manifest.extract.firstArchivePart)) {
    return Result<Manifest>::Error("extract.first_archive_part is unsafe");
  }
  if (const JsonValue* value = extract->Find("target_subfolder"); value != nullptr && value->IsString()) {
    manifest.extract.targetSubfolder = value->AsString();
    if (!manifest.extract.targetSubfolder.empty() && !IsSafeManifestRelativePath(manifest.extract.targetSubfolder)) {
      return Result<Manifest>::Error("extract.target_subfolder is unsafe");
    }
  }
  if (const JsonValue* value = extract->Find("cleanup_after_success"); value != nullptr) {
    manifest.extract.cleanupAfterSuccess = value->AsBool(manifest.extract.cleanupAfterSuccess);
  }
  if (const JsonValue* value = extract->Find("use_same_disk_temp"); value != nullptr) {
    manifest.extract.useSameDiskTemp = value->AsBool(manifest.extract.useSameDiskTemp);
  }

  return Result<Manifest>::Ok(std::move(manifest));
}

bool IsSafeManifestRelativePath(const std::filesystem::path& path) {
  if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const auto& part : path) {
    const std::string text = part.string();
    if (text == ".." || text == "." || text.empty()) {
      return false;
    }
  }
  return true;
}

}  // namespace modlist
