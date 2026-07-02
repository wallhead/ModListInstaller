#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <winioctl.h>
#include <bcrypt.h>

#include "resource.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int kSourceEdit = 1001;
constexpr int kSourceBrowse = 1002;
constexpr int kReleaseEdit = 1003;
constexpr int kReleaseBrowse = 1004;
constexpr int kArchiveNameEdit = 1007;
constexpr int kFormatCombo = 1008;
constexpr int kLevelCombo = 1009;
constexpr int kMethodCombo = 1010;
constexpr int kDictionaryCombo = 1011;
constexpr int kWordCombo = 1012;
constexpr int kSolidCombo = 1013;
constexpr int kVolumeCombo = 1014;
constexpr int kThreadsCombo = 1015;
constexpr int kChunkCombo = 1016;
constexpr int kCopyInstallerCheck = 1017;
constexpr int kBuildButton = 1018;
constexpr int kManifestButton = 1019;
constexpr int kStopButton = 1020;
constexpr int kLogEdit = 1021;
constexpr int kProgress = 1022;
constexpr int kStatusLabel = 1023;
constexpr int kParametersEdit = 1024;
constexpr int kPathModeCombo = 1025;
constexpr int kTestArchiveCheck = 1026;

constexpr UINT kLogMessage = WM_APP + 1;
constexpr UINT kStatusMessage = WM_APP + 2;
constexpr UINT kProgressMessage = WM_APP + 3;
constexpr UINT kWorkerFinishedMessage = WM_APP + 4;

HINSTANCE g_instance = nullptr;
HWND g_sourceEdit = nullptr;
HWND g_releaseEdit = nullptr;
HWND g_archiveNameEdit = nullptr;
HWND g_formatCombo = nullptr;
HWND g_levelCombo = nullptr;
HWND g_methodCombo = nullptr;
HWND g_dictionaryCombo = nullptr;
HWND g_wordCombo = nullptr;
HWND g_solidCombo = nullptr;
HWND g_volumeCombo = nullptr;
HWND g_threadsCombo = nullptr;
HWND g_chunkCombo = nullptr;
HWND g_copyInstallerCheck = nullptr;
HWND g_logEdit = nullptr;
HWND g_progress = nullptr;
HWND g_statusLabel = nullptr;
HWND g_parametersEdit = nullptr;
HWND g_pathModeCombo = nullptr;
HWND g_testArchiveCheck = nullptr;
std::atomic_bool g_workerRunning{false};
std::atomic_bool g_cancelRequested{false};

struct PackerConfig {
  std::filesystem::path sourceFolder;
  std::filesystem::path releaseFolder;
  std::filesystem::path sevenZipExe;
  std::wstring archiveName;
  std::wstring format;
  std::wstring level;
  std::wstring method;
  std::wstring dictionary;
  std::wstring wordSize;
  std::wstring solid;
  std::wstring volumeSize;
  std::wstring threads;
  std::wstring parameters;
  std::wstring pathMode;
  uint64_t chunkSize{64ull * 1024ull * 1024ull};
  bool copyInstaller{true};
  bool testArchive{true};
  bool archiveFirst{true};
};

std::wstring Widen(const std::string& text);

std::filesystem::path ModuleFolder() {
  std::wstring buffer(MAX_PATH, L'\0');
  DWORD size = 0;
  while (true) {
    size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0) {
      return std::filesystem::current_path();
    }
    if (size < buffer.size() - 1) {
      buffer.resize(size);
      break;
    }
    buffer.resize(buffer.size() * 2);
  }
  return std::filesystem::path(buffer).parent_path();
}

std::filesystem::path LocalToolCacheFolder() {
  std::wstring localAppData(MAX_PATH, L'\0');
  const DWORD localAppDataSize = GetEnvironmentVariableW(
      L"LOCALAPPDATA", localAppData.data(), static_cast<DWORD>(localAppData.size()));
  if (localAppDataSize > 0 && localAppDataSize < localAppData.size()) {
    localAppData.resize(localAppDataSize);
    return std::filesystem::path(localAppData) / "ModlistPacker" / "tools" / "7zip";
  }

  std::error_code ec;
  auto temp = std::filesystem::temp_directory_path(ec);
  if (!ec) {
    return temp / "ModlistPacker" / "tools" / "7zip";
  }

  return ModuleFolder() / "tools" / "7zip";
}

std::optional<std::filesystem::path> ExtractEmbeddedSevenZip(std::wstring& error) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_7ZIP_EXE), MAKEINTRESOURCEW(10));
  if (resource == nullptr) {
    error = L"Embedded 7-Zip resource was not found.";
    return std::nullopt;
  }

  HGLOBAL loaded = LoadResource(module, resource);
  const DWORD size = SizeofResource(module, resource);
  const void* data = LockResource(loaded);
  if (loaded == nullptr || data == nullptr || size == 0) {
    error = L"Embedded 7-Zip resource could not be loaded.";
    return std::nullopt;
  }

  const auto outputFolder = LocalToolCacheFolder();
  std::error_code ec;
  std::filesystem::create_directories(outputFolder, ec);
  if (ec) {
    error = L"Unable to create embedded 7-Zip cache folder: " + Widen(ec.message());
    return std::nullopt;
  }

  const auto outputPath = outputFolder / "7z.exe";
  bool writeFile = true;
  if (std::filesystem::exists(outputPath, ec) && !ec) {
    const auto existingSize = std::filesystem::file_size(outputPath, ec);
    writeFile = ec || existingSize != size;
  }

  if (writeFile) {
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = L"Unable to write embedded 7-Zip executable.";
      return std::nullopt;
    }
    output.write(static_cast<const char*>(data), size);
    if (!output) {
      error = L"Unable to finish writing embedded 7-Zip executable.";
      return std::nullopt;
    }
  }

  return outputPath;
}

std::wstring GetText(HWND hwnd) {
  const int length = GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(length));
  return text;
}

void SetText(HWND hwnd, const std::wstring& text) {
  SetWindowTextW(hwnd, text.c_str());
}

std::wstring ComboText(HWND hwnd) {
  const int index = static_cast<int>(SendMessageW(hwnd, CB_GETCURSEL, 0, 0));
  if (index < 0) {
    return {};
  }
  const int length = static_cast<int>(SendMessageW(hwnd, CB_GETLBTEXTLEN, index, 0));
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  SendMessageW(hwnd, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(text.data()));
  text.resize(static_cast<size_t>(length));
  return text;
}

void AddComboItem(HWND combo, const wchar_t* text) {
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
}

void SelectCombo(HWND combo, int index) {
  SendMessageW(combo, CB_SETCURSEL, index, 0);
}

std::wstring Quote(const std::filesystem::path& path) {
  std::wstring text = path.wstring();
  std::wstring escaped;
  escaped.reserve(text.size() + 2);
  escaped.push_back(L'"');
  for (wchar_t ch : text) {
    if (ch == L'"') {
      escaped += L"\\\"";
    } else {
      escaped.push_back(ch);
    }
  }
  escaped.push_back(L'"');
  return escaped;
}

std::wstring FormatBytes(uint64_t bytes) {
  const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  std::wostringstream out;
  out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << L" " << units[unit];
  return out.str();
}

std::wstring FormatDuration(int64_t seconds) {
  if (seconds < 0) {
    return L"unknown";
  }
  const int64_t hours = seconds / 3600;
  const int64_t minutes = (seconds % 3600) / 60;
  const int64_t secs = seconds % 60;
  std::wostringstream out;
  if (hours > 0) {
    out << hours << L"h " << minutes << L"m";
  } else if (minutes > 0) {
    out << minutes << L"m " << secs << L"s";
  } else {
    out << secs << L"s";
  }
  return out.str();
}

std::wstring FormatBytesPerSecond(uint64_t bytesPerSecond) {
  return FormatBytes(bytesPerSecond) + L"/s";
}

std::wstring FormatProgressStatus(const std::wstring& label,
                                  int percent,
                                  uint64_t doneBytes,
                                  uint64_t totalBytes,
                                  uint64_t bytesPerSecond,
                                  int64_t etaSeconds,
                                  int64_t elapsedSeconds) {
  std::wostringstream out;
  out << label << L" " << percent << L"%";
  if (totalBytes > 0) {
    out << L" | " << FormatBytes(doneBytes) << L" / " << FormatBytes(totalBytes);
  }
  if (bytesPerSecond > 0) {
    out << L" | " << FormatBytesPerSecond(bytesPerSecond);
  }
  out << L" | ETA " << FormatDuration(etaSeconds)
      << L" | Elapsed " << FormatDuration(elapsedSeconds);
  return out.str();
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
  return result;
}

std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring result(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
  return result;
}

std::string JsonEscape(const std::string& text) {
  std::ostringstream out;
  for (unsigned char ch : text) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
        } else {
          out << static_cast<char>(ch);
        }
        break;
    }
  }
  return out.str();
}

void AppendLog(const std::wstring& text) {
  const int length = GetWindowTextLengthW(g_logEdit);
  SendMessageW(g_logEdit, EM_SETSEL, length, length);
  std::wstring line = text + L"\r\n";
  SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void PostLog(HWND hwnd, std::wstring text) {
  PostMessageW(hwnd, kLogMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(std::move(text))));
}

void PostStatus(HWND hwnd, std::wstring text) {
  PostMessageW(hwnd, kStatusMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(std::move(text))));
}

void PostProgress(HWND hwnd, int progress) {
  PostMessageW(hwnd, kProgressMessage, static_cast<WPARAM>(progress), 0);
}

std::optional<std::filesystem::path> PickFolder(HWND owner) {
  BROWSEINFOW browse{};
  browse.hwndOwner = owner;
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
  if (item == nullptr) {
    return std::nullopt;
  }
  wchar_t path[MAX_PATH]{};
  const BOOL ok = SHGetPathFromIDListW(item, path);
  CoTaskMemFree(item);
  if (!ok) {
    return std::nullopt;
  }
  return std::filesystem::path(path);
}

uint64_t ParseSizeText(const std::wstring& text, uint64_t fallback) {
  if (text.empty()) {
    return fallback;
  }
  wchar_t suffix = text.back();
  std::wstring number = text;
  uint64_t multiplier = 1;
  if (!iswdigit(suffix)) {
    number.pop_back();
    suffix = static_cast<wchar_t>(towlower(suffix));
    if (suffix == L'k') {
      multiplier = 1024ull;
    } else if (suffix == L'm') {
      multiplier = 1024ull * 1024ull;
    } else if (suffix == L'g') {
      multiplier = 1024ull * 1024ull * 1024ull;
    }
  }
  try {
    return std::stoull(number) * multiplier;
  } catch (...) {
    return fallback;
  }
}

uint64_t EstimateFolderBytes(const std::filesystem::path& folder) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
    return 0;
  }

  uint64_t total = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           folder, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    total += entry.file_size(ec);
    if (ec) {
      ec.clear();
    }
  }
  return total;
}

std::wstring SelectArchiveExtension(const PackerConfig& config) {
  if (config.format == L"zip") {
    return L".zip";
  }
  return L".7z";
}

std::wstring SevenZipVolumeSize(const std::wstring& value) {
  if (value == L"4092M - FAT") {
    return L"4092m";
  }
  return value;
}

std::filesystem::path ArchivePath(const PackerConfig& config) {
  return config.releaseFolder / (config.archiveName + SelectArchiveExtension(config));
}

bool IsSafeArchiveName(const std::wstring& name) {
  if (name.empty() || name == L"." || name == L"..") {
    return false;
  }
  if (name.back() == L'.' || name.back() == L' ') {
    return false;
  }
  if (name.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) {
    return false;
  }
  const std::filesystem::path path(name);
  return !path.is_absolute() && path.filename().wstring() == name;
}

bool IsNumericVolumeSuffix(const std::wstring& suffix) {
  return !suffix.empty() &&
         std::all_of(suffix.begin(), suffix.end(), [](wchar_t ch) {
           return iswdigit(ch) != 0;
         });
}

std::wstring Lower(std::wstring text) {
  for (auto& ch : text) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return text;
}

bool EndsWithInsensitive(const std::wstring& text, const std::wstring& suffix) {
  const auto lowerText = Lower(text);
  const auto lowerSuffix = Lower(suffix);
  return lowerText.size() >= lowerSuffix.size() &&
         lowerText.compare(lowerText.size() - lowerSuffix.size(), lowerSuffix.size(), lowerSuffix) == 0;
}

std::optional<std::wstring> ArchiveBaseNameFromFileName(const std::wstring& name) {
  const auto lower = Lower(name);
  for (const auto* extension : {L".7z", L".zip"}) {
    const std::wstring ext(extension);
    if (EndsWithInsensitive(name, ext)) {
      return name;
    }

    const auto splitMarker = ext + L".";
    const auto markerPos = lower.rfind(splitMarker);
    if (markerPos != std::wstring::npos) {
      const auto suffix = name.substr(markerPos + splitMarker.size());
      if (IsNumericVolumeSuffix(suffix)) {
        return name.substr(0, markerPos + ext.size());
      }
    }
  }
  return std::nullopt;
}

std::wstring ArchiveNameFromBaseName(const std::wstring& baseName) {
  if (EndsWithInsensitive(baseName, L".7z")) {
    return baseName.substr(0, baseName.size() - 3);
  }
  if (EndsWithInsensitive(baseName, L".zip")) {
    return baseName.substr(0, baseName.size() - 4);
  }
  return baseName;
}

std::optional<bool> DriveIncursSeekPenalty(const std::filesystem::path& path) {
  std::error_code ec;
  const auto absolute = std::filesystem::absolute(path, ec);
  const auto rootName = (ec ? path : absolute).root_name().wstring();
  if (rootName.empty()) {
    return std::nullopt;
  }

  const std::wstring volumePath = L"\\\\.\\" + rootName;
  HANDLE volume = CreateFileW(volumePath.c_str(),
                              0,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              0,
                              nullptr);
  if (volume == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }

  STORAGE_PROPERTY_QUERY query{};
  query.PropertyId = StorageDeviceSeekPenaltyProperty;
  query.QueryType = PropertyStandardQuery;
  DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
  DWORD bytesReturned = 0;
  const BOOL ok = DeviceIoControl(volume,
                                  IOCTL_STORAGE_QUERY_PROPERTY,
                                  &query,
                                  sizeof(query),
                                  &descriptor,
                                  sizeof(descriptor),
                                  &bytesReturned,
                                  nullptr);
  CloseHandle(volume);
  if (!ok || bytesReturned < sizeof(descriptor)) {
    return std::nullopt;
  }
  return descriptor.IncursSeekPenalty != FALSE;
}

size_t SelectHashWorkerCount(const std::filesystem::path& folder, size_t fileCount) {
  if (fileCount == 0) {
    return 1;
  }
  const auto incursSeekPenalty = DriveIncursSeekPenalty(folder);
  if (!incursSeekPenalty.has_value() || *incursSeekPenalty) {
    return 1;
  }
  const unsigned int hardwareThreads = std::max(1u, std::thread::hardware_concurrency());
  return std::max<size_t>(1, std::min<size_t>(fileCount, std::min<size_t>(4, hardwareThreads)));
}

std::wstring BuildSevenZipCommand(const PackerConfig& config) {
  std::wstring command = Quote(config.sevenZipExe) + L" a " + Quote(ArchivePath(config)) + L" " +
                         Quote(config.sourceFolder / L"*") + L" -t" + config.format +
                         L" -mx=" + config.level +
                         L" -mmt=" + config.threads +
                         L" -y -bsp1";
  if (config.pathMode == L"Full paths") {
    command += L" -spf";
  }
  if (config.format == L"7z") {
    if (!config.method.empty()) {
      command += L" -m0=" + config.method;
    }
    if (!config.dictionary.empty()) {
      command += L" -md=" + config.dictionary;
    }
    if (!config.wordSize.empty()) {
      command += L" -mfb=" + config.wordSize;
    }
    if (config.solid == L"Solid") {
      command += L" -ms=on";
    } else if (config.solid == L"Non-solid") {
      command += L" -ms=off";
    } else if (!config.solid.empty()) {
      command += L" -ms=" + config.solid;
    }
  }
  const auto volumeSize = SevenZipVolumeSize(config.volumeSize);
  if (!volumeSize.empty() && volumeSize != L"none") {
    command += L" -v" + volumeSize;
  }
  if (!config.parameters.empty()) {
    command += L" " + config.parameters;
  }
  return command;
}

std::wstring BuildSevenZipTestCommand(const PackerConfig& config) {
  auto archive = ArchivePath(config);
  const auto volumeSize = SevenZipVolumeSize(config.volumeSize);
  if (!volumeSize.empty() && volumeSize != L"none") {
    archive += L".001";
  }
  return Quote(config.sevenZipExe) + L" t " + Quote(archive) + L" -y -bsp1";
}

std::wstring Hex(const std::vector<unsigned char>& bytes) {
  std::wostringstream out;
  out << std::hex << std::setfill(L'0');
  for (unsigned char byte : bytes) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

struct FileManifest {
  std::filesystem::path path;
  uint64_t size{0};
  std::wstring sha256;
  std::vector<std::wstring> chunks;
};

bool HashArchiveFile(HWND hwnd,
                     const std::filesystem::path& root,
                     const std::filesystem::path& file,
                     uint64_t chunkSize,
                     FileManifest& manifest,
                     std::atomic_uint64_t& doneBytes,
                     uint64_t totalBytes,
                     std::chrono::steady_clock::time_point startedAt) {
  std::ifstream input(file, std::ios::binary);
  if (!input) {
    PostLog(hwnd, L"Unable to open file for hashing: " + file.wstring());
    return false;
  }

  std::error_code ec;
  manifest.path = std::filesystem::relative(file, root, ec);
  if (ec) {
    manifest.path = file.filename();
  }
  manifest.size = std::filesystem::file_size(file, ec);
  if (ec) {
    manifest.size = 0;
  }

  BCRYPT_HASH_HANDLE fullHash = nullptr;
  if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &fullHash, nullptr, 0, nullptr, 0, 0) != 0) {
    PostLog(hwnd, L"Unable to initialize SHA-256.");
    return false;
  }

  BCRYPT_HASH_HANDLE chunkHash = nullptr;
  if (BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &chunkHash, nullptr, 0, nullptr, 0, 0) != 0) {
    BCryptDestroyHash(fullHash);
    PostLog(hwnd, L"Unable to initialize chunk SHA-256.");
    return false;
  }

  auto finishChunk = [&]() -> bool {
    std::vector<unsigned char> digest(32);
    const bool ok = BCryptFinishHash(chunkHash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(chunkHash);
    chunkHash = nullptr;
    if (!ok) {
      return false;
    }
    manifest.chunks.push_back(Hex(digest));
    return BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &chunkHash, nullptr, 0, nullptr, 0, 0) == 0;
  };

  std::vector<char> buffer(static_cast<size_t>(std::min<uint64_t>(chunkSize, 4ull * 1024ull * 1024ull)));
  uint64_t chunkDone = 0;
  while (input && !g_cancelRequested) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = input.gcount();
    if (read <= 0) {
      break;
    }
    BCryptHashData(fullHash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(read), 0);
    size_t offset = 0;
    while (offset < static_cast<size_t>(read)) {
      const uint64_t remainingInChunk = chunkSize - chunkDone;
      const size_t toHash = static_cast<size_t>(std::min<uint64_t>(
          remainingInChunk, static_cast<uint64_t>(read) - offset));
      BCryptHashData(chunkHash, reinterpret_cast<PUCHAR>(buffer.data() + offset), static_cast<ULONG>(toHash), 0);
      offset += toHash;
      chunkDone += toHash;
      if (chunkDone == chunkSize) {
        if (!finishChunk()) {
          BCryptDestroyHash(fullHash);
          return false;
        }
        chunkDone = 0;
      }
    }
    const uint64_t done = doneBytes.fetch_add(static_cast<uint64_t>(read)) + static_cast<uint64_t>(read);
    const int percent = totalBytes > 0 ? static_cast<int>((done * 100) / totalBytes) : 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    uint64_t speed = 0;
    int64_t eta = -1;
    if (elapsed > 0 && done > 0) {
      speed = done / static_cast<uint64_t>(elapsed);
      if (speed > 0 && totalBytes > done) {
        eta = static_cast<int64_t>((totalBytes - done) / speed);
      }
    }
    PostProgress(hwnd, percent);
    PostStatus(hwnd, FormatProgressStatus(L"Hashing", percent, done, totalBytes, speed, eta, elapsed));
  }

  if (chunkDone > 0) {
    std::vector<unsigned char> digest(32);
    const bool ok = BCryptFinishHash(chunkHash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(chunkHash);
    chunkHash = nullptr;
    if (!ok) {
      BCryptDestroyHash(fullHash);
      return false;
    }
    manifest.chunks.push_back(Hex(digest));
  }
  if (chunkHash != nullptr) {
    BCryptDestroyHash(chunkHash);
    chunkHash = nullptr;
  }

  std::vector<unsigned char> fullDigest(32);
  const bool ok = BCryptFinishHash(fullHash, fullDigest.data(), static_cast<ULONG>(fullDigest.size()), 0) == 0;
  BCryptDestroyHash(fullHash);
  if (!ok || !input.eof()) {
    PostLog(hwnd, L"Hashing failed: " + file.wstring());
    return false;
  }
  manifest.sha256 = Hex(fullDigest);
  return !g_cancelRequested;
}

struct ArchiveSet {
  std::wstring baseName;
  std::wstring archiveName;
  std::vector<std::filesystem::path> files;
};

std::vector<std::filesystem::path> FindArchivePartsForBase(const std::filesystem::path& folder, const std::wstring& base) {
  std::vector<std::filesystem::path> files;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const auto name = entry.path().filename().wstring();
    const auto volumePrefix = base + L".";
    if (name == base ||
        (name.starts_with(volumePrefix) && IsNumericVolumeSuffix(name.substr(volumePrefix.size())))) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<ArchiveSet> FindArchiveSets(const std::filesystem::path& folder) {
  std::vector<ArchiveSet> sets;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }

    const auto name = entry.path().filename().wstring();
    auto baseName = ArchiveBaseNameFromFileName(name);
    if (!baseName.has_value()) {
      continue;
    }

    auto existing = std::find_if(sets.begin(), sets.end(), [&](const ArchiveSet& set) {
      return Lower(set.baseName) == Lower(*baseName);
    });
    if (existing == sets.end()) {
      ArchiveSet set;
      set.baseName = *baseName;
      set.archiveName = ArchiveNameFromBaseName(*baseName);
      set.files.push_back(entry.path());
      sets.push_back(std::move(set));
    } else {
      existing->files.push_back(entry.path());
    }
  }

  for (auto& set : sets) {
    std::sort(set.files.begin(), set.files.end());
  }
  std::sort(sets.begin(), sets.end(), [](const ArchiveSet& left, const ArchiveSet& right) {
    return Lower(left.baseName) < Lower(right.baseName);
  });
  return sets;
}

std::optional<ArchiveSet> SelectArchiveSet(HWND hwnd, const PackerConfig& config) {
  if (config.archiveFirst) {
    ArchiveSet set;
    set.baseName = config.archiveName + SelectArchiveExtension(config);
    set.archiveName = config.archiveName;
    set.files = FindArchivePartsForBase(config.releaseFolder, set.baseName);
    return set;
  }

  auto sets = FindArchiveSets(config.releaseFolder);
  if (sets.empty()) {
    return std::nullopt;
  }

  if (!config.archiveName.empty()) {
    std::vector<ArchiveSet> namedMatches;
    for (const auto& set : sets) {
      if (Lower(set.archiveName) == Lower(config.archiveName)) {
        namedMatches.push_back(set);
      }
    }
    if (namedMatches.size() == 1) {
      return namedMatches.front();
    }
  }

  if (sets.size() == 1) {
    PostLog(hwnd, L"Manifest Only auto-detected archive: " + sets.front().baseName);
    return sets.front();
  }

  PostLog(hwnd, L"More than one archive set was found. Enter the wanted archive name first:");
  for (const auto& set : sets) {
    PostLog(hwnd, L"  " + set.archiveName + L" (" + std::to_wstring(set.files.size()) + L" file(s))");
  }
  return std::nullopt;
}

bool WriteManifest(HWND hwnd, const PackerConfig& config) {
  const auto archiveSet = SelectArchiveSet(hwnd, config);
  if (!archiveSet.has_value()) {
    PostLog(hwnd, L"No archive parts found in release folder.");
    return false;
  }

  const auto& files = archiveSet->files;
  if (files.empty()) {
    PostLog(hwnd, L"No archive parts found in release folder.");
    return false;
  }

  uint64_t totalBytes = 0;
  for (const auto& file : files) {
    std::error_code ec;
    totalBytes += std::filesystem::file_size(file, ec);
    if (ec) {
      ec.clear();
    }
  }

  PostLog(hwnd, L"Generating manifest for " + std::to_wstring(files.size()) + L" archive file(s).");
  const size_t workerCount = SelectHashWorkerCount(config.releaseFolder, files.size());
  PostLog(hwnd, workerCount == 1
                    ? L"Manifest hashing workers: 1 (sequential HDD-friendly read)"
                    : L"Manifest hashing workers: " + std::to_wstring(workerCount) + L" (SSD parallel read)");

  std::vector<FileManifest> manifests(files.size());
  std::deque<size_t> jobs;
  for (size_t i = 0; i < files.size(); ++i) {
    jobs.push_back(i);
  }

  std::mutex jobsMutex;
  std::atomic_uint64_t doneBytes{0};
  std::atomic_bool failed{false};
  const auto hashingStartedAt = std::chrono::steady_clock::now();

  auto fail = [&](std::wstring message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      PostLog(hwnd, std::move(message));
    }
  };

  auto worker = [&]() {
    while (!g_cancelRequested.load() && !failed.load()) {
      size_t index = 0;
      {
        std::lock_guard<std::mutex> lock(jobsMutex);
        if (jobs.empty()) {
          return;
        }
        index = jobs.front();
        jobs.pop_front();
      }

      PostLog(hwnd, L"Hashing archive file: " + files[index].filename().wstring());
      if (!HashArchiveFile(hwnd,
                           config.releaseFolder,
                           files[index],
                           config.chunkSize,
                           manifests[index],
                           doneBytes,
                           totalBytes,
                           hashingStartedAt)) {
        fail(L"Hashing failed: " + files[index].wstring());
        return;
      }
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(workerCount);
  for (size_t i = 0; i < workerCount; ++i) {
    workers.emplace_back(worker);
  }
  for (auto& thread : workers) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  if (g_cancelRequested.load()) {
    return false;
  }
  if (failed.load()) {
    return false;
  }

  std::error_code ec;
  const auto packageFolder = config.releaseFolder / "data" / "package";
  std::filesystem::create_directories(packageFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to create package folder: " + Widen(ec.message()));
    return false;
  }

  const auto manifestPath = packageFolder / "manifest.json";
  std::ofstream output(manifestPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    PostLog(hwnd, L"Unable to write manifest: " + manifestPath.wstring());
    return false;
  }

  output << "{\n";
  output << "  \"schema\": \"modlist-manifest-chunks-v1\",\n";
  output << "  \"archive_name\": \"" << JsonEscape(Narrow(archiveSet->archiveName)) << "\",\n";
  output << "  \"hash\": {\n";
  output << "    \"algorithm\": \"sha256\",\n";
  output << "    \"chunk_size\": " << config.chunkSize << "\n";
  output << "  },\n";
  output << "  \"files\": [\n";
  for (size_t i = 0; i < manifests.size(); ++i) {
    const auto& file = manifests[i];
    output << "    {\n";
    output << "      \"path\": \"" << JsonEscape(file.path.generic_string()) << "\",\n";
    output << "      \"size\": " << file.size << ",\n";
    output << "      \"sha256\": \"" << Narrow(file.sha256) << "\",\n";
    output << "      \"chunks\": [\n";
    for (size_t j = 0; j < file.chunks.size(); ++j) {
      output << "        \"" << Narrow(file.chunks[j]) << "\"";
      output << (j + 1 == file.chunks.size() ? "\n" : ",\n");
    }
    output << "      ]\n";
    output << "    }" << (i + 1 == manifests.size() ? "\n" : ",\n");
  }
  output << "  ]\n";
  output << "}\n";
  if (!output) {
    PostLog(hwnd, L"Unable to finish manifest write.");
    return false;
  }

  PostProgress(hwnd, 100);
  PostStatus(hwnd, L"Manifest complete");
  PostLog(hwnd, L"Manifest written: " + manifestPath.wstring());
  return true;
}

void UpdateProcessProgressStatus(HWND hwnd,
                                 const std::wstring& label,
                                 int percent,
                                 uint64_t estimatedBytes,
                                 std::chrono::steady_clock::time_point startedAt) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - startedAt).count();
  uint64_t done = 0;
  uint64_t speed = 0;
  int64_t eta = -1;
  if (estimatedBytes > 0) {
    done = (estimatedBytes * static_cast<uint64_t>(percent)) / 100;
    if (elapsed > 0 && done > 0) {
      speed = done / static_cast<uint64_t>(elapsed);
      if (speed > 0 && estimatedBytes > done) {
        eta = static_cast<int64_t>((estimatedBytes - done) / speed);
      }
    }
  }
  PostProgress(hwnd, percent);
  PostStatus(hwnd, FormatProgressStatus(label, percent, done, estimatedBytes, speed, eta, elapsed));
}

void ConsumeProcessOutput(HWND hwnd,
                          const char* buffer,
                          DWORD bytesRead,
                          std::string& tail,
                          int& lastPercent,
                          const std::wstring& label,
                          uint64_t estimatedBytes,
                          std::chrono::steady_clock::time_point startedAt) {
  tail.append(buffer, bytesRead);
  if (tail.size() > 4096) {
    tail.erase(0, tail.size() - 4096);
  }
  const auto percentPos = tail.rfind('%');
  if (percentPos == std::string::npos) {
    return;
  }
  size_t begin = percentPos;
  while (begin > 0 && std::isdigit(static_cast<unsigned char>(tail[begin - 1]))) {
    --begin;
  }
  if (begin >= percentPos) {
    return;
  }
  const int percent = std::stoi(tail.substr(begin, percentPos - begin));
  if (percent >= 0 && percent <= 100 && percent != lastPercent) {
    lastPercent = percent;
    UpdateProcessProgressStatus(hwnd, label, percent, estimatedBytes, startedAt);
  }
}

int RunProcess(HWND hwnd, const std::wstring& command, const std::wstring& label, uint64_t estimatedBytes) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
    return static_cast<int>(GetLastError());
  }
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = writePipe;
  startup.hStdError = writePipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::wstring mutableCommand = command;
  const BOOL created = CreateProcessW(nullptr,
                                      mutableCommand.data(),
                                      nullptr,
                                      nullptr,
                                      TRUE,
                                      CREATE_NO_WINDOW,
                                      nullptr,
                                      nullptr,
                                      &startup,
                                      &process);
  CloseHandle(writePipe);
  if (!created) {
    CloseHandle(readPipe);
    return static_cast<int>(GetLastError());
  }

  char buffer[4096];
  std::string tail;
  int lastPercent = -1;
  const auto startedAt = std::chrono::steady_clock::now();
  UpdateProcessProgressStatus(hwnd, label, 0, estimatedBytes, startedAt);

  while (true) {
    if (g_cancelRequested.load()) {
      TerminateProcess(process.hProcess, 255);
      break;
    }

    DWORD available = 0;
    while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
      DWORD bytesRead = 0;
      const DWORD toRead = std::min<DWORD>(available, sizeof(buffer));
      if (!ReadFile(readPipe, buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
        break;
      }
      ConsumeProcessOutput(hwnd, buffer, bytesRead, tail, lastPercent, label, estimatedBytes, startedAt);
      available = 0;
    }

    const DWORD wait = WaitForSingleObject(process.hProcess, 100);
    if (wait == WAIT_OBJECT_0) {
      DWORD bytesRead = 0;
      while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        ConsumeProcessOutput(hwnd, buffer, bytesRead, tail, lastPercent, label, estimatedBytes, startedAt);
      }
      break;
    }
  }

  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  CloseHandle(readPipe);
  return static_cast<int>(exitCode);
}

void CopyInstallerIfRequested(HWND hwnd, const PackerConfig& config) {
  if (!config.copyInstaller) {
    return;
  }
  const auto installerDist = ModuleFolder().parent_path().parent_path() / "InstallerApp" / "dist";
  const auto source = installerDist / "modlist-installer.exe";
  const auto target = config.releaseFolder / "modlist-installer.exe";
  std::error_code ec;
  if (std::filesystem::exists(source, ec)) {
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      PostLog(hwnd, L"Installer copy warning: " + Widen(ec.message()));
    } else {
      PostLog(hwnd, L"Installer copied: " + target.wstring());
    }
  } else {
    PostLog(hwnd, L"Installer copy warning: installer exe was not found next to the repo.");
  }

  const auto sourceUi = installerDist / "ui";
  const auto targetUi = config.releaseFolder / "ui";
  if (std::filesystem::exists(sourceUi, ec) && std::filesystem::is_directory(sourceUi, ec)) {
    std::filesystem::remove_all(targetUi, ec);
    ec.clear();
    std::filesystem::copy(sourceUi, targetUi, std::filesystem::copy_options::recursive, ec);
    if (ec) {
      PostLog(hwnd, L"Installer UI copy warning: " + Widen(ec.message()));
    } else {
      PostLog(hwnd, L"Installer UI copied: " + targetUi.wstring());
    }
  } else {
    PostLog(hwnd, L"Installer UI copy warning: ui folder was not found next to the installer exe.");
  }

  for (const auto& folder : {config.releaseFolder / "data" / "package",
                            config.releaseFolder / "data" / "downloads",
                            config.releaseFolder / "data" / "logs",
                            config.releaseFolder / "data" / "tools" / "7zip"}) {
    ec.clear();
    std::filesystem::create_directories(folder, ec);
    if (ec) {
      PostLog(hwnd, L"Runtime folder warning: " + folder.wstring() + L" - " + Widen(ec.message()));
    }
  }
}

PackerConfig ReadConfig(bool archiveFirst) {
  PackerConfig config;
  config.sourceFolder = GetText(g_sourceEdit);
  config.releaseFolder = GetText(g_releaseEdit);
  config.archiveName = GetText(g_archiveNameEdit);
  config.format = ComboText(g_formatCombo);
  config.level = ComboText(g_levelCombo);
  config.method = ComboText(g_methodCombo);
  config.dictionary = ComboText(g_dictionaryCombo);
  config.wordSize = ComboText(g_wordCombo);
  config.solid = ComboText(g_solidCombo);
  config.volumeSize = ComboText(g_volumeCombo);
  config.threads = ComboText(g_threadsCombo);
  config.parameters = GetText(g_parametersEdit);
  config.pathMode = ComboText(g_pathModeCombo);
  config.chunkSize = ParseSizeText(ComboText(g_chunkCombo), 64ull * 1024ull * 1024ull);
  config.copyInstaller = SendMessageW(g_copyInstallerCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
  config.testArchive = SendMessageW(g_testArchiveCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
  config.archiveFirst = archiveFirst;
  return config;
}

bool ValidateConfig(HWND hwnd, const PackerConfig& config) {
  std::error_code ec;
  if (config.releaseFolder.empty()) {
    PostLog(hwnd, L"Release folder is required.");
    return false;
  }
  std::filesystem::create_directories(config.releaseFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to create release folder: " + Widen(ec.message()));
    return false;
  }
  if (config.archiveName.empty()) {
    PostLog(hwnd, L"Archive name is required.");
    return false;
  }
  if (!IsSafeArchiveName(config.archiveName)) {
    PostLog(hwnd, L"Archive name cannot contain path separators, reserved characters, or parent-directory segments.");
    return false;
  }
  if (config.archiveFirst) {
    if (!std::filesystem::exists(config.sourceFolder, ec) || !std::filesystem::is_directory(config.sourceFolder, ec)) {
      PostLog(hwnd, L"Source folder is required.");
      return false;
    }
  }
  return true;
}

bool ResolveSevenZip(HWND hwnd, PackerConfig& config) {
  if (!config.archiveFirst) {
    return true;
  }

  std::wstring error;
  auto embedded = ExtractEmbeddedSevenZip(error);
  if (!embedded.has_value()) {
    PostLog(hwnd, L"Embedded 7-Zip error: " + error);
    return false;
  }
  config.sevenZipExe = *embedded;
  PostLog(hwnd, L"Using embedded 7-Zip: " + config.sevenZipExe.wstring());
  return true;
}

void Worker(HWND hwnd, PackerConfig config) {
  g_workerRunning = true;
  g_cancelRequested = false;
  PostProgress(hwnd, 0);

  if (!ResolveSevenZip(hwnd, config) || !ValidateConfig(hwnd, config)) {
    g_workerRunning = false;
    PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
    return;
  }

  bool ok = true;
  if (config.archiveFirst) {
    PostLog(hwnd, L"Creating archive...");
    PostLog(hwnd, L"7-Zip command: " + BuildSevenZipCommand(config));
    const uint64_t sourceBytes = EstimateFolderBytes(config.sourceFolder);
    if (sourceBytes > 0) {
      PostLog(hwnd, L"Packing input size estimate (uncompressed source): " + FormatBytes(sourceBytes));
    }
    PostStatus(hwnd, L"Packing input");
    const int exitCode = RunProcess(hwnd, BuildSevenZipCommand(config), L"Packing input", sourceBytes);
    if (exitCode != 0) {
      PostLog(hwnd, L"7-Zip failed with exit code " + std::to_wstring(exitCode));
      ok = false;
    } else {
      PostLog(hwnd, L"Archive creation completed.");
    }
    if (ok && config.testArchive && !g_cancelRequested) {
      PostLog(hwnd, L"Testing archive...");
      PostStatus(hwnd, L"Testing archive");
      uint64_t archiveBytes = 0;
      for (const auto& file : FindArchivePartsForBase(config.releaseFolder, config.archiveName + SelectArchiveExtension(config))) {
        std::error_code ec;
        archiveBytes += std::filesystem::file_size(file, ec);
        if (ec) {
          ec.clear();
        }
      }
      const int testExitCode = RunProcess(hwnd, BuildSevenZipTestCommand(config), L"Testing archive", archiveBytes);
      if (testExitCode != 0) {
        PostLog(hwnd, L"7-Zip test failed with exit code " + std::to_wstring(testExitCode));
        ok = false;
      } else {
        PostLog(hwnd, L"Archive test completed.");
      }
    }
  }

  if (ok && !g_cancelRequested) {
    CopyInstallerIfRequested(hwnd, config);
    ok = WriteManifest(hwnd, config);
  }

  if (g_cancelRequested) {
    PostLog(hwnd, L"Stopped.");
    PostStatus(hwnd, L"Stopped");
  } else if (ok) {
    PostLog(hwnd, L"Package ready.");
    PostStatus(hwnd, L"Package ready");
  }
  g_workerRunning = false;
  PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, g_instance, nullptr);
}

HWND CreateEdit(HWND parent, int id, int x, int y, int w, int h) {
  return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                         x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                         x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

HWND CreateCombo(HWND parent, int id, int x, int y, int w, int h) {
  return CreateWindowExW(0, WC_COMBOBOXW, L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                         x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

HWND CreateGroupBox(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
  return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                         x, y, w, h, parent, nullptr, g_instance, nullptr);
}

void EnableRunningControls(HWND hwnd, bool running) {
  EnableWindow(GetDlgItem(hwnd, kBuildButton), !running);
  EnableWindow(GetDlgItem(hwnd, kManifestButton), !running);
  EnableWindow(GetDlgItem(hwnd, kStopButton), running);
}

void StartWork(HWND hwnd, bool archiveFirst) {
  if (g_workerRunning) {
    return;
  }
  EnableRunningControls(hwnd, true);
  std::thread(Worker, hwnd, ReadConfig(archiveFirst)).detach();
}

void SetDefaults(HWND hwnd) {
  (void)hwnd;
  AddComboItem(g_formatCombo, L"7z");
  AddComboItem(g_formatCombo, L"zip");
  SelectCombo(g_formatCombo, 0);

  for (const wchar_t* item : {L"0", L"1", L"3", L"5", L"7", L"9"}) {
    AddComboItem(g_levelCombo, item);
  }
  SelectCombo(g_levelCombo, 3);

  for (const wchar_t* item : {L"LZMA2", L"LZMA", L"PPMd", L"BZip2"}) {
    AddComboItem(g_methodCombo, item);
  }
  SelectCombo(g_methodCombo, 0);

  for (const wchar_t* item : {L"32m", L"64m", L"128m", L"256m", L"512m", L"1g"}) {
    AddComboItem(g_dictionaryCombo, item);
  }
  SelectCombo(g_dictionaryCombo, 0);

  for (const wchar_t* item : {L"32", L"64", L"128", L"273"}) {
    AddComboItem(g_wordCombo, item);
  }
  SelectCombo(g_wordCombo, 0);

  for (const wchar_t* item : {L"Solid", L"Non-solid", L"1g", L"4g", L"8g"}) {
    AddComboItem(g_solidCombo, item);
  }
  SelectCombo(g_solidCombo, 4);

  for (const wchar_t* item : {L"none", L"2g", L"4092M - FAT", L"4g", L"8g", L"16g"}) {
    AddComboItem(g_volumeCombo, item);
  }
  SelectCombo(g_volumeCombo, 2);

  for (const wchar_t* item : {L"on", L"2", L"4", L"8", L"16", L"20"}) {
    AddComboItem(g_threadsCombo, item);
  }
  SelectCombo(g_threadsCombo, 5);

  for (const wchar_t* item : {L"16m", L"64m", L"128m", L"256m"}) {
    AddComboItem(g_chunkCombo, item);
  }
  SelectCombo(g_chunkCombo, 1);

  AddComboItem(g_pathModeCombo, L"Relative paths");
  AddComboItem(g_pathModeCombo, L"Full paths");
  SelectCombo(g_pathModeCombo, 0);

  SetText(g_archiveNameEdit, L"MyPack");
  SendMessageW(g_copyInstallerCheck, BM_SETCHECK, BST_CHECKED, 0);
  SendMessageW(g_testArchiveCheck, BM_SETCHECK, BST_CHECKED, 0);
  EnableWindow(GetDlgItem(hwnd, kStopButton), FALSE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: {
      INITCOMMONCONTROLSEX controls{};
      controls.dwSize = sizeof(controls);
      controls.dwICC = ICC_PROGRESS_CLASS;
      InitCommonControlsEx(&controls);

      CreateGroupBox(hwnd, L"Folders", 10, 8, 704, 78);
      CreateLabel(hwnd, L"Source folder", 24, 30, 110, 20);
      g_sourceEdit = CreateEdit(hwnd, kSourceEdit, 140, 26, 520, 24);
      CreateButton(hwnd, kSourceBrowse, L"...", 668, 26, 34, 24);

      CreateLabel(hwnd, L"Release folder", 24, 60, 110, 20);
      g_releaseEdit = CreateEdit(hwnd, kReleaseEdit, 140, 56, 520, 24);
      CreateButton(hwnd, kReleaseBrowse, L"...", 668, 56, 34, 24);

      CreateGroupBox(hwnd, L"Archive", 10, 92, 704, 78);
      CreateLabel(hwnd, L"Archive", 24, 116, 110, 20);
      g_archiveNameEdit = CreateEdit(hwnd, kArchiveNameEdit, 140, 112, 190, 24);
      CreateLabel(hwnd, L"Archive format", 350, 116, 110, 20);
      g_formatCombo = CreateCombo(hwnd, kFormatCombo, 462, 112, 92, 160);
      CreateLabel(hwnd, L"Path mode", 24, 146, 110, 20);
      g_pathModeCombo = CreateCombo(hwnd, kPathModeCombo, 140, 142, 190, 160);

      CreateGroupBox(hwnd, L"Compression", 10, 176, 704, 172);
      CreateLabel(hwnd, L"Compression level", 24, 200, 130, 20);
      g_levelCombo = CreateCombo(hwnd, kLevelCombo, 164, 196, 110, 160);
      CreateLabel(hwnd, L"Compression method", 300, 200, 150, 20);
      g_methodCombo = CreateCombo(hwnd, kMethodCombo, 460, 196, 110, 160);

      CreateLabel(hwnd, L"Dictionary size", 24, 232, 130, 20);
      g_dictionaryCombo = CreateCombo(hwnd, kDictionaryCombo, 164, 228, 110, 160);
      CreateLabel(hwnd, L"Word size", 300, 232, 150, 20);
      g_wordCombo = CreateCombo(hwnd, kWordCombo, 460, 228, 110, 160);

      CreateLabel(hwnd, L"Solid block size", 24, 264, 130, 20);
      g_solidCombo = CreateCombo(hwnd, kSolidCombo, 164, 260, 110, 160);
      CreateLabel(hwnd, L"Split to volumes, bytes", 300, 264, 150, 20);
      g_volumeCombo = CreateCombo(hwnd, kVolumeCombo, 460, 260, 110, 160);

      CreateLabel(hwnd, L"Number of CPU threads", 24, 296, 140, 20);
      g_threadsCombo = CreateCombo(hwnd, kThreadsCombo, 164, 292, 110, 160);
      CreateLabel(hwnd, L"Parameters", 300, 296, 150, 20);
      g_parametersEdit = CreateEdit(hwnd, kParametersEdit, 460, 292, 210, 24);

      CreateGroupBox(hwnd, L"Manifest / Release", 10, 354, 704, 74);
      CreateLabel(hwnd, L"Manifest chunk size", 24, 380, 130, 20);
      g_chunkCombo = CreateCombo(hwnd, kChunkCombo, 164, 376, 110, 160);
      g_copyInstallerCheck = CreateWindowExW(0, L"BUTTON", L"Copy modlist-installer.exe into release folder",
                                             WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                             300, 376, 360, 24, hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCopyInstallerCheck)), g_instance, nullptr);
      g_testArchiveCheck = CreateWindowExW(0, L"BUTTON", L"Test archive after compression",
                                           WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                           300, 400, 260, 24, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTestArchiveCheck)), g_instance, nullptr);

      CreateButton(hwnd, kBuildButton, L"Build Package", 14, 438, 130, 30);
      CreateButton(hwnd, kManifestButton, L"Manifest Only", 154, 438, 130, 30);
      CreateButton(hwnd, kStopButton, L"Stop", 294, 438, 90, 30);

      g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                   400, 442, 300, 20, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgress)), g_instance, nullptr);
      g_statusLabel = CreateWindowExW(0, L"STATIC", L"Ready", WS_CHILD | WS_VISIBLE,
                                      14, 478, 690, 22, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabel)), g_instance, nullptr);
      g_logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                  14, 504, 690, 156, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLogEdit)), g_instance, nullptr);
      SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
      SetDefaults(hwnd);
      AppendLog(L"Modlist Packer ready.");
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == kSourceBrowse) {
        if (auto path = PickFolder(hwnd)) {
          SetText(g_sourceEdit, path->wstring());
        }
      } else if (id == kReleaseBrowse) {
        if (auto path = PickFolder(hwnd)) {
          SetText(g_releaseEdit, path->wstring());
        }
      } else if (id == kBuildButton) {
        StartWork(hwnd, true);
      } else if (id == kManifestButton) {
        StartWork(hwnd, false);
      } else if (id == kStopButton) {
        g_cancelRequested = true;
      }
      return 0;
    }
    case kLogMessage: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      AppendLog(*text);
      return 0;
    }
    case kStatusMessage: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      SetWindowTextW(g_statusLabel, text->c_str());
      return 0;
    }
    case kProgressMessage:
      SendMessageW(g_progress, PBM_SETPOS, static_cast<int>(wParam), 0);
      return 0;
    case kWorkerFinishedMessage:
      EnableRunningControls(hwnd, false);
      return 0;
    case WM_CLOSE:
      if (g_workerRunning) {
        g_cancelRequested = true;
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wParam, lParam);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  g_instance = instance;
  OleInitialize(nullptr);

  const wchar_t className[] = L"ModlistPackerWindow";
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = className;
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  RegisterClassW(&windowClass);

  HWND hwnd = CreateWindowExW(0, className, L"Modlist Packer",
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, 735, 710,
                              nullptr, nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    OleUninitialize();
    return 1;
  }

  ShowWindow(hwnd, showCommand);
  UpdateWindow(hwnd);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  OleUninitialize();
  return static_cast<int>(message.wParam);
}
