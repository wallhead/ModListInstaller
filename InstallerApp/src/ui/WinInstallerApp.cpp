#define NOMINMAX

#include "app/PackageDiscovery.h"
#include "downloader/LibtorrentDownloader.h"
#include "extractor/SevenZipExtractor.h"
#include "paths/PathValidator.h"

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <cwctype>
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

constexpr int kDownloadEdit = 1003;
constexpr int kDownloadBrowse = 1004;
constexpr int kInstallEdit = 1005;
constexpr int kInstallBrowse = 1006;
constexpr int kValidateButton = 1007;
constexpr int kStartButton = 1008;
constexpr int kLogEdit = 1009;
constexpr int kProgress = 1010;
constexpr int kStatusLabel = 1011;
constexpr int kPauseButton = 1012;
constexpr int kStopButton = 1013;
constexpr int kUnpackButton = 1014;

constexpr UINT kLogMessage = WM_APP + 1;
constexpr UINT kProgressMessage = WM_APP + 2;
constexpr UINT kWorkerFinishedMessage = WM_APP + 3;
constexpr UINT kStatusMessage = WM_APP + 4;

HINSTANCE g_instance = nullptr;
HWND g_downloadEdit = nullptr;
HWND g_installEdit = nullptr;
HWND g_logEdit = nullptr;
HWND g_progress = nullptr;
HWND g_statusLabel = nullptr;
std::atomic_bool g_workerRunning{false};
std::atomic_bool g_closeAfterWorker{false};
std::mutex g_downloaderMutex;
std::shared_ptr<modlist::LibtorrentDownloader> g_activeDownloader;

std::wstring Widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(size), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
  return wide;
}

std::string Narrow(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  std::string narrow(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(), size, nullptr, nullptr);
  return narrow;
}

std::filesystem::path ModuleFolder() {
  wchar_t buffer[MAX_PATH]{};
  GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return std::filesystem::path(buffer).parent_path();
}

std::string Timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  localtime_s(&tm, &time);
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

std::filesystem::path AppLogPath() {
  const auto logFolder = ModuleFolder() / "logs";
  std::error_code ec;
  std::filesystem::create_directories(logFolder, ec);
  return logFolder / "modlist-installer.log";
}

void AppendAppLog(const std::wstring& text) {
  std::ofstream log(AppLogPath(), std::ios::binary | std::ios::app);
  if (log) {
    log << Timestamp() << " " << Narrow(text) << "\r\n";
  }
}

std::wstring TailForLog(const std::string& text, size_t maxChars = 4000) {
  if (text.empty()) {
    return {};
  }
  if (text.size() <= maxChars) {
    return Widen(text);
  }
  return L"...\r\n" + Widen(text.substr(text.size() - maxChars));
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

void AppendLog(const std::wstring& text) {
  AppendAppLog(text);
  const int length = GetWindowTextLengthW(g_logEdit);
  SendMessageW(g_logEdit, EM_SETSEL, length, length);
  std::wstring line = text + L"\r\n";
  SendMessageW(g_logEdit, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
}

void PostLog(HWND hwnd, std::wstring text) {
  PostMessageW(hwnd, kLogMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(std::move(text))));
}

void PostProgress(HWND hwnd, int progress) {
  PostMessageW(hwnd, kProgressMessage, static_cast<WPARAM>(progress), 0);
}

void PostStatus(HWND hwnd, std::wstring text) {
  PostMessageW(hwnd, kStatusMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(std::move(text))));
}

std::wstring PathToDisplay(const std::filesystem::path& path) {
  return path.wstring();
}

std::filesystem::path ExeFolder() {
  return ModuleFolder();
}

void WriteLastSevenZipLog(const std::string& output) {
  if (output.empty()) {
    return;
  }
  std::ofstream log(ExeFolder() / "last-7z-output.log", std::ios::binary);
  if (log) {
    log << output;
  }
}

std::optional<std::filesystem::path> PickFolder(HWND owner) {
  BROWSEINFOW browse{};
  browse.hwndOwner = owner;
  browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  PIDLIST_ABSOLUTE idList = SHBrowseForFolderW(&browse);
  if (idList == nullptr) {
    return std::nullopt;
  }

  wchar_t path[MAX_PATH]{};
  const bool ok = SHGetPathFromIDListW(idList, path) != FALSE;
  CoTaskMemFree(idList);
  if (!ok) {
    return std::nullopt;
  }
  return std::filesystem::path(path);
}

HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int width, int height) {
  return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                         x, y, width, height, parent, nullptr, g_instance, nullptr);
}

HWND CreateEdit(HWND parent, int id, int x, int y, int width, int height) {
  return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                         x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int width, int height) {
  return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                         x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

void Layout(HWND hwnd) {
  RECT rect{};
  GetClientRect(hwnd, &rect);
  const int width = rect.right - rect.left;
  const int margin = 16;
  const int labelWidth = 105;
  const int buttonWidth = 88;
  const int rowHeight = 25;
  const int editX = margin + labelWidth;
  const int editWidth = width - editX - buttonWidth - margin * 2;
  const int buttonX = editX + editWidth + 8;

  MoveWindow(GetDlgItem(hwnd, kDownloadEdit), editX, 22, editWidth, rowHeight, TRUE);
  MoveWindow(GetDlgItem(hwnd, kDownloadBrowse), buttonX, 22, buttonWidth, rowHeight, TRUE);
  MoveWindow(GetDlgItem(hwnd, kInstallEdit), editX, 57, editWidth, rowHeight, TRUE);
  MoveWindow(GetDlgItem(hwnd, kInstallBrowse), buttonX, 57, buttonWidth, rowHeight, TRUE);
  MoveWindow(GetDlgItem(hwnd, kValidateButton), editX, 96, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kStartButton), editX + 132, 96, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kUnpackButton), editX + 264, 96, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kPauseButton), editX + 396, 96, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kStopButton), editX + 528, 96, 92, 30, TRUE);
  MoveWindow(g_progress, margin, 142, width - margin * 2, 20, TRUE);
  MoveWindow(g_statusLabel, margin, 170, width - margin * 2, 22, TRUE);
  MoveWindow(g_logEdit, margin, 202, width - margin * 2, rect.bottom - 218, TRUE);
}

modlist::Result<modlist::PackageDiscovery> ReadPackageFromUi() {
  return modlist::DiscoverPackageNear(ExeFolder());
}

std::wstring StageToText(modlist::DownloadStage stage);
std::wstring FormatBytes(uintmax_t bytes);

std::optional<std::filesystem::path> FindFirstArchivePart(const std::filesystem::path& folder) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
    return std::nullopt;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec)) {
    if (ec) {
      break;
    }
    if (!entry.is_regular_file()) {
      continue;
    }
    std::wstring name = entry.path().filename().wstring();
    for (auto& ch : name) {
      ch = static_cast<wchar_t>(towlower(ch));
    }
    if (name.ends_with(L".7z.001")) {
      return entry.path();
    }
  }
  return std::nullopt;
}

bool IsArchiveVolume(const std::filesystem::path& path) {
  std::wstring name = path.filename().wstring();
  for (auto& ch : name) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return name.find(L".7z.") != std::wstring::npos || name.ends_with(L".7z");
}

bool IsPlainSevenZipArchive(const std::filesystem::path& path) {
  std::wstring name = path.filename().wstring();
  for (auto& ch : name) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return name.ends_with(L".7z") && name.find(L".7z.") == std::wstring::npos;
}

bool IsFirstSplitArchivePart(const std::filesystem::path& path) {
  std::wstring name = path.filename().wstring();
  for (auto& ch : name) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return name.ends_with(L".001");
}

std::vector<std::filesystem::path> FindPlainSevenZipArchives(const std::filesystem::path& folder) {
  std::vector<std::filesystem::path> archives;
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
    return archives;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(folder, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file() && IsPlainSevenZipArchive(entry.path())) {
      archives.push_back(entry.path());
    }
  }
  return archives;
}

uintmax_t EstimateNearbyArchiveBytes(const std::filesystem::path& folder) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec) || !std::filesystem::is_directory(folder, ec)) {
    return 0;
  }
  uintmax_t total = 0;
  for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
    if (ec || !entry.is_regular_file() || !IsArchiveVolume(entry.path())) {
      continue;
    }
    total += entry.file_size(ec);
    if (ec) {
      ec.clear();
    }
  }
  return total;
}

uintmax_t EstimateRequiredBytes(const modlist::PackageDiscovery& package) {
  auto torrentSize = modlist::LibtorrentDownloader::ReadTorrentPayloadSize(package.torrentFile);
  if (torrentSize.ok()) {
    AppendLog(L"Torrent payload size: " + FormatBytes(torrentSize.value()));
    return torrentSize.value();
  }

  AppendLog(L"Torrent size warning: " + Widen(torrentSize.error()));
  const uintmax_t archiveBytes = EstimateNearbyArchiveBytes(ExeFolder());
  if (archiveBytes > 0) {
    AppendLog(L"Using nearby archive size estimate: " + FormatBytes(archiveBytes));
  }
  return archiveBytes;
}

uintmax_t FreeBytes(const std::filesystem::path& folder) {
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  const auto space = std::filesystem::space(folder, ec);
  return ec ? 0 : space.available;
}

std::wstring FormatBytesPerSecond(int bytesPerSecond) {
  if (bytesPerSecond <= 0) {
    return L"0 B/s";
  }
  const wchar_t* units[] = {L"B/s", L"KB/s", L"MB/s", L"GB/s"};
  double value = static_cast<double>(bytesPerSecond);
  size_t unit = 0;
  while (value >= 1024.0 && unit < 3) {
    value /= 1024.0;
    ++unit;
  }
  std::wostringstream out;
  out << std::fixed << std::setprecision(unit == 0 ? 0 : 1) << value << L" " << units[unit];
  return out.str();
}

std::wstring FormatBytes(uintmax_t bytes) {
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

std::wstring FormatEta(int seconds) {
  if (seconds < 0) {
    return L"unknown";
  }
  const int hours = seconds / 3600;
  const int minutes = (seconds % 3600) / 60;
  const int secs = seconds % 60;
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

bool HasEnoughSpace(const std::filesystem::path& folder, uintmax_t requiredBytes, const std::wstring& label) {
  if (requiredBytes == 0) {
    AppendLog(label + L" required space: unknown until torrent metadata is available.");
    return true;
  }
  const uintmax_t free = FreeBytes(folder);
  AppendLog(label + L" free space: " + FormatBytes(free) + L"; minimum required: " + FormatBytes(requiredBytes));
  return free >= requiredBytes;
}

std::wstring FormatDownloadStatus(const modlist::DownloadStatus& status) {
  std::wostringstream out;
  out << StageToText(status.stage)
      << L" | " << static_cast<int>(status.progress * 100.0f) << L"%"
      << L" | Down " << FormatBytesPerSecond(status.downloadRateBytesPerSecond)
      << L" | Up " << FormatBytesPerSecond(status.uploadRateBytesPerSecond)
      << L" | Seeds " << status.seedCount
      << L" | Peers " << status.peerCount
      << L" | ETA " << FormatEta(status.etaSeconds);
  if (status.totalBytes > 0) {
    out << L" | " << FormatBytes(static_cast<uintmax_t>(status.downloadedBytes))
        << L" / " << FormatBytes(static_cast<uintmax_t>(status.totalBytes));
  }
  return out.str();
}

void FinishWorker(HWND hwnd, const std::shared_ptr<modlist::LibtorrentDownloader>& downloader) {
  {
    std::lock_guard<std::mutex> lock(g_downloaderMutex);
    if (g_activeDownloader == downloader) {
      g_activeDownloader.reset();
    }
  }
  g_workerRunning = false;
  PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
}

std::wstring StageToText(modlist::DownloadStage stage) {
  switch (stage) {
    case modlist::DownloadStage::Idle:
      return L"Idle";
    case modlist::DownloadStage::Loading:
      return L"Loading torrent";
    case modlist::DownloadStage::Downloading:
      return L"Downloading";
    case modlist::DownloadStage::Checking:
      return L"Checking";
    case modlist::DownloadStage::Seeding:
      return L"Seeding";
    case modlist::DownloadStage::Completed:
      return L"Completed";
    case modlist::DownloadStage::Paused:
      return L"Paused";
    case modlist::DownloadStage::Cancelled:
      return L"Cancelled";
    case modlist::DownloadStage::Failed:
      return L"Failed";
  }
  return L"Unknown";
}

std::wstring FormatExtractionStatus(const std::wstring& label, int percent) {
  std::wostringstream out;
  out << label << L" " << percent << L"%";
  return out.str();
}

bool RunExtractionStep(HWND hwnd,
                       modlist::SevenZipExtractor& extractor,
                       const modlist::ExtractionConfig& extraction,
                       const std::wstring& statusLabel,
                       int progressBase,
                       int progressSpan) {
  PostLog(hwnd, L"Extracting: " + PathToDisplay(extraction.archiveFirstPart));
  PostProgress(hwnd, progressBase);
  PostStatus(hwnd, FormatExtractionStatus(statusLabel, 0));
  int lastPercent = -1;
  const auto result = extractor.Extract(extraction, [hwnd, statusLabel, progressBase, progressSpan, &lastPercent](int percent) {
    if (percent == lastPercent) {
      return;
    }
    lastPercent = percent;
    const int mapped = progressBase + (percent * progressSpan) / 100;
    PostProgress(hwnd, mapped);
    PostStatus(hwnd, FormatExtractionStatus(statusLabel, percent));
  });
  PostLog(hwnd, Widen(result.message));
  WriteLastSevenZipLog(result.output);
  if (!result.ok) {
    if (!result.outputLogPath.empty()) {
      PostLog(hwnd, L"Full 7-Zip output saved to: " + PathToDisplay(result.outputLogPath));
    }
    if (!result.output.empty()) {
      PostLog(hwnd, L"7-Zip error details:");
      PostLog(hwnd, TailForLog(result.output, 2000));
    } else {
      PostLog(hwnd, L"7-Zip produced no captured output. Check the saved log path above.");
    }
  } else if (result.exitCode == 1 && !result.outputLogPath.empty()) {
    PostLog(hwnd, L"Warning details saved to: " + PathToDisplay(result.outputLogPath));
  }
  if (result.ok) {
    PostProgress(hwnd, progressBase + progressSpan);
    PostStatus(hwnd, FormatExtractionStatus(statusLabel, 100));
  }
  return result.ok;
}

bool ExtractArchiveChain(HWND hwnd,
                         modlist::SevenZipExtractor& extractor,
                         const std::filesystem::path& sevenZipExe,
                         std::filesystem::path archiveFirstPart,
                         std::filesystem::path installFolder) {
  modlist::ExtractionConfig extraction;
  extraction.sevenZipExe = sevenZipExe;
  extraction.archiveFirstPart = std::move(archiveFirstPart);
  extraction.installFolder = installFolder;
  extraction.useSameDiskTemp = true;

  const bool splitArchive = IsFirstSplitArchivePart(extraction.archiveFirstPart);
  const int firstSpan = splitArchive ? 50 : 100;
  if (!RunExtractionStep(hwnd, extractor, extraction, L"Unpacking", 0, firstSpan)) {
    return false;
  }

  const auto innerArchives = FindPlainSevenZipArchives(installFolder);
  if (innerArchives.empty()) {
    return true;
  }
  if (innerArchives.size() != 1) {
    PostLog(hwnd, L"Found more than one inner .7z archive, so automatic second extraction was skipped.");
    return true;
  }

  PostLog(hwnd, L"Detected inner archive, starting second extraction: " + PathToDisplay(innerArchives.front()));
  extraction.archiveFirstPart = innerArchives.front();
  extraction.installFolder = innerArchives.front().parent_path();
  return RunExtractionStep(hwnd, extractor, extraction, L"Unpacking inner archive", 50, 50);
}

void RunInstallWorker(HWND hwnd,
                      modlist::PackageDiscovery package,
                      std::filesystem::path downloadFolder,
                      std::filesystem::path installFolder,
                      std::shared_ptr<modlist::LibtorrentDownloader> downloader) {
  g_workerRunning = true;
  PostProgress(hwnd, 0);
  PostLog(hwnd, L"Starting torrent download...");
  PostLog(hwnd, L"Existing files in the selected download folder will be checked before downloading.");

  modlist::DownloadConfig config;
  config.torrent.type = modlist::TorrentSourceType::TorrentFile;
  config.torrent.source = package.torrentFile.string();
  config.downloadFolder = std::move(downloadFolder);
  config.features.enableDht = true;
  config.features.enablePex = true;
  config.features.enableLsd = true;

  downloader->Start(config);

  modlist::DownloadStage lastStage = modlist::DownloadStage::Idle;
  bool checkedTorrentSpace = false;
  while (true) {
    const auto status = downloader->GetStatus();
    if (status.stage != lastStage) {
      lastStage = status.stage;
      PostLog(hwnd, L"Torrent stage: " + StageToText(status.stage));
    }
    PostProgress(hwnd, static_cast<int>(status.progress * 100.0f));
    PostStatus(hwnd, FormatDownloadStatus(status));

    if (!checkedTorrentSpace && status.totalBytes > 0) {
      checkedTorrentSpace = true;
      const uintmax_t remaining = static_cast<uintmax_t>(status.totalBytes - status.downloadedBytes);
      const uintmax_t free = FreeBytes(config.downloadFolder);
      PostLog(hwnd, L"Download free space: " + FormatBytes(free) + L"; remaining torrent bytes: " + FormatBytes(remaining));
      if (free < remaining) {
        downloader->Cancel();
        PostLog(hwnd, L"Not enough free space in the download folder.");
        FinishWorker(hwnd, downloader);
        return;
      }
    }

    if (status.stage == modlist::DownloadStage::Completed) {
      break;
    }
    if (status.stage == modlist::DownloadStage::Failed) {
      PostLog(hwnd, L"Torrent error: " + Widen(status.error));
      FinishWorker(hwnd, downloader);
      return;
    }
    if (status.stage == modlist::DownloadStage::Cancelled) {
      PostLog(hwnd, L"Torrent cancelled.");
      FinishWorker(hwnd, downloader);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  PostLog(hwnd, L"Torrent finished. Validating downloaded files...");
  downloader->ForceRecheck();
  lastStage = modlist::DownloadStage::Idle;
  while (true) {
    const auto status = downloader->GetStatus();
    if (status.stage != lastStage) {
      lastStage = status.stage;
      PostLog(hwnd, L"Torrent validation stage: " + StageToText(status.stage));
    }
    PostProgress(hwnd, static_cast<int>(status.progress * 100.0f));
    PostStatus(hwnd, FormatDownloadStatus(status));

    if (status.stage == modlist::DownloadStage::Completed) {
      PostLog(hwnd, L"Download validation completed.");
      break;
    }
    if (status.stage == modlist::DownloadStage::Failed) {
      PostLog(hwnd, L"Torrent validation error: " + Widen(status.error));
      FinishWorker(hwnd, downloader);
      return;
    }
    if (status.stage == modlist::DownloadStage::Cancelled) {
      PostLog(hwnd, L"Torrent validation cancelled.");
      FinishWorker(hwnd, downloader);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  PostLog(hwnd, L"Torrent finished. Releasing downloaded files before unpacking...");
  {
    std::lock_guard<std::mutex> lock(g_downloaderMutex);
    if (g_activeDownloader == downloader) {
      g_activeDownloader.reset();
    }
  }
  downloader->ReleaseFiles();

  PostLog(hwnd, L"Torrent finished. Looking for first .7z.001 archive part...");
  auto firstArchivePart = package.firstArchivePart;
  if (!firstArchivePart.has_value()) {
    firstArchivePart = FindFirstArchivePart(config.downloadFolder);
  }
  if (!firstArchivePart.has_value()) {
    PostLog(hwnd, L"No .7z.001 archive part found after download. Cannot extract automatically.");
    FinishWorker(hwnd, downloader);
    return;
  }

  const uintmax_t archiveBytes = EstimateNearbyArchiveBytes(firstArchivePart->parent_path());
  if (archiveBytes > 0) {
    const uintmax_t installFree = FreeBytes(installFolder);
    PostLog(hwnd, L"Install free space: " + FormatBytes(installFree) + L"; archive size minimum: " + FormatBytes(archiveBytes));
    if (installFree < archiveBytes) {
      PostLog(hwnd, L"Not enough free space in the install folder for extraction.");
      FinishWorker(hwnd, downloader);
      return;
    }
  }

  modlist::SevenZipExtractor extractor;
  auto sevenZip = extractor.LocateExecutable(ExeFolder());
  if (!sevenZip.ok()) {
    PostLog(hwnd, L"7-Zip error: " + Widen(sevenZip.error()));
    FinishWorker(hwnd, downloader);
    return;
  }

  const bool extracted = ExtractArchiveChain(hwnd, extractor, sevenZip.value(), *firstArchivePart, std::move(installFolder));
  PostProgress(hwnd, extracted ? 100 : 0);
  FinishWorker(hwnd, downloader);
}

bool ValidateFolders(uintmax_t knownRequiredBytes = 0) {
  modlist::PathValidator validator;
  bool ok = true;

  const auto downloadText = GetText(g_downloadEdit);
  if (!downloadText.empty()) {
    const auto result = validator.ValidateDownloadFolder(std::filesystem::path(downloadText), knownRequiredBytes);
    AppendLog(L"Download folder: " + Widen(result.message));
    if (result.warning) {
      AppendLog(L"Warning: " + Widen(result.message));
    }
    ok = ok && result.ok;
  } else {
    AppendLog(L"Download folder: not selected yet.");
    ok = false;
  }

  const auto installText = GetText(g_installEdit);
  if (!installText.empty()) {
    const auto result = validator.ValidateInstallFolder(std::filesystem::path(installText), knownRequiredBytes);
    AppendLog(L"Install folder: " + Widen(result.message));
    if (result.warning) {
      AppendLog(L"Warning: " + Widen(result.message));
    }
    ok = ok && result.ok;
  } else {
    AppendLog(L"Install folder: not selected yet. Use a short path like D:\\Sky.");
    ok = false;
  }

  if (!downloadText.empty() && !installText.empty()) {
    if (!validator.IsSameDrive(std::filesystem::path(downloadText), std::filesystem::path(installText))) {
      AppendLog(L"Note: download and install folders are on different drives. Extraction temp will still stay under the install folder.");
    }
  }

  if (!downloadText.empty() && !HasEnoughSpace(std::filesystem::path(downloadText), knownRequiredBytes, L"Download folder")) {
    ok = false;
  }
  if (!installText.empty() && !HasEnoughSpace(std::filesystem::path(installText), knownRequiredBytes, L"Install folder")) {
    ok = false;
  }

  return ok;
}

void ValidatePackage() {
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  AppendLog(L"Validating package...");
  auto package = ReadPackageFromUi();
  if (!package.ok()) {
    AppendLog(L"Package error: " + Widen(package.error()));
    return;
  }

  AppendLog(L"Torrent: " + PathToDisplay(package.value().torrentFile));
  if (package.value().firstArchivePart.has_value()) {
    AppendLog(L"Archive first part: " + PathToDisplay(*package.value().firstArchivePart));
  } else {
    AppendLog(L"No .7z.001 found next to the torrent yet. That is OK before download.");
  }

  const uintmax_t knownRequiredBytes = EstimateRequiredBytes(package.value());
  ValidateFolders(knownRequiredBytes);

  modlist::SevenZipExtractor extractor;
  auto sevenZip = extractor.LocateExecutable(ExeFolder());
  if (sevenZip.ok()) {
    AppendLog(L"7-Zip: " + PathToDisplay(sevenZip.value()));
  } else {
    AppendLog(L"7-Zip warning: " + Widen(sevenZip.error()));
  }
  SendMessageW(g_progress, PBM_SETPOS, 100, 0);
}

void SetControlsRunning(HWND hwnd, bool running) {
  EnableWindow(GetDlgItem(hwnd, kStartButton), running ? FALSE : TRUE);
  EnableWindow(GetDlgItem(hwnd, kValidateButton), running ? FALSE : TRUE);
  EnableWindow(GetDlgItem(hwnd, kUnpackButton), running ? FALSE : TRUE);
  EnableWindow(GetDlgItem(hwnd, kDownloadBrowse), running ? FALSE : TRUE);
  EnableWindow(GetDlgItem(hwnd, kInstallBrowse), running ? FALSE : TRUE);
  EnableWindow(GetDlgItem(hwnd, kPauseButton), running ? TRUE : FALSE);
  EnableWindow(GetDlgItem(hwnd, kStopButton), running ? TRUE : FALSE);
  SetWindowTextW(GetDlgItem(hwnd, kPauseButton), L"Pause");
}

std::shared_ptr<modlist::LibtorrentDownloader> ActiveDownloader() {
  std::lock_guard<std::mutex> lock(g_downloaderMutex);
  return g_activeDownloader;
}

void TogglePause(HWND hwnd) {
  auto downloader = ActiveDownloader();
  if (!downloader) {
    AppendLog(L"No active download to pause.");
    return;
  }

  const auto status = downloader->GetStatus();
  if (status.stage == modlist::DownloadStage::Paused) {
    downloader->Resume();
    SetWindowTextW(GetDlgItem(hwnd, kPauseButton), L"Pause");
    AppendLog(L"Download resumed.");
  } else {
    downloader->Pause();
    SetWindowTextW(GetDlgItem(hwnd, kPauseButton), L"Resume");
    AppendLog(L"Download paused.");
  }
}

void StopInstall() {
  auto downloader = ActiveDownloader();
  if (!downloader) {
    AppendLog(L"No active download to stop.");
    return;
  }
  downloader->Cancel();
  AppendLog(L"Stopping download. Existing files are left in the download folder.");
}

void StartInstall(HWND hwnd) {
  if (g_workerRunning) {
    AppendLog(L"Installer is already running.");
    return;
  }
  AppendLog(L"Starting installer flow...");
  auto package = ReadPackageFromUi();
  if (!package.ok()) {
    AppendLog(L"Package error: " + Widen(package.error()));
    return;
  }
  const uintmax_t knownRequiredBytes = EstimateRequiredBytes(package.value());
  if (!ValidateFolders(knownRequiredBytes)) {
    AppendLog(L"Fix the folder warnings/errors before starting.");
    return;
  }

  const auto downloadFolder = std::filesystem::path(GetText(g_downloadEdit));
  const auto installFolder = std::filesystem::path(GetText(g_installEdit));
  auto downloader = std::make_shared<modlist::LibtorrentDownloader>();
  {
    std::lock_guard<std::mutex> lock(g_downloaderMutex);
    g_activeDownloader = downloader;
  }
  g_workerRunning = true;
  SetControlsRunning(hwnd, true);
  g_closeAfterWorker = false;
  std::thread(RunInstallWorker, hwnd, std::move(package.value()), downloadFolder, installFolder, downloader).detach();
}

void RunUnpackWorker(HWND hwnd, std::filesystem::path archiveFirstPart, std::filesystem::path installFolder) {
  g_workerRunning = true;
  PostProgress(hwnd, 0);
  PostLog(hwnd, L"Unpacking without torrent validation: " + PathToDisplay(archiveFirstPart));

  const uintmax_t archiveBytes = EstimateNearbyArchiveBytes(archiveFirstPart.parent_path());
  if (archiveBytes > 0) {
    const uintmax_t installFree = FreeBytes(installFolder);
    PostLog(hwnd, L"Install free space: " + FormatBytes(installFree) + L"; archive size minimum: " + FormatBytes(archiveBytes));
    if (installFree < archiveBytes) {
      PostLog(hwnd, L"Not enough free space in the install folder for extraction.");
      g_workerRunning = false;
      PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
      return;
    }
  }

  modlist::SevenZipExtractor extractor;
  auto sevenZip = extractor.LocateExecutable(ExeFolder());
  if (!sevenZip.ok()) {
    PostLog(hwnd, L"7-Zip error: " + Widen(sevenZip.error()));
    g_workerRunning = false;
    PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
    return;
  }

  const bool extracted = ExtractArchiveChain(hwnd, extractor, sevenZip.value(), std::move(archiveFirstPart), std::move(installFolder));
  PostProgress(hwnd, extracted ? 100 : 0);
  g_workerRunning = false;
  PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
}

void UnpackOnly(HWND hwnd) {
  if (g_workerRunning) {
    AppendLog(L"Installer is already running.");
    return;
  }

  const auto downloadFolder = std::filesystem::path(GetText(g_downloadEdit));
  const auto installFolder = std::filesystem::path(GetText(g_installEdit));
  if (downloadFolder.empty() || installFolder.empty()) {
    AppendLog(L"Select download and install folders before unpacking.");
    return;
  }

  modlist::PathValidator validator;
  const auto install = validator.ValidateInstallFolder(installFolder);
  AppendLog(L"Install folder: " + Widen(install.message));
  if (!install.ok) {
    return;
  }

  auto archive = FindFirstArchivePart(downloadFolder);
  if (!archive.has_value()) {
    archive = FindFirstArchivePart(ExeFolder());
  }
  if (!archive.has_value()) {
    AppendLog(L"No .7z.001 archive part found in the download folder or next to the exe.");
    return;
  }

  SetControlsRunning(hwnd, true);
  g_closeAfterWorker = false;
  std::thread(RunUnpackWorker, hwnd, *archive, installFolder).detach();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: {
      INITCOMMONCONTROLSEX controls{};
      controls.dwSize = sizeof(controls);
      controls.dwICC = ICC_PROGRESS_CLASS;
      InitCommonControlsEx(&controls);
      std::filesystem::create_directories(ExeFolder() / "logs");

      CreateLabel(hwnd, L"Download", 16, 26, 100, 20);
      CreateLabel(hwnd, L"Install", 16, 61, 100, 20);
      g_downloadEdit = CreateEdit(hwnd, kDownloadEdit, 120, 22, 420, 25);
      g_installEdit = CreateEdit(hwnd, kInstallEdit, 120, 57, 420, 25);
      CreateButton(hwnd, kDownloadBrowse, L"Browse", 550, 22, 88, 25);
      CreateButton(hwnd, kInstallBrowse, L"Browse", 550, 57, 88, 25);
      CreateButton(hwnd, kValidateButton, L"Validate", 120, 96, 120, 30);
      CreateButton(hwnd, kStartButton, L"Start", 252, 96, 120, 30);
      CreateButton(hwnd, kUnpackButton, L"Unpack", 384, 96, 120, 30);
      CreateButton(hwnd, kPauseButton, L"Pause", 516, 96, 120, 30);
      CreateButton(hwnd, kStopButton, L"Stop", 648, 96, 92, 30);
      g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                   16, 142, 622, 20, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgress)), g_instance, nullptr);
      g_statusLabel = CreateWindowExW(0, L"STATIC", L"Idle | Down 0 B/s | Up 0 B/s | Seeds 0 | Peers 0 | ETA unknown",
                                      WS_CHILD | WS_VISIBLE,
                                      16, 170, 622, 22, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabel)), g_instance, nullptr);
      g_logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                  16, 177, 622, 258, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLogEdit)), g_instance, nullptr);
      SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
      SetText(g_downloadEdit, (ExeFolder() / "downloads").wstring());
      SetText(g_installEdit, L"D:\\Sky");
      AppendLog(L"App log: " + PathToDisplay(AppLogPath()));
      AppendLog(L"Place exactly one .torrent file next to this exe.");
      auto package = ReadPackageFromUi();
      if (package.ok()) {
        AppendLog(L"Found torrent: " + PathToDisplay(package.value().torrentFile));
        auto torrentSize = modlist::LibtorrentDownloader::ReadTorrentPayloadSize(package.value().torrentFile);
        if (torrentSize.ok()) {
          AppendLog(L"Torrent payload size: " + FormatBytes(torrentSize.value()));
        } else {
          AppendLog(L"Torrent size warning: " + Widen(torrentSize.error()));
        }
      } else {
        AppendLog(L"Torrent auto-detect: " + Widen(package.error()));
      }
      AppendLog(L"Use a short install path near the drive root, for example D:\\Sky.");
      Layout(hwnd);
      SetControlsRunning(hwnd, false);
      return 0;
    }
    case WM_SIZE:
      Layout(hwnd);
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == kDownloadBrowse) {
        if (auto path = PickFolder(hwnd)) {
          SetText(g_downloadEdit, path->wstring());
        }
      } else if (id == kInstallBrowse) {
        if (auto path = PickFolder(hwnd)) {
          SetText(g_installEdit, path->wstring());
        }
      } else if (id == kValidateButton) {
        ValidatePackage();
      } else if (id == kStartButton) {
        StartInstall(hwnd);
      } else if (id == kUnpackButton) {
        UnpackOnly(hwnd);
      } else if (id == kPauseButton) {
        TogglePause(hwnd);
      } else if (id == kStopButton) {
        StopInstall();
      }
      return 0;
    }
    case kLogMessage: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      AppendLog(*text);
      return 0;
    }
    case kProgressMessage:
      SendMessageW(g_progress, PBM_SETPOS, static_cast<int>(wParam), 0);
      return 0;
    case kStatusMessage: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      SetWindowTextW(g_statusLabel, text->c_str());
      return 0;
    }
    case kWorkerFinishedMessage:
      SetControlsRunning(hwnd, false);
      if (g_closeAfterWorker) {
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_CLOSE:
      if (g_workerRunning) {
        g_closeAfterWorker = true;
        StopInstall();
        SetWindowTextW(hwnd, L"Modlist Installer - stopping...");
        AppendLog(L"Waiting for downloader to stop before closing.");
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

  const wchar_t className[] = L"ModlistInstallerWindow";
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = className;
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&windowClass);

  HWND hwnd = CreateWindowExW(0, className, L"Modlist Installer",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 860, 520,
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
