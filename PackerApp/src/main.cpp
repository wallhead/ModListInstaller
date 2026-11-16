#define NOMINMAX

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <winioctl.h>
#include <bcrypt.h>
#include <psapi.h>

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
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
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
constexpr int kMemoryLimitCombo = 1027;

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
HWND g_memoryLimitCombo = nullptr;
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
  int memoryLimitPercent{80};
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

bool ExtractEmbeddedTool(int resourceId, const wchar_t* name, std::wstring& error) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(resourceId), MAKEINTRESOURCEW(10));
  if (resource == nullptr) {
    error = L"Embedded 7-Zip resource was not found.";
    return false;
  }

  HGLOBAL loaded = LoadResource(module, resource);
  const DWORD size = SizeofResource(module, resource);
  const void* data = LockResource(loaded);
  if (loaded == nullptr || data == nullptr || size == 0) {
    error = L"Embedded 7-Zip resource could not be loaded.";
    return false;
  }

  const auto outputFolder = LocalToolCacheFolder();
  std::error_code ec;
  std::filesystem::create_directories(outputFolder, ec);
  if (ec) {
    error = L"Unable to create embedded 7-Zip cache folder: " + Widen(ec.message());
    return false;
  }

  const auto outputPath = outputFolder / name;
  bool writeFile = true;
  if (std::filesystem::exists(outputPath, ec) && !ec) {
    const auto existingSize = std::filesystem::file_size(outputPath, ec);
    writeFile = ec || existingSize != size;
    if (!writeFile) {
      std::ifstream input(outputPath, std::ios::binary);
      std::vector<char> existingBytes(size);
      input.read(existingBytes.data(), static_cast<std::streamsize>(existingBytes.size()));
      writeFile = !input ||
          !std::equal(existingBytes.begin(), existingBytes.end(), static_cast<const char*>(data)) ||
          input.peek() != std::char_traits<char>::eof();
    }
  }

  if (writeFile) {
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    if (!output) {
      error = L"Unable to write embedded 7-Zip component: " + outputPath.wstring();
      return false;
    }
    output.write(static_cast<const char*>(data), size);
    output.close();
    if (!output) {
      error = L"Unable to finish writing embedded 7-Zip component: " + outputPath.wstring();
      return false;
    }
  }

  return true;
}

std::optional<std::filesystem::path> ExtractEmbeddedSevenZip(std::wstring& error) {
  if (!ExtractEmbeddedTool(IDR_7ZIP_EXE, L"7z.exe", error) ||
      !ExtractEmbeddedTool(IDR_7ZIP_DLL, L"7z.dll", error) ||
      !ExtractEmbeddedTool(IDR_7ZIP_LICENSE, L"7zip-License.txt", error)) {
    return std::nullopt;
  }
  return LocalToolCacheFolder() / "7z.exe";
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
                                  int64_t elapsedSeconds,
                                  uint64_t memoryUsedBytes = 0,
                                  uint64_t memoryLimitBytes = 0) {
  std::wostringstream out;
  out << label << L" " << percent << L"%";
  if (totalBytes > 0) {
    out << L" | " << FormatBytes(doneBytes) << L" / " << FormatBytes(totalBytes);
  }
  if (bytesPerSecond > 0) {
    out << L" | " << FormatBytesPerSecond(bytesPerSecond);
  }
  if (memoryUsedBytes > 0) {
    out << L" | RAM " << FormatBytes(memoryUsedBytes);
    if (memoryLimitBytes > 0) {
      out << L" / " << FormatBytes(memoryLimitBytes);
    }
  }
  out << L" | ETA " << FormatDuration(etaSeconds)
      << L" | Elapsed " << FormatDuration(elapsedSeconds);
  return out.str();
}

uint64_t PhysicalMemoryLimitBytes(int percent) {
  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (!GlobalMemoryStatusEx(&memory)) {
    return 0;
  }
  const auto boundedPercent = static_cast<uint64_t>(std::clamp(percent, 1, 100));
  return (memory.ullTotalPhys / 100) * boundedPercent;
}

int ParsePercentText(const std::wstring& text, int fallback) {
  try {
    size_t parsed = 0;
    const int value = std::stoi(text, &parsed);
    if (parsed > 0) {
      return std::clamp(value, 1, 100);
    }
  } catch (const std::exception&) {
  }
  return fallback;
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
  auto payload = std::make_unique<std::wstring>(std::move(text));
  if (hwnd != nullptr && PostMessageW(hwnd, kLogMessage, 0, reinterpret_cast<LPARAM>(payload.get()))) {
    payload.release();
  }
}

void PostStatus(HWND hwnd, std::wstring text) {
  auto payload = std::make_unique<std::wstring>(std::move(text));
  if (hwnd != nullptr && PostMessageW(hwnd, kStatusMessage, 0, reinterpret_cast<LPARAM>(payload.get()))) {
    payload.release();
  }
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

uint64_t EstimateFolderBytes(HWND hwnd, const std::filesystem::path& folder) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
    return 0;
  }

  uint64_t total = 0;
  uint64_t fileCount = 0;
  const auto startedAt = std::chrono::steady_clock::now();
  auto lastStatusAt = startedAt - std::chrono::seconds(1);
  PostProgress(hwnd, 0);
  PostStatus(hwnd, L"Scanning source files | 0 files | 0 B | Elapsed 0s");

  auto iterator = std::filesystem::recursive_directory_iterator(
      folder, std::filesystem::directory_options::skip_permission_denied, ec);
  const auto end = std::filesystem::recursive_directory_iterator();
  while (!g_cancelRequested && iterator != end) {
    const auto entry = *iterator;
    if (entry.is_regular_file(ec) && !ec) {
      const auto size = entry.file_size(ec);
      if (!ec) {
        total += size;
        ++fileCount;
      }
    }
    if (ec) {
      ec.clear();
    }
    iterator.increment(ec);
    if (ec) {
      ec.clear();
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastStatusAt >= std::chrono::seconds(1)) {
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startedAt).count();
      PostStatus(hwnd, L"Scanning source files | " + std::to_wstring(fileCount) +
                           L" files | " + FormatBytes(total) +
                           L" | Elapsed " + FormatDuration(elapsed));
      lastStatusAt = now;
    }
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - startedAt).count();
  PostStatus(hwnd, L"Source scan complete | " + std::to_wstring(fileCount) +
                       L" files | " + FormatBytes(total) +
                       L" | Elapsed " + FormatDuration(elapsed));
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

struct SevenZipOptions {
  bool fullPaths{false};
  bool splitVolumes{false};
};

SevenZipOptions ParseSevenZipOptions(std::wstring_view parameters) {
  SevenZipOptions options;
  // 7-Zip 26.02 toggles quoting on each double quote; backslashes are literal.
  std::wstring argument;
  while (!parameters.empty()) {
    argument.clear();
    bool quoted = false;
    while (!parameters.empty()) {
      const wchar_t ch = parameters.front();
      parameters.remove_prefix(1);
      if (ch == L'"') quoted = !quoted;
      else if (!quoted && (ch == L' ' || ch == L'\t')) break;
      else argument += ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
    }
    if (argument == L"--") break;
    options.fullPaths |= argument.starts_with(L"-spf") || argument.starts_with(L"/spf");
    options.splitVolumes |= argument.size() > 2 && argument.starts_with(L"-v");
  }
  return options;
}

bool UsesConfiguredVolumes(const PackerConfig& config) {
  return !config.volumeSize.empty() && config.volumeSize != L"none";
}

bool SevenZipRequestsSplit(const PackerConfig& config) {
  return UsesConfiguredVolumes(config) || ParseSevenZipOptions(config.parameters).splitVolumes;
}

std::filesystem::path PackageFolder(const PackerConfig& config) {
  return config.releaseFolder / config.archiveName;
}

std::filesystem::path ArchivePath(const PackerConfig& config) {
  return PackageFolder(config) / (config.archiveName + SelectArchiveExtension(config));
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
  if (std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 0x20; })) {
    return false;
  }
  auto stem = name.substr(0, name.find(L'.'));
  for (auto& ch : stem) {
    ch = static_cast<wchar_t>(std::towupper(ch));
  }
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
      (stem.size() == 4 && (stem.starts_with(L"COM") || stem.starts_with(L"LPT")) &&
       stem[3] >= L'1' && stem[3] <= L'9')) {
    return false;
  }
  const std::filesystem::path path(name);
  return !path.is_absolute() && path.filename().wstring() == name;
}

std::wstring NormalizedPathText(const std::filesystem::path& path) {
  std::error_code ec;
  auto normalized = std::filesystem::weakly_canonical(path, ec);
  if (ec) {
    ec.clear();
    normalized = std::filesystem::absolute(path, ec).lexically_normal();
  }
  if (ec) {
    normalized = path.lexically_normal();
  }
  auto text = normalized.wstring();
  for (auto& ch : text) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  while (!text.empty() && (text.back() == L'\\' || text.back() == L'/')) {
    text.pop_back();
  }
  return text;
}

bool IsSameOrChildPath(const std::filesystem::path& candidate, const std::filesystem::path& parent) {
  const auto candidateText = NormalizedPathText(candidate);
  const auto parentText = NormalizedPathText(parent);
  return candidateText == parentText ||
         (candidateText.size() > parentText.size() &&
          candidateText.starts_with(parentText) &&
          (candidateText[parentText.size()] == L'\\' || candidateText[parentText.size()] == L'/'));
}

bool PathsOverlap(const std::filesystem::path& left, const std::filesystem::path& right) {
  return IsSameOrChildPath(left, right) || IsSameOrChildPath(right, left);
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
  if (UsesConfiguredVolumes(config)) {
    command += L" -v" + SevenZipVolumeSize(config.volumeSize);
  }
  if (!config.parameters.empty()) {
    command += L" " + config.parameters;
  }
  return command;
}

std::wstring BuildSevenZipTestCommand(const PackerConfig& config) {
  auto archive = ArchivePath(config);
  if (SevenZipRequestsSplit(config)) {
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
    const auto lowerName = Lower(name);
    const auto lowerBase = Lower(base);
    const auto volumePrefix = lowerBase + L".";
    if (lowerName == lowerBase ||
        (lowerName.starts_with(volumePrefix) && IsNumericVolumeSuffix(lowerName.substr(volumePrefix.size())))) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

std::vector<std::filesystem::path> FindArchiveOutputsAndTemps(const std::filesystem::path& folder,
                                                              const std::wstring& base) {
  auto files = FindArchivePartsForBase(folder, base);
  std::error_code ec;
  const auto lowerBase = Lower(base);
  const auto tempPrefix = lowerBase + L".";
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const auto lowerName = Lower(entry.path().filename().wstring());
    const bool selectedArchiveTemp = lowerName == lowerBase + L".tmp" ||
        (lowerName.starts_with(tempPrefix) && lowerName.ends_with(L".tmp") &&
         IsNumericVolumeSuffix(lowerName.substr(tempPrefix.size(), lowerName.size() - tempPrefix.size() - 4)));
    if (selectedArchiveTemp &&
        std::find(files.begin(), files.end(), entry.path()) == files.end()) {
      files.push_back(entry.path());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

bool RemovePreviousPackageOutputs(HWND hwnd, const PackerConfig& config) {
  std::error_code ec;
  for (const auto* extension : {L".7z", L".zip"}) {
    const auto base = config.archiveName + extension;
    for (const auto& file : FindArchiveOutputsAndTemps(PackageFolder(config), base)) {
      std::filesystem::remove(file, ec);
      if (ec) {
        PostLog(hwnd, L"Unable to remove previous archive part: " + file.wstring() + L" - " + Widen(ec.message()));
        return false;
      }
      PostLog(hwnd, L"Removed previous archive part: " + file.wstring());
    }
  }

  const auto manifestPath = PackageFolder(config) / "manifest.json";
  const bool manifestExists = std::filesystem::exists(manifestPath, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to inspect previous manifest: " + Widen(ec.message()));
    return false;
  }
  if (manifestExists) {
    if (!std::filesystem::is_regular_file(manifestPath, ec) || ec) {
      PostLog(hwnd, L"Previous manifest is not a regular file: " + manifestPath.wstring());
      return false;
    }
    std::filesystem::remove(manifestPath, ec);
    if (ec) {
      PostLog(hwnd, L"Unable to remove previous manifest: " + Widen(ec.message()));
      return false;
    }
    PostLog(hwnd, L"Removed previous manifest: " + manifestPath.wstring());
  }
  return true;
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
    set.files = FindArchivePartsForBase(PackageFolder(config), set.baseName);
    return set;
  }

  auto sets = FindArchiveSets(PackageFolder(config));
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

using ManifestCloser = std::function<void(std::ofstream&)>;

void CloseManifest(std::ofstream& output) {
  output.close();
}

constexpr std::wstring_view kWorkNamePattern = L".packer-0000000000000000";

std::wstring UniqueWorkName() {
  unsigned char nonce[8]{};
  if (BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    throw std::runtime_error("Unable to generate a package work name");
  }
  std::wstring name(kWorkNamePattern);
  constexpr wchar_t hex[] = L"0123456789abcdef";
  auto digit = name.end() - 2 * sizeof(nonce);
  for (auto byte : nonce) {
    *digit++ = hex[byte >> 4];
    *digit++ = hex[byte & 15];
  }
  return name;
}

bool CheckPackagePath(HWND hwnd, const std::filesystem::path& path, bool directory = false) {
  const auto absolute = std::filesystem::absolute(path).make_preferred().wstring();
  const size_t limit = directory ? MAX_PATH - 12 : MAX_PATH - 1;
  if (absolute.size() > limit || absolute.starts_with(L"\\\\?\\") || absolute.starts_with(L"\\\\.\\")) {
    PostLog(hwnd, L"Package path is too long or uses an unsupported path prefix. Shorten the release folder or archive name: " +
                     absolute);
    return false;
  }
  return true;
}

bool WriteManifest(HWND hwnd, const PackerConfig& config, uint64_t unpackedSize,
                   const ManifestCloser& closeManifest = CloseManifest,
                   const std::filesystem::path& outputFolder = {}) {
  const auto archiveSet = SelectArchiveSet(hwnd, config);
  if (!archiveSet.has_value()) {
    PostLog(hwnd, L"No archive parts found in: " + PackageFolder(config).wstring());
    return false;
  }

  const auto& files = archiveSet->files;
  if (files.empty()) {
    PostLog(hwnd, L"No archive parts found in: " + PackageFolder(config).wstring());
    return false;
  }

  const auto packageFolder = PackageFolder(config);
  const auto manifestFolder = outputFolder.empty() ? packageFolder : outputFolder;
  const auto manifestPath = manifestFolder / "manifest.json";
  const auto temporaryPath = manifestFolder / UniqueWorkName();
  if (!CheckPackagePath(hwnd, manifestFolder, true) || !CheckPackagePath(hwnd, manifestPath) ||
      !CheckPackagePath(hwnd, temporaryPath)) return false;
  for (const auto& file : files) {
    if (!CheckPackagePath(hwnd, file)) return false;
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
  const size_t workerCount = SelectHashWorkerCount(packageFolder, files.size());
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
                           packageFolder,
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
  std::filesystem::create_directories(manifestFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to create package folder: " + Widen(ec.message()));
    return false;
  }

  HANDLE temporary = CreateFileW(temporaryPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (temporary == INVALID_HANDLE_VALUE) {
    PostLog(hwnd, L"Unable to create temporary manifest: " + temporaryPath.wstring());
    return false;
  }
  CloseHandle(temporary);
  struct TemporaryManifest {
    std::filesystem::path path;
    ~TemporaryManifest() { std::error_code ignored; std::filesystem::remove(path, ignored); }
  } cleanup{temporaryPath};
  std::ofstream output(temporaryPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    PostLog(hwnd, L"Unable to write manifest: " + manifestPath.wstring());
    return false;
  }

  output << "{\n";
  output << "  \"schema\": \"modlist-manifest-chunks-v1\",\n";
  output << "  \"archive_name\": \"" << JsonEscape(Narrow(archiveSet->archiveName)) << "\",\n";
  if (unpackedSize > 0) {
    output << "  \"unpacked_size\": " << unpackedSize << ",\n";
  }
  output << "  \"hash\": {\n";
  output << "    \"algorithm\": \"sha256\",\n";
  output << "    \"chunk_size\": " << config.chunkSize << "\n";
  output << "  },\n";
  output << "  \"files\": [\n";
  for (size_t i = 0; i < manifests.size(); ++i) {
    const auto& file = manifests[i];
    output << "    {\n";
    output << "      \"path\": \"" << JsonEscape(Narrow(file.path.generic_wstring())) << "\",\n";
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
  closeManifest(output);
  if (!output) {
    PostLog(hwnd, L"Unable to finish manifest write.");
    return false;
  }
  if (g_cancelRequested || !MoveFileExW(temporaryPath.c_str(), manifestPath.c_str(),
                                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    PostLog(hwnd, L"Unable to publish completed manifest (or operation cancelled).");
    return false;
  }

  PostProgress(hwnd, 100);
  PostStatus(hwnd, L"Manifest complete");
  PostLog(hwnd, L"Manifest written: " + manifestPath.wstring());
  return true;
}

void UpdateProcessProgressStatus(HWND hwnd,
                                 const std::wstring& label,
                                 int sevenZipPercent,
                                 uint64_t estimatedBytes,
                                 std::chrono::steady_clock::time_point startedAt,
                                 HANDLE processHandle,
                                 uint64_t memoryLimitBytes) {
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - startedAt).count();
  uint64_t done = 0;
  uint64_t speed = 0;
  int64_t eta = -1;

  IO_COUNTERS io{};
  const bool hasIoCounters = processHandle != nullptr && GetProcessIoCounters(processHandle, &io);
  if (hasIoCounters) {
    done = estimatedBytes > 0
        ? std::min<uint64_t>(estimatedBytes, io.ReadTransferCount)
        : io.ReadTransferCount;
    if (elapsed > 0 && io.ReadTransferCount > 0) {
      speed = io.ReadTransferCount / static_cast<uint64_t>(elapsed);
    }
    if (estimatedBytes > done && speed > 0) {
      eta = static_cast<int64_t>((estimatedBytes - done) / speed);
    }
  }

  int percent = std::clamp(sevenZipPercent, 0, 100);
  if (sevenZipPercent < 0 && estimatedBytes > 0 && done > 0) {
    percent = static_cast<int>(std::min<long double>(
        100.0L, (static_cast<long double>(done) * 100.0L) / static_cast<long double>(estimatedBytes)));
  }

  uint64_t memoryUsedBytes = 0;
  PROCESS_MEMORY_COUNTERS_EX memory{};
  if (processHandle != nullptr &&
      GetProcessMemoryInfo(processHandle,
                           reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
                           sizeof(memory))) {
    memoryUsedBytes = static_cast<uint64_t>(memory.PrivateUsage);
  }

  PostProgress(hwnd, percent);
  PostStatus(hwnd, FormatProgressStatus(label,
                                        percent,
                                        done,
                                        estimatedBytes,
                                        speed,
                                        eta,
                                        elapsed,
                                        memoryUsedBytes,
                                        memoryLimitBytes));
}

class SevenZipProgress {
 public:
  void Consume(std::string_view bytes, int& lastPercent) noexcept {
    for (char byte : bytes) {
      bool erased = false;
      if (erasePercent_ >= 0) {
        if (byte == ' ' && eraseSpaces_ < eraseWidth_) {
          ++eraseSpaces_;
        } else {
          erased = byte == '\r' && eraseSpaces_ == eraseWidth_;
          if (erased) lastPercent = erasePercent_;
          erasePercent_ = -1;
        }
      }
      if (byte == '\r' || byte == '\n' || byte == '\b') {
        if (byte == '\r' && candidate_ >= 0) {
          // Windows 7-Zip clears a frame with CR, one space per byte, then CR.
          erasePercent_ = candidate_;
          eraseWidth_ = length_;
          eraseSpaces_ = 0;
        }
        if (byte == '\b' && candidate_ >= 0) lastPercent = candidate_;
        length_ = 0;
        candidate_ = -1;
        frameStart_ = erased;
      } else if (length_ < kPrefixWidth) {
        frame_[length_++] = byte;
        if (length_ == kPrefixWidth) candidate_ = ParsePrefix();
      } else if (candidate_ >= 0) {
        if ((length_ == kPrefixWidth && byte != ' ') || length_ == sizeof(frame_)) candidate_ = -1;
        else frame_[length_++] = byte;
      }
    }
  }

  int Current(int lastPercent) const noexcept {
    // A bare prefix on an ordinary log line is ambiguous until 7-Zip erases it.
    return frameStart_ && candidate_ >= 0 && HasProgressSuffix() ? candidate_ : lastPercent;
  }

 private:
  int ParsePrefix() const noexcept {
    if (frame_[3] != '%') return -1;
    size_t i = 0;
    while (i < 2 && frame_[i] == ' ') ++i;
    if (i < 2 && frame_[i] == '0') return -1;
    int percent = 0;
    for (; i < 3; ++i) {
      if (frame_[i] < '0' || frame_[i] > '9') return -1;
      percent = percent * 10 + frame_[i] - '0';
    }
    return percent <= 100 ? percent : -1;
  }

  bool HasProgressSuffix() const noexcept {
    std::string_view suffix(frame_ + kPrefixWidth, length_ - kPrefixWidth);
    auto skipSpaces = [&] { while (!suffix.empty() && suffix.front() == ' ') suffix.remove_prefix(1); };
    skipSpaces();
    const auto count = suffix.find_first_not_of("0123456789");
    if (count != 0 && !suffix.empty()) {
      if (count == std::string_view::npos) return suffix.size() <= 20;
      if (count > 20 || suffix[count] != ' ') return false;
      suffix.remove_prefix(count);
      skipSpaces();
    }
    if (suffix.empty()) return true;
    const auto command = suffix.substr(0, suffix.find(' '));
    // Once a known operation starts, its remaining text, including names, is opaque.
    for (std::string_view known : {"+", "U", "A", "R", "D", ".", "=", "Scan", "Removing",
                                   "Anti", "Reading", "Header", "Size", "Open", ":", "T", "E", "X"}) {
      if (command == known) return true;
    }
    return false;
  }

  static constexpr size_t kPrefixWidth = 4;
  char frame_[1024]{};
  size_t length_{0};
  int candidate_{-1};
  int erasePercent_{-1};
  size_t eraseWidth_{0};
  size_t eraseSpaces_{0};
  bool frameStart_{false};
};

struct CloseWinHandle {
  void operator()(HANDLE handle) const noexcept { CloseHandle(handle); }
};
using ScopedWinHandle = std::unique_ptr<void, CloseWinHandle>;

class RunningChild {
 public:
  explicit RunningChild(HANDLE process) noexcept : process_(process) {}
  ~RunningChild() {
    if (WaitForSingleObject(Get(), 0) != WAIT_OBJECT_0) {
      TerminateProcess(Get(), 255);
      WaitForSingleObject(Get(), INFINITE);
    }
  }
  HANDLE Get() const noexcept { return process_.get(); }

 private:
  ScopedWinHandle process_;
};

int RunProcess(HWND hwnd,
               const std::wstring& command,
               const std::wstring& label,
               uint64_t estimatedBytes,
               int memoryLimitPercent = 0) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE rawRead = nullptr;
  HANDLE rawWrite = nullptr;
  if (!CreatePipe(&rawRead, &rawWrite, &security, 0)) {
    return static_cast<int>(GetLastError());
  }
  const ScopedWinHandle readPipe(rawRead);
  ScopedWinHandle writePipe(rawWrite);
  if (!SetHandleInformation(readPipe.get(), HANDLE_FLAG_INHERIT, 0)) {
    return static_cast<int>(GetLastError());
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = writePipe.get();
  startup.hStdError = writePipe.get();
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  ScopedWinHandle job;
  uint64_t memoryLimitBytes = 0;
  if (memoryLimitPercent > 0) {
    memoryLimitBytes = PhysicalMemoryLimitBytes(memoryLimitPercent);
    if (memoryLimitBytes == 0) {
      return static_cast<int>(ERROR_NOT_ENOUGH_MEMORY);
    }
    job.reset(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
      return static_cast<int>(GetLastError());
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limits.ProcessMemoryLimit = static_cast<SIZE_T>(memoryLimitBytes);
    if (!SetInformationJobObject(job.get(),
                                 JobObjectExtendedLimitInformation,
                                 &limits,
                                 sizeof(limits))) {
      return static_cast<int>(GetLastError());
    }
    PostLog(hwnd, L"7-Zip RAM limit: " + std::to_wstring(memoryLimitPercent) +
                      L"% of physical memory (" + FormatBytes(memoryLimitBytes) + L")");
  }

  PROCESS_INFORMATION process{};
  std::wstring mutableCommand = command;
  const BOOL created = CreateProcessW(nullptr,
                                      mutableCommand.data(),
                                      nullptr,
                                      nullptr,
                                      TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                      nullptr,
                                      nullptr,
                                      &startup,
                                      &process);
  if (!created) {
    return static_cast<int>(GetLastError());
  }
  RunningChild child(process.hProcess);
  const ScopedWinHandle thread(process.hThread);
  writePipe.reset();

  if (job && !AssignProcessToJobObject(job.get(), child.Get())) {
    return static_cast<int>(GetLastError());
  }
  if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
    return static_cast<int>(GetLastError());
  }

  char buffer[4096];
  SevenZipProgress progress;
  int lastPercent = -1;
  int reportedPercent = -1;
  const auto startedAt = std::chrono::steady_clock::now();
  auto lastHeartbeatAt = startedAt;
  UpdateProcessProgressStatus(hwnd, label, lastPercent, estimatedBytes, startedAt, child.Get(), memoryLimitBytes);

  while (true) {
    if (g_cancelRequested.load()) {
      TerminateProcess(child.Get(), 255);
      break;
    }

    DWORD available = 0;
    while (PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &available, nullptr) && available > 0) {
      DWORD bytesRead = 0;
      const DWORD toRead = std::min<DWORD>(available, sizeof(buffer));
      if (!ReadFile(readPipe.get(), buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
        break;
      }
      progress.Consume({buffer, bytesRead}, lastPercent);
      available = 0;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastHeartbeatAt >= std::chrono::seconds(1)) {
      reportedPercent = progress.Current(lastPercent);
      UpdateProcessProgressStatus(hwnd,
                                  label,
                                  reportedPercent,
                                  estimatedBytes,
                                  startedAt,
                                  child.Get(),
                                  memoryLimitBytes);
      lastHeartbeatAt = now;
    }

    const DWORD wait = WaitForSingleObject(child.Get(), 100);
    if (wait == WAIT_FAILED) return static_cast<int>(GetLastError());
    if (wait == WAIT_OBJECT_0) {
      DWORD remaining = 0;
      while (PeekNamedPipe(readPipe.get(), nullptr, 0, nullptr, &remaining, nullptr) && remaining > 0) {
        DWORD bytesRead = 0;
        const DWORD toRead = std::min<DWORD>(remaining, sizeof(buffer));
        if (!ReadFile(readPipe.get(), buffer, toRead, &bytesRead, nullptr) || bytesRead == 0) {
          break;
        }
        progress.Consume({buffer, bytesRead}, lastPercent);
      }
      break;
    }
  }

  if (WaitForSingleObject(child.Get(), INFINITE) == WAIT_FAILED) return static_cast<int>(GetLastError());
  DWORD exitCode = 0;
  if (!GetExitCodeProcess(child.Get(), &exitCode)) return static_cast<int>(GetLastError());
  if (exitCode == 0 && !g_cancelRequested && lastPercent >= 0 && lastPercent != reportedPercent) {
    UpdateProcessProgressStatus(hwnd, label, lastPercent, estimatedBytes, startedAt, child.Get(), memoryLimitBytes);
  }
  return static_cast<int>(exitCode);
}

bool PrepareInstallerSetup(HWND hwnd, const PackerConfig& config,
                           const std::filesystem::path& installerSourceFolder = ModuleFolder()) {
  std::error_code ec;
  const auto packageFolder = PackageFolder(config);
  std::filesystem::create_directories(packageFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to create package folder: " + packageFolder.wstring() + L" - " + Widen(ec.message()));
    return false;
  }
  if (!config.copyInstaller) {
    return true;
  }

  const auto source = installerSourceFolder / "modlist-installer.exe";
  const auto target = packageFolder / "modlist-installer.exe";
  if (!std::filesystem::is_regular_file(source, ec) || ec) {
    PostLog(hwnd, L"Unable to prepare setup: modlist-installer.exe was not found beside modlist-packer.exe.");
    return false;
  }
  if (std::filesystem::exists(target, ec) && !ec && std::filesystem::equivalent(source, target, ec) && !ec) {
    return true;
  }
  ec.clear();
  std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to copy installer: " + Widen(ec.message()));
    return false;
  }
  PostLog(hwnd, L"Installer copied: " + target.wstring());
  return true;
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
  config.memoryLimitPercent = ParsePercentText(ComboText(g_memoryLimitCombo), 80);
  config.parameters = GetText(g_parametersEdit);
  config.pathMode = ComboText(g_pathModeCombo);
  config.chunkSize = ParseSizeText(ComboText(g_chunkCombo), 64ull * 1024ull * 1024ull);
  config.copyInstaller = true;
  config.testArchive = SendMessageW(g_testArchiveCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
  config.archiveFirst = archiveFirst;
  return config;
}

bool ValidateConfig(HWND hwnd, const PackerConfig& config, bool createFolders = true) {
  std::error_code ec;
  if (config.releaseFolder.empty()) {
    PostLog(hwnd, L"Release folder is required.");
    return false;
  }
  if (config.archiveName.empty()) {
    PostLog(hwnd, L"Archive name is required.");
    return false;
  }
  if (!IsSafeArchiveName(config.archiveName)) {
    PostLog(hwnd, L"Archive name cannot contain path separators, reserved/control characters, device names, or parent-directory segments.");
    return false;
  }
  if (!config.pathMode.empty() && config.pathMode != L"Relative paths") {
    PostLog(hwnd, L"Only Relative paths is supported; absolute archive entries cannot be installed safely.");
    return false;
  }
  if (ParseSevenZipOptions(config.parameters).fullPaths) {
    PostLog(hwnd, L"The -spf full-path switches are not supported; use Relative paths.");
    return false;
  }
  if (config.archiveFirst) {
    if (!std::filesystem::exists(config.sourceFolder, ec) || !std::filesystem::is_directory(config.sourceFolder, ec)) {
      PostLog(hwnd, L"Source folder is required.");
      return false;
    }
    if (PathsOverlap(config.sourceFolder, config.releaseFolder) ||
        PathsOverlap(config.sourceFolder, PackageFolder(config))) {
      PostLog(hwnd, L"Source and release folders must not be the same or contain one another.");
      return false;
    }
  }
  if (!createFolders) return true;
  std::filesystem::create_directories(config.releaseFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to create release folder: " + Widen(ec.message()));
    return false;
  }
  std::filesystem::create_directories(PackageFolder(config), ec);
  if (ec) {
    PostLog(hwnd, L"Unable to create archive-named package folder: " + Widen(ec.message()));
    return false;
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

using FileRenamer = std::function<void(const std::filesystem::path&, const std::filesystem::path&,
                                      std::error_code&)>;

void RenamePackageFile(const std::filesystem::path& from, const std::filesystem::path& to,
                       std::error_code& ec) {
  ec.clear();
  if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) {
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
  }
}

struct WorkerDependencies {
  std::filesystem::path installerSourceFolder{ModuleFolder()};
  ManifestCloser closeManifest{CloseManifest};
  FileRenamer renameFile{RenamePackageFile};
};

class PackageWorkspace {
 public:
  PackageWorkspace(HWND hwnd, const std::filesystem::path& releaseFolder) : hwnd_(hwnd) {
    root_ = std::filesystem::canonical(releaseFolder) / UniqueWorkName();
    if (!std::filesystem::create_directory(root_)) throw std::runtime_error("Unable to reserve package workspace");
    markerText_ = "ModlistPacker owned workspace\n" + Narrow(root_.wstring()) + '\n';
    std::ofstream marker(root_ / "packer-workspace.txt", std::ios::binary);
    marker << markerText_;
    marker.close();
    if (!marker) throw std::runtime_error("Unable to mark package workspace: " + Narrow(root_.wstring()));
  }

  ~PackageWorkspace() {
    if (retained_) return;
    try {
      if (std::filesystem::canonical(root_) != root_) throw std::runtime_error("Workspace identity changed");
      auto check = [](const std::filesystem::path& path) {
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
          throw std::runtime_error("Workspace contains a redirected or inaccessible path");
        }
      };
      check(root_);
      check(root_ / "packer-workspace.txt");
      std::ifstream marker(root_ / "packer-workspace.txt", std::ios::binary);
      if (!marker || std::string(std::istreambuf_iterator<char>(marker), {}) != markerText_) {
        throw std::runtime_error("Workspace ownership marker changed");
      }
      marker.close();
      for (const auto& entry : std::filesystem::recursive_directory_iterator(root_)) check(entry.path());
      std::filesystem::remove_all(root_);
    } catch (const std::exception& error) {
      PostLog(hwnd_, L"Package workspace retained: " + root_.wstring() + L" - " + Widen(error.what()));
    }
  }

  const std::filesystem::path& Root() const { return root_; }
  void Retain() {
    retained_ = true;
    PostLog(hwnd_, L"RECOVERY REQUIRED. Previous files and promotion journal retained at: " + root_.wstring());
  }

 private:
  HWND hwnd_;
  std::filesystem::path root_;
  std::string markerText_;
  bool retained_{false};
};

constexpr auto kStagedDirectory = L"new";

bool CheckLivePackageDirectory(HWND hwnd, const std::filesystem::path& target) {
  const DWORD attributes = GetFileAttributesW(target.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return true;
  } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
    return true;
  }
  PostLog(hwnd, L"Live package folder is redirected, inaccessible, or not a directory: " + target.wstring());
  return false;
}

bool CheckWorkerPackagePaths(HWND hwnd, const PackerConfig& live) {
  const auto target = PackageFolder(live);
  if (!CheckPackagePath(hwnd, live.releaseFolder, true) || !CheckPackagePath(hwnd, target, true)) return false;
  if (!CheckLivePackageDirectory(hwnd, target)) return false;
  const auto root = std::filesystem::weakly_canonical(live.releaseFolder) / kWorkNamePattern;
  const auto stage = root / kStagedDirectory;
  const auto replacement = stage / live.archiveName;
  const auto backup = root / "previous";
  for (const auto& directory : {root, stage, replacement, backup}) {
    if (!CheckPackagePath(hwnd, directory, true)) return false;
  }
  for (const auto& file : {root / "packer-workspace.txt", root / "promotion-journal.txt", replacement / kWorkNamePattern}) {
    if (!CheckPackagePath(hwnd, file)) return false;
  }
  for (const auto& directory : {target, replacement, backup}) {
    if (!CheckPackagePath(hwnd, directory / "manifest.json") ||
        (live.copyInstaller && !CheckPackagePath(hwnd, directory / "modlist-installer.exe"))) return false;
  }
  if (live.archiveFirst) {
    const auto archive = live.archiveName + SelectArchiveExtension(live);
    if (SevenZipRequestsSplit(live)) {
      // A 32-bit volume number and 7-Zip's optional collision suffix (.8 hex digits.tmp).
      if (!CheckPackagePath(hwnd, target / (archive + L".4294967295")) ||
          !CheckPackagePath(hwnd, replacement / (archive + L".4294967295.ffffffff.tmp"))) {
        PostLog(hwnd, L"Split volumes require more room in the package path.");
        return false;
      }
    } else if (!CheckPackagePath(hwnd, target / archive) ||
               !CheckPackagePath(hwnd, replacement / (archive + L".tmp4294967295"))) {
      return false;
    }
    for (const auto* extension : {L".7z", L".zip"}) {
      for (const auto& file : FindArchiveOutputsAndTemps(target, live.archiveName + extension)) {
        if (!CheckPackagePath(hwnd, file) || !CheckPackagePath(hwnd, backup / file.filename())) return false;
      }
    }
  }
  return true;
}

bool IsPlainPackageFile(const std::filesystem::path& path) {
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
      (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

bool SameWindowsPackagePath(const std::filesystem::path& left, const std::filesystem::path& right) {
  const auto a = std::filesystem::absolute(left).lexically_normal().make_preferred().wstring();
  const auto b = std::filesystem::absolute(right).lexically_normal().make_preferred().wstring();
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool PromotePackage(HWND hwnd, const PackerConfig& live, const PackerConfig& staged,
                    PackageWorkspace& workspace, const FileRenamer& renameFile) {
  const auto target = PackageFolder(live);
  if (!CheckLivePackageDirectory(hwnd, target)) return false;
  const auto backup = workspace.Root() / "previous";
  std::filesystem::create_directory(backup);
  struct Move {
    std::filesystem::path from;
    std::filesystem::path to;
    bool done{false};
  };
  std::vector<Move> oldFiles;
  std::vector<Move> newFiles;
  auto addOld = [&](const std::filesystem::path& file) {
    if (!IsPlainPackageFile(file)) throw std::runtime_error("Previous package output is not a plain file");
    if (std::none_of(oldFiles.begin(), oldFiles.end(), [&](const Move& move) { return SameWindowsPackagePath(move.from, file); })) {
      oldFiles.push_back({file, backup / file.filename()});
    }
  };
  if (live.archiveFirst) {
    for (const auto* extension : {L".7z", L".zip"}) {
      for (const auto& file : FindArchiveOutputsAndTemps(target, live.archiveName + extension)) addOld(file);
    }
  }
  const auto manifest = target / "manifest.json";
  if (std::filesystem::exists(manifest)) addOld(manifest);
  for (const auto& entry : std::filesystem::directory_iterator(PackageFolder(staged))) {
    if (!IsPlainPackageFile(entry.path())) throw std::runtime_error("Staged output is not a plain file");
    if (!live.archiveFirst && entry.path().filename() != "manifest.json" &&
        entry.path().filename() != "modlist-installer.exe") {
      throw std::runtime_error("Manifest Only cannot promote archive or unrelated files");
    }
    const auto destination = target / entry.path().filename();
    if (std::filesystem::exists(destination)) {
      if (entry.path().filename() == "modlist-installer.exe") addOld(destination);
      else if (std::none_of(oldFiles.begin(), oldFiles.end(), [&](const Move& move) { return SameWindowsPackagePath(move.from, destination); })) {
        throw std::runtime_error("Publication would overwrite an unrelated output");
      }
    }
    newFiles.push_back({entry.path(), destination});
  }
  // Publish the manifest last so incomplete promotion cannot advertise a ready replacement.
  std::sort(newFiles.begin(), newFiles.end(), [](const Move& a, const Move& b) {
    if ((a.to.filename() == "manifest.json") != (b.to.filename() == "manifest.json")) {
      return b.to.filename() == "manifest.json";
    }
    return a.to < b.to;
  });
  std::ofstream journal(workspace.Root() / "promotion-journal.txt", std::ios::binary);
  journal << "Package: " << Narrow(target.wstring()) << "\nRestore previous/ files to the package if rollback was interrupted.\n";
  for (const auto& move : oldFiles) journal << "OLD " << Narrow(move.from.wstring()) << " -> " << Narrow(move.to.wstring()) << '\n';
  for (const auto& move : newFiles) journal << "NEW " << Narrow(move.from.wstring()) << " -> " << Narrow(move.to.wstring()) << '\n';
  journal.close();
  if (!journal) throw std::runtime_error("Unable to close promotion journal");

  bool ok = true;
  try {
    for (auto* moves : {&oldFiles, &newFiles}) {
      for (auto& move : *moves) {
        if (g_cancelRequested) { ok = false; break; }
        std::error_code ec;
        renameFile(move.from, move.to, ec);
        if (ec) {
          PostLog(hwnd, L"Package promotion failed: " + move.from.wstring() + L" - " + Widen(ec.message()));
          ok = false;
          break;
        }
        move.done = true;
      }
      if (!ok) break;
    }
    if (g_cancelRequested) ok = false;
  } catch (const std::exception& error) {
    PostLog(hwnd, L"Package promotion failed: " + Widen(error.what()));
    ok = false;
  }
  if (ok) return true;

  bool restored = true;
  for (auto* moves : {&newFiles, &oldFiles}) {
    for (auto it = moves->rbegin(); it != moves->rend(); ++it) {
      if (!it->done) continue;
      try {
        std::error_code ec;
        renameFile(it->to, it->from, ec);
        if (ec) restored = false;
      } catch (...) { restored = false; }
    }
  }
  if (!restored) workspace.Retain();
  else PostLog(hwnd, L"Previous package restored after unsuccessful promotion.");
  return false;
}

void ReportWorkerOutcome(HWND hwnd, bool committed, bool cancelled) {
  PostProgress(hwnd, committed ? 100 : 0);
  const std::wstring status = committed ? L"Package ready" : cancelled ? L"Stopped" : L"Package build failed";
  PostStatus(hwnd, status);
  PostLog(hwnd, status + L".");
}

void Worker(HWND hwnd, PackerConfig config, const WorkerDependencies& dependencies = {}) {
  g_workerRunning = true;
  PostProgress(hwnd, 0);
  struct Finished {
    HWND hwnd;
    bool committed{false};
    ~Finished() {
      ReportWorkerOutcome(hwnd, committed, g_cancelRequested.load());
      g_workerRunning = false;
      PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
    }
  } finished{hwnd};
  try {
    bool ok = true;
    uint64_t unpackedSize = 0;
    if (g_cancelRequested || !ValidateConfig(hwnd, config, false) ||
        !CheckWorkerPackagePaths(hwnd, config) || !ResolveSevenZip(hwnd, config)) return;
    if (config.copyInstaller && !IsPlainPackageFile(dependencies.installerSourceFolder / "modlist-installer.exe")) {
      PostLog(hwnd, L"Unable to prepare setup: modlist-installer.exe was not found beside modlist-packer.exe.");
      return;
    }
    const auto liveConfig = config;
    std::filesystem::create_directories(PackageFolder(config));
    std::unique_ptr<PackageWorkspace> workspace;
    if (config.archiveFirst) {
      unpackedSize = EstimateFolderBytes(hwnd, config.sourceFolder);
      if (g_cancelRequested) return;
    }
    // Manifest Only reserves metadata/installer space, without copying its live archive inputs.
    const auto available = std::filesystem::space(config.releaseFolder).available;
    const uint64_t overhead = std::max<uint64_t>(16ull * 1024 * 1024, unpackedSize / 16);
    const uint64_t installerSize = config.copyInstaller
        ? std::filesystem::file_size(dependencies.installerSourceFolder / "modlist-installer.exe") : 0;
    if (available < overhead || installerSize > available - overhead ||
        unpackedSize > available - overhead - installerSize) {
      PostLog(hwnd, L"Insufficient free space for a replacement alongside the previous package; previous package unchanged.");
      return;
    }
    workspace = std::make_unique<PackageWorkspace>(hwnd, config.releaseFolder);
    config.releaseFolder = workspace->Root() / kStagedDirectory;
    PostLog(hwnd, L"Building replacement in owned workspace: " + workspace->Root().wstring());
    if (!PrepareInstallerSetup(hwnd, config, dependencies.installerSourceFolder)) return;
    if (config.archiveFirst) {
      PostLog(hwnd, L"Creating archive...");
      PostLog(hwnd, L"Archive output folder: " + PackageFolder(config).wstring());
      PostLog(hwnd, L"7-Zip command: " + BuildSevenZipCommand(config));
      if (unpackedSize > 0) {
        PostLog(hwnd, L"Packing input size estimate (uncompressed source): " + FormatBytes(unpackedSize));
      }
      if (!g_cancelRequested) {
        PostStatus(hwnd, L"Starting 7-Zip...");
        const int exitCode = RunProcess(hwnd,
                                        BuildSevenZipCommand(config),
                                        L"Packing archive",
                                        unpackedSize,
                                        config.memoryLimitPercent);
        if (exitCode != 0) {
          PostLog(hwnd, L"7-Zip failed with exit code " + std::to_wstring(exitCode));
          ok = false;
        } else {
          PostLog(hwnd, L"Archive creation completed.");
        }
      }
      if (ok && config.testArchive && !g_cancelRequested) {
        PostLog(hwnd, L"Testing archive...");
        PostStatus(hwnd, L"Starting archive test...");
        uint64_t archiveBytes = 0;
        for (const auto& file : FindArchivePartsForBase(PackageFolder(config), config.archiveName + SelectArchiveExtension(config))) {
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
      const auto& manifestInput = config.archiveFirst ? config : liveConfig;
      ok = WriteManifest(hwnd, manifestInput, unpackedSize, dependencies.closeManifest, PackageFolder(config));
    }
    if (ok && !g_cancelRequested && workspace) {
      finished.committed = PromotePackage(hwnd, liveConfig, config, *workspace, dependencies.renameFile);
    }
  } catch (const std::exception& error) {
    PostLog(hwnd, L"Package build failed: " + Widen(error.what()));
  }
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
  g_cancelRequested = false;
  g_workerRunning = true;
  std::thread([hwnd, config = ReadConfig(archiveFirst)] { Worker(hwnd, config); }).detach();
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

  for (const wchar_t* item : {L"50%", L"60%", L"70%", L"80%", L"90%"}) {
    AddComboItem(g_memoryLimitCombo, item);
  }
  SelectCombo(g_memoryLimitCombo, 3);

  for (const wchar_t* item : {L"16m", L"64m", L"128m", L"256m"}) {
    AddComboItem(g_chunkCombo, item);
  }
  SelectCombo(g_chunkCombo, 1);

  AddComboItem(g_pathModeCombo, L"Relative paths");
  SelectCombo(g_pathModeCombo, 0);

  SetText(g_archiveNameEdit, L"MyPack");
  SendMessageW(g_copyInstallerCheck, BM_SETCHECK, BST_CHECKED, 0);
  EnableWindow(g_copyInstallerCheck, FALSE);
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
      CreateLabel(hwnd, L"RAM limit", 300, 296, 70, 20);
      g_memoryLimitCombo = CreateCombo(hwnd, kMemoryLimitCombo, 370, 292, 76, 160);
      CreateLabel(hwnd, L"Parameters", 458, 296, 75, 20);
      g_parametersEdit = CreateEdit(hwnd, kParametersEdit, 536, 292, 164, 24);

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
      SendMessageW(g_logEdit, EM_SETLIMITTEXT, 0, 0);
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
