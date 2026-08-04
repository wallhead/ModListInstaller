#define NOMINMAX

#include "app/PackageDiscovery.h"
#include "extractor/PipelinedSevenZipExtractor.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Manifest.h"
#include "paths/PathValidator.h"
#include "resource.h"
#include "ui/NativeInstallerView.h"
#include "ui/NativeStrings.h"
#include "verifier/Sha256.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <winioctl.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <ctime>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr const wchar_t* kUnpackFolderName = L"Unpacked";

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
constexpr int kPreviousButton = 1015;
constexpr int kNextButton = 1016;
constexpr int kUnpackDriveCombo = 1017;
constexpr int kOpenLogButton = 1018;
constexpr int kDrivePickerButton = 1019;

constexpr UINT kLogMessage = WM_APP + 1;
constexpr UINT kProgressMessage = WM_APP + 2;
constexpr UINT kWorkerFinishedMessage = WM_APP + 3;
constexpr UINT kStatusMessage = WM_APP + 4;
constexpr UINT kValidationFailedMessage = WM_APP + 5;

enum class WizardPage {
  Welcome,
  Folders,
  Activity,
};

HINSTANCE g_instance = nullptr;
HWND g_stepLabel = nullptr;
HWND g_welcomeTitle = nullptr;
HWND g_welcomeBody = nullptr;
HWND g_downloadLabel = nullptr;
HWND g_unpackDriveLabel = nullptr;
HWND g_unpackTargetLabel = nullptr;
HWND g_installLabel = nullptr;
HWND g_downloadEdit = nullptr;
HWND g_unpackDriveCombo = nullptr;
HWND g_installEdit = nullptr;
HWND g_logEdit = nullptr;
HWND g_progress = nullptr;
HWND g_statusLabel = nullptr;
HWND g_previousButton = nullptr;
HWND g_nextButton = nullptr;
HWND g_hotButton = nullptr;
HWND g_mainWindow = nullptr;
modlist::NativeInstallerView g_nativeView;
modlist::NativeStrings g_strings;
std::wstring g_archiveFolderName;
struct PendingFolderCleanup {
  std::filesystem::path unpackFolder;
  std::filesystem::path installFolder;

  bool empty() const {
    return unpackFolder.empty() && installFolder.empty();
  }
};
std::optional<PendingFolderCleanup> g_pendingFolderCleanup;
std::wstring g_statusText = L"Ожидание | Ожидание проверки";
int g_progressPercent = 0;
WizardPage g_page = WizardPage::Folders;
HBRUSH g_contentBrush = nullptr;
HBRUSH g_headerBrush = nullptr;
HBRUSH g_panelBrush = nullptr;
HBRUSH g_footerBrush = nullptr;
HBRUSH g_editBrush = nullptr;
HFONT g_stepFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_bodyFont = nullptr;
HFONT g_labelFont = nullptr;
std::atomic_bool g_workerRunning{false};
std::atomic_bool g_closeAfterWorker{false};
std::atomic_bool g_stopRequested{false};
bool g_installCompleted = false;

std::wstring UiText(std::string_view key, std::wstring_view fallback) {
  return g_strings.Get(key, fallback);
}

constexpr COLORREF kRailColor = RGB(23, 28, 31);
constexpr COLORREF kRailDarkColor = RGB(16, 20, 22);
constexpr COLORREF kContentColor = RGB(16, 20, 22);
constexpr COLORREF kHeaderColor = RGB(29, 36, 40);
constexpr COLORREF kPanelColor = RGB(23, 28, 31);
constexpr COLORREF kEditColor = RGB(18, 23, 25);
constexpr COLORREF kFooterColor = RGB(29, 36, 40);
constexpr COLORREF kLineColor = RGB(42, 50, 54);
constexpr COLORREF kStrongLineColor = RGB(58, 68, 74);
constexpr COLORREF kPrimaryTextColor = RGB(246, 247, 242);
constexpr COLORREF kMutedTextColor = RGB(194, 201, 203);
constexpr COLORREF kDimTextColor = RGB(146, 156, 160);
constexpr COLORREF kAccentTextColor = RGB(173, 216, 235);
constexpr COLORREF kAccentStrongColor = RGB(220, 241, 250);
constexpr COLORREF kDangerColor = RGB(168, 111, 111);
constexpr COLORREF kButtonTopColor = RGB(34, 41, 45);
constexpr COLORREF kButtonBottomColor = RGB(23, 28, 31);
constexpr COLORREF kButtonHoverTopColor = RGB(42, 51, 56);
constexpr COLORREF kButtonHoverBottomColor = RGB(27, 33, 37);
constexpr COLORREF kButtonPressedTopColor = RGB(18, 23, 25);
constexpr COLORREF kButtonPressedBottomColor = RGB(16, 20, 22);
constexpr COLORREF kButtonBorderColor = RGB(58, 68, 74);
constexpr COLORREF kButtonDisabledTextColor = RGB(102, 111, 115);

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

std::wstring JsonEscape(const std::wstring& text) {
  std::wostringstream out;
  for (const wchar_t ch : text) {
    switch (ch) {
      case L'\\':
        out << L"\\\\";
        break;
      case L'"':
        out << L"\\\"";
        break;
      case L'\b':
        out << L"\\b";
        break;
      case L'\f':
        out << L"\\f";
        break;
      case L'\n':
        out << L"\\n";
        break;
      case L'\r':
        out << L"\\r";
        break;
      case L'\t':
        out << L"\\t";
        break;
      default:
        if (ch < 0x20) {
          out << L"\\u" << std::hex << std::setw(4) << std::setfill(L'0') << static_cast<int>(ch)
              << std::dec << std::setfill(L' ');
        } else {
          out << ch;
        }
        break;
    }
  }
  return out.str();
}

std::wstring JsonString(const std::wstring& text) {
  return L"\"" + JsonEscape(text) + L"\"";
}

void PostUiJson(const std::wstring& json) {
  (void)json;
}

void SendUiProgress(int percent, const std::wstring& status);
void SendUiStatus(const std::wstring& status);
void SendUiLog(const std::wstring& message);
void SendUiStep(const std::wstring& stepName);
void SendUiPath(const std::wstring& fieldName, const std::wstring& path);
void SendUiOption(const std::wstring& optionName, bool value);
void SendUiError(const std::wstring& title, const std::wstring& message);
void SendUiCleanupConfirm(const PendingFolderCleanup& folders);
void SendUiButtonEnabled(const std::wstring& buttonName, bool enabled);
void SendUiState();
void ConfirmPendingFolderCleanup(HWND hwnd);
void CancelPendingFolderCleanup();

void SetDwmColorAttribute(HWND hwnd, DWORD attribute, COLORREF color) {
  HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
  if (dwm == nullptr) {
    return;
  }

  using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
  auto setWindowAttribute =
      reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
  if (setWindowAttribute != nullptr) {
    setWindowAttribute(hwnd, attribute, &color, sizeof(color));
  }
  FreeLibrary(dwm);
}

void ApplyWindowFrameTheme(HWND hwnd) {
  BOOL darkMode = TRUE;
  HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
  if (dwm != nullptr) {
    using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto setWindowAttribute =
        reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (setWindowAttribute != nullptr) {
      constexpr DWORD kUseImmersiveDarkMode = 20;
      setWindowAttribute(hwnd, kUseImmersiveDarkMode, &darkMode, sizeof(darkMode));
    }
    FreeLibrary(dwm);
  }

  constexpr DWORD kDwmwaBorderColor = 34;
  constexpr DWORD kDwmwaCaptionColor = 35;
  constexpr DWORD kDwmwaTextColor = 36;
  SetDwmColorAttribute(hwnd, kDwmwaBorderColor, kLineColor);
  SetDwmColorAttribute(hwnd, kDwmwaCaptionColor, kHeaderColor);
  SetDwmColorAttribute(hwnd, kDwmwaTextColor, kPrimaryTextColor);
}

void FillVerticalGradient(HDC dc, RECT rect, COLORREF top, COLORREF bottom) {
  const int height = rect.bottom - rect.top;
  if (height <= 0) {
    return;
  }

  for (int y = 0; y < height; ++y) {
    const int red = GetRValue(top) + (GetRValue(bottom) - GetRValue(top)) * y / height;
    const int green = GetGValue(top) + (GetGValue(bottom) - GetGValue(top)) * y / height;
    const int blue = GetBValue(top) + (GetBValue(bottom) - GetBValue(top)) * y / height;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(red, green, blue));
    HPEN oldPen = static_cast<HPEN>(SelectObject(dc, pen));
    MoveToEx(dc, rect.left, rect.top + y, nullptr);
    LineTo(dc, rect.right, rect.top + y);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
  }
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
  const auto logFolder = ModuleFolder() / "data" / "logs";
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
  SendUiLog(text);
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

void PostValidationFailed(HWND hwnd) {
  PostMessageW(hwnd, kValidationFailedMessage, 0, 0);
}

std::wstring PathToDisplay(const std::filesystem::path& path) {
  return path.wstring();
}

std::filesystem::path ExeFolder() {
  return ModuleFolder();
}

std::filesystem::path DataFolder() {
  return ExeFolder() / "data";
}

std::filesystem::path ArchiveFolder() {
  return DataFolder() / "downloads";
}

std::filesystem::path PackageFolder() {
  return DataFolder() / "package";
}

std::filesystem::path UiFolder() {
  return DataFolder() / "ui";
}

std::filesystem::path ManifestPath() {
  return PackageFolder() / "manifest.json";
}

bool PackageManifestFileExists() {
  std::error_code ec;
  return std::filesystem::exists(ManifestPath(), ec) && !ec;
}

void WriteLastSevenZipLog(const std::string& output) {
  if (output.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(DataFolder(), ec);
  std::ofstream log(DataFolder() / "last-7z-output.log", std::ios::binary);
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
  HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                         x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
  SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
  return edit;
}

HWND CreateCombo(HWND parent, int id, int x, int y, int width, int height) {
  return CreateWindowExW(0, L"COMBOBOX", L"",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                             CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL,
                         x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
}

std::wstring ComboText(HWND combo) {
  const auto index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (index == CB_ERR) {
    return {};
  }
  const auto length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0));
  if (length <= 0) {
    return {};
  }
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text.data()));
  text.resize(static_cast<size_t>(length));
  return text;
}

std::filesystem::path SelectedUnpackFolder() {
  auto drive = ComboText(g_unpackDriveCombo);
  if (drive.empty()) {
    return {};
  }
  if (!drive.ends_with(L"\\")) {
    drive += L"\\";
  }
  return std::filesystem::path(drive) / kUnpackFolderName;
}

void UpdateUnpackTargetLabel() {
  if (g_unpackTargetLabel == nullptr) {
    return;
  }
  const auto folder = SelectedUnpackFolder();
  if (folder.empty()) {
    const auto emptyText = UiText("unpack_target_empty", L"Выберите диск");
    SetText(g_unpackTargetLabel, emptyText);
    SendUiPath(L"unpackTarget", emptyText);
  } else {
    SetText(g_unpackTargetLabel, folder.wstring());
    SendUiPath(L"unpackTarget", folder.wstring());
  }
  if (g_mainWindow != nullptr) {
    const std::wstring drive = ComboText(g_unpackDriveCombo);
    const auto chooseText = UiText("button_choose", L"Выберите");
    SetWindowTextW(GetDlgItem(g_mainWindow, kDrivePickerButton),
                   drive.empty() ? chooseText.c_str() : drive.c_str());
  }
}

void PopulateDriveCombo() {
  if (g_unpackDriveCombo == nullptr) {
    return;
  }
  wchar_t drives[512]{};
  const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(sizeof(drives) / sizeof(drives[0])), drives);
  int selected = -1;
  int index = 0;
  for (const wchar_t* drive = drives; length > 0 && *drive != L'\0'; drive += wcslen(drive) + 1) {
    const UINT type = GetDriveTypeW(drive);
    if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) {
      continue;
    }
    std::wstring item = drive;
    SendMessageW(g_unpackDriveCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    if ((item.size() >= 2 && towupper(item[0]) == L'D') || selected < 0) {
      selected = index;
    }
    ++index;
  }
  if (selected >= 0) {
    SendMessageW(g_unpackDriveCombo, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
  }
  UpdateUnpackTargetLabel();
}

struct DrivePopupData {
  std::vector<std::wstring> drives;
  int selectedIndex = -1;
  int hotIndex = -1;
  int result = -1;
};

int DrivePopupIndexAt(HWND hwnd, const DrivePopupData& data, LPARAM lParam) {
  const int x = static_cast<short>(LOWORD(lParam));
  const int y = static_cast<short>(HIWORD(lParam));
  RECT client{};
  GetClientRect(hwnd, &client);
  if (x < 0 || y < 0 || x >= client.right || y >= client.bottom) {
    return -1;
  }
  const float dipY = static_cast<float>(y) * 96.0f /
                     static_cast<float>(GetDpiForWindow(hwnd));
  const int index = static_cast<int>((dipY - 1.0f) /
                                     g_nativeView.Theme().controlHeight);
  return index >= 0 && index < static_cast<int>(data.drives.size()) ? index : -1;
}

LRESULT CALLBACK DrivePopupProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* data = reinterpret_cast<DrivePopupData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    data = static_cast<DrivePopupData*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
  }
  switch (message) {
    case WM_CREATE:
      SetCapture(hwnd);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(hwnd, &paint);
      if (data != nullptr) {
        g_nativeView.PaintDrivePopup(hwnd, data->drives, data->selectedIndex,
                                     data->hotIndex);
      }
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_MOUSEMOVE:
      if (data != nullptr) {
        const int hotIndex = DrivePopupIndexAt(hwnd, *data, lParam);
        if (hotIndex != data->hotIndex) {
          data->hotIndex = hotIndex;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
      }
      return 0;
    case WM_LBUTTONUP:
      if (data != nullptr) {
        data->result = DrivePopupIndexAt(hwnd, *data, lParam);
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_KEYDOWN:
      if (data == nullptr || data->drives.empty()) {
        break;
      }
      if (wParam == VK_ESCAPE) {
        DestroyWindow(hwnd);
        return 0;
      }
      if (wParam == VK_RETURN || wParam == VK_SPACE) {
        data->result = data->hotIndex;
        DestroyWindow(hwnd);
        return 0;
      }
      if (wParam == VK_UP || wParam == VK_DOWN) {
        const int direction = wParam == VK_UP ? -1 : 1;
        const int count = static_cast<int>(data->drives.size());
        data->hotIndex = (data->hotIndex + direction + count) % count;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      if (suggested != nullptr) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (reinterpret_cast<HWND>(lParam) != hwnd && IsWindow(hwnd)) {
        DestroyWindow(hwnd);
      }
      return 0;
    case WM_NCDESTROY:
      if (GetCapture() == hwnd) {
        ReleaseCapture();
      }
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

void ShowDriveMenu(HWND owner) {
  const int count = static_cast<int>(SendMessageW(g_unpackDriveCombo, CB_GETCOUNT, 0, 0));
  if (count <= 0) {
    SendUiError(UiText("drive_unavailable_title", L"Диск недоступен"),
                UiText("drive_unavailable_message", L"Не найден доступный диск для распаковки."));
    return;
  }
  DrivePopupData data;
  data.selectedIndex = static_cast<int>(SendMessageW(g_unpackDriveCombo, CB_GETCURSEL, 0, 0));
  data.hotIndex = data.selectedIndex >= 0 ? data.selectedIndex : 0;
  for (int i = 0; i < count; ++i) {
    const int length = static_cast<int>(SendMessageW(g_unpackDriveCombo, CB_GETLBTEXTLEN, i, 0));
    if (length <= 0) {
      continue;
    }
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    SendMessageW(g_unpackDriveCombo, CB_GETLBTEXT, i, reinterpret_cast<LPARAM>(value.data()));
    value.resize(static_cast<size_t>(length));
    data.drives.push_back(std::move(value));
  }
  if (data.drives.empty()) {
    return;
  }

  constexpr const wchar_t* className = L"ModlistInstallerDrivePopup";
  WNDCLASSW windowClass{};
  windowClass.style = CS_DROPSHADOW;
  windowClass.lpfnWndProc = DrivePopupProc;
  windowClass.hInstance = g_instance;
  windowClass.lpszClassName = className;
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  RegisterClassW(&windowClass);

  HWND buttonWindow = GetDlgItem(owner, kDrivePickerButton);
  RECT button{};
  GetWindowRect(buttonWindow, &button);
  const UINT dpi = GetDpiForWindow(buttonWindow);
  const int popupWidth = button.right - button.left;
  const int popupHeight = MulDiv(
      static_cast<int>(std::lround(data.drives.size() *
                                   g_nativeView.Theme().controlHeight + 2.0f)),
      static_cast<int>(dpi), 96);
  MONITORINFO monitorInfo{};
  monitorInfo.cbSize = sizeof(monitorInfo);
  GetMonitorInfoW(MonitorFromRect(&button, MONITOR_DEFAULTTONEAREST), &monitorInfo);
  const int workLeft = static_cast<int>(monitorInfo.rcWork.left);
  const int workTop = static_cast<int>(monitorInfo.rcWork.top);
  const int workRight = static_cast<int>(monitorInfo.rcWork.right);
  const int workBottom = static_cast<int>(monitorInfo.rcWork.bottom);
  int x = std::clamp(static_cast<int>(button.left), workLeft,
                     std::max(workLeft, workRight - popupWidth));
  int y = button.bottom;
  if (y + popupHeight > workBottom) {
    y = button.top - popupHeight;
  }
  y = std::clamp(y, workTop, std::max(workTop, workBottom - popupHeight));

  HWND popup = CreateWindowExW(WS_EX_TOOLWINDOW, className, L"",
                               WS_POPUP, x, y, popupWidth, popupHeight,
                               owner, nullptr, g_instance, &data);
  if (popup == nullptr) {
    return;
  }
  ShowWindow(popup, SW_SHOWNORMAL);
  SetForegroundWindow(popup);
  SetFocus(popup);

  MSG popupMessage{};
  while (IsWindow(popup)) {
    const BOOL messageResult = GetMessageW(&popupMessage, nullptr, 0, 0);
    if (messageResult <= 0) {
      if (messageResult == 0) {
        PostQuitMessage(static_cast<int>(popupMessage.wParam));
      }
      break;
    }
    TranslateMessage(&popupMessage);
    DispatchMessageW(&popupMessage);
  }
  SetFocus(buttonWindow);
  if (data.result >= 0 && data.result < count) {
    SendMessageW(g_unpackDriveCombo, CB_SETCURSEL, data.result, 0);
    UpdateUnpackTargetLabel();
  }
}

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData) {
  (void)subclassId;
  (void)refData;
  switch (message) {
    case WM_MOUSEMOVE: {
      if (g_hotButton != hwnd) {
        if (g_hotButton != nullptr) {
          InvalidateRect(g_hotButton, nullptr, FALSE);
        }
        g_hotButton = hwnd;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      TRACKMOUSEEVENT track{};
      track.cbSize = sizeof(track);
      track.dwFlags = TME_LEAVE;
      track.hwndTrack = hwnd;
      TrackMouseEvent(&track);
      break;
    }
    case WM_MOUSELEAVE:
      if (g_hotButton == hwnd) {
        g_hotButton = nullptr;
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      InvalidateRect(hwnd, nullptr, FALSE);
      break;
    case WM_NCDESTROY:
      if (g_hotButton == hwnd) {
        g_hotButton = nullptr;
      }
      RemoveWindowSubclass(hwnd, ButtonSubclassProc, 1);
      break;
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

HWND CreateButton(HWND parent, int id, const wchar_t* text, int x, int y, int width, int height) {
  HWND button = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                x, y, width, height, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                g_instance, nullptr);
  SetWindowSubclass(button, ButtonSubclassProc, 1, 0);
  return button;
}

HFONT CreateUiFont(int pointSize, UINT dpi, int weight = FW_NORMAL,
                   const wchar_t* family = L"Segoe UI") {
  const int height = -MulDiv(pointSize, static_cast<int>(dpi), 72);
  return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
}

void SetControlFont(HWND hwnd, HFONT font) {
  if (hwnd != nullptr && font != nullptr) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
  }
}

void PaintInstallerChrome(HWND hwnd, HDC dc) {
  RECT rect{};
  GetClientRect(hwnd, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  constexpr int railWidth = 116;
  constexpr int headerHeight = 66;
  constexpr int footerHeight = 66;

  RECT content{0, 0, width, height};
  FillRect(dc, &content, g_contentBrush);

  RECT panel{railWidth, 0, width, height - footerHeight};
  FillRect(dc, &panel, g_panelBrush);

  RECT header{railWidth, 0, width, headerHeight};
  FillVerticalGradient(dc, header, RGB(31, 38, 42), kHeaderColor);

  RECT rail{0, 0, railWidth, height - footerHeight};
  HBRUSH railBrush = CreateSolidBrush(kRailColor);
  FillRect(dc, &rail, railBrush);
  DeleteObject(railBrush);

  RECT railDark{0, 0, 12, height - footerHeight};
  HBRUSH railDarkBrush = CreateSolidBrush(kRailDarkColor);
  FillRect(dc, &railDark, railDarkBrush);
  DeleteObject(railDarkBrush);

  RECT footer{0, height - footerHeight, width, height};
  FillVerticalGradient(dc, footer, kFooterColor, kPanelColor);

  HPEN linePen = CreatePen(PS_SOLID, 1, kLineColor);
  HPEN oldPen = static_cast<HPEN>(SelectObject(dc, linePen));
  MoveToEx(dc, railWidth, headerHeight, nullptr);
  LineTo(dc, width, headerHeight);
  MoveToEx(dc, 0, height - footerHeight, nullptr);
  LineTo(dc, width, height - footerHeight);
  MoveToEx(dc, railWidth, 0, nullptr);
  LineTo(dc, railWidth, height - footerHeight);
  SelectObject(dc, oldPen);
  DeleteObject(linePen);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, kMutedTextColor);
  HFONT oldFont = static_cast<HFONT>(SelectObject(dc, g_labelFont));
  RECT railText{18, 26, railWidth - 12, 88};
  DrawTextW(dc, L"MODLIST", -1, &railText, DT_LEFT | DT_TOP | DT_SINGLELINE);
  SelectObject(dc, g_titleFont);
  RECT railMark{18, 74, railWidth - 12, 132};
  DrawTextW(dc, L"III", -1, &railMark, DT_LEFT | DT_TOP | DT_SINGLELINE);
  SelectObject(dc, oldFont);
}

void DrawNsisButton(const DRAWITEMSTRUCT& item) {
  const int controlId = GetDlgCtrlID(item.hwndItem);
  modlist::NativeButtonStyle style = modlist::NativeButtonStyle::Normal;
  if (controlId == kDrivePickerButton) {
    style = modlist::NativeButtonStyle::Input;
  } else if (controlId == kStartButton || controlId == kNextButton ||
             controlId == IDOK || controlId == IDYES) {
    style = modlist::NativeButtonStyle::Primary;
  } else if (controlId == kStopButton) {
    style = modlist::NativeButtonStyle::Danger;
  }
  g_nativeView.DrawButton(item, style, item.hwndItem == g_hotButton);
}

void RecreateUiFonts(HWND hwnd) {
  const UINT dpi = GetDpiForWindow(hwnd);
  const int bodyPointSize = std::max(
      8, static_cast<int>(std::lround(g_nativeView.Theme().fontSize * 0.75f)));
  HFONT stepFont = CreateUiFont(10, dpi, FW_SEMIBOLD);
  HFONT titleFont = CreateUiFont(22, dpi, FW_NORMAL, L"Georgia");
  HFONT bodyFont = CreateUiFont(bodyPointSize, dpi, FW_NORMAL,
                                g_nativeView.Theme().fontFamily.c_str());
  HFONT labelFont = CreateUiFont(9, dpi, FW_SEMIBOLD);

  SetControlFont(g_stepLabel, stepFont);
  SetControlFont(g_welcomeTitle, titleFont);
  SetControlFont(g_welcomeBody, bodyFont);
  SetControlFont(g_downloadLabel, labelFont);
  SetControlFont(g_unpackDriveLabel, labelFont);
  SetControlFont(g_unpackTargetLabel, bodyFont);
  SetControlFont(g_installLabel, labelFont);
  SetControlFont(g_downloadEdit, bodyFont);
  SetControlFont(g_unpackDriveCombo, bodyFont);
  SetControlFont(GetDlgItem(hwnd, kDrivePickerButton), bodyFont);
  SetControlFont(g_installEdit, bodyFont);
  SetControlFont(g_statusLabel, bodyFont);
  SetControlFont(g_logEdit, bodyFont);
  SetControlFont(GetDlgItem(hwnd, kDownloadBrowse), bodyFont);
  SetControlFont(GetDlgItem(hwnd, kInstallBrowse), bodyFont);
  SetControlFont(GetDlgItem(hwnd, kValidateButton), bodyFont);
  SetControlFont(GetDlgItem(hwnd, kStartButton), bodyFont);
  SetControlFont(GetDlgItem(hwnd, kUnpackButton), bodyFont);
  SetControlFont(GetDlgItem(hwnd, kPauseButton), bodyFont);
  SetControlFont(GetDlgItem(hwnd, kStopButton), bodyFont);
  SetControlFont(GetDlgItem(hwnd, kOpenLogButton), bodyFont);
  SetControlFont(g_previousButton, bodyFont);
  SetControlFont(g_nextButton, bodyFont);

  DeleteObject(g_stepFont);
  DeleteObject(g_titleFont);
  DeleteObject(g_bodyFont);
  DeleteObject(g_labelFont);
  g_stepFont = stepFont;
  g_titleFont = titleFont;
  g_bodyFont = bodyFont;
  g_labelFont = labelFont;
}

struct ThemedDialogData {
  std::wstring title;
  std::wstring message;
  bool confirmation = false;
  int result = IDCANCEL;
};

LRESULT CALLBACK ThemedDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  auto* data = reinterpret_cast<ThemedDialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
    data = static_cast<ThemedDialogData*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
  }
  switch (message) {
    case WM_CREATE:
      ApplyWindowFrameTheme(hwnd);
      if (data != nullptr && data->confirmation) {
        CreateButton(hwnd, IDYES, UiText("button_yes", L"Да").c_str(), 0, 0, 96, 32);
        CreateButton(hwnd, IDNO, UiText("button_no", L"Нет").c_str(), 0, 0, 96, 32);
      } else {
        CreateButton(hwnd, IDOK, UiText("button_ok", L"OK").c_str(), 0, 0, 96, 32);
      }
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(hwnd, &paint);
      if (data != nullptr) {
        g_nativeView.PaintDialog(hwnd, data->title, data->message);
      }
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_SIZE: {
      const int width = LOWORD(lParam);
      const int height = HIWORD(lParam);
      const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
      const int buttonWidth = MulDiv(96, dpi, 96);
      const int buttonHeight = MulDiv(32, dpi, 96);
      const int bottom = MulDiv(13, dpi, 96);
      const int gap = MulDiv(8, dpi, 96);
      const int y = height - bottom - buttonHeight;
      if (data != nullptr && data->confirmation) {
        const int groupWidth = buttonWidth * 2 + gap;
        const int x = (width - groupWidth) / 2;
        MoveWindow(GetDlgItem(hwnd, IDNO), x, y, buttonWidth, buttonHeight, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDYES), x + buttonWidth + gap, y,
                   buttonWidth, buttonHeight, TRUE);
      } else {
        MoveWindow(GetDlgItem(hwnd, IDOK), (width - buttonWidth) / 2, y,
                   buttonWidth, buttonHeight, TRUE);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_DPICHANGED: {
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      if (suggested != nullptr) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
      }
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (item != nullptr && item->CtlType == ODT_BUTTON) {
        DrawNsisButton(*item);
        return TRUE;
      }
      break;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == IDOK || id == IDYES || id == IDNO) {
        if (data != nullptr) {
          data->result = id;
        }
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    case WM_KEYDOWN:
      if (wParam == VK_ESCAPE) {
        if (data != nullptr) {
          data->result = data->confirmation ? IDNO : IDOK;
        }
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    case WM_CLOSE:
      if (data != nullptr) {
        data->result = data->confirmation ? IDNO : IDOK;
      }
      DestroyWindow(hwnd);
      return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

int ShowThemedMessage(HWND owner,
                      const std::wstring& title,
                      const std::wstring& message,
                      bool confirmation = false) {
  static bool registered = false;
  constexpr wchar_t className[] = L"ModlistInstallerThemedDialog";
  if (!registered) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = ThemedDialogProc;
    windowClass.hInstance = g_instance;
    windowClass.lpszClassName = className;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_MODLIST_INSTALLER));
    if (RegisterClassW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return confirmation ? IDNO : IDOK;
    }
    registered = true;
  }

  ThemedDialogData data;
  data.title = title;
  data.message = message;
  data.confirmation = confirmation;
  data.result = confirmation ? IDNO : IDOK;

  const size_t explicitLines = static_cast<size_t>(std::count(message.begin(), message.end(), L'\n')) + 1;
  const size_t wrappedLines = std::max<size_t>(1, message.size() / 54);
  const int heightDip = std::clamp(210 + static_cast<int>(std::max(explicitLines, wrappedLines)) * 18,
                                   250, 430);
  const UINT dpi = owner != nullptr ? GetDpiForWindow(owner) : GetDpiForSystem();
  RECT windowRect{0, 0, MulDiv(480, static_cast<int>(dpi), 96),
                  MulDiv(heightDip, static_cast<int>(dpi), 96)};
  constexpr DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
  AdjustWindowRectExForDpi(&windowRect, style, FALSE, WS_EX_DLGMODALFRAME, dpi);
  int x = CW_USEDEFAULT;
  int y = CW_USEDEFAULT;
  if (owner != nullptr) {
    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    x = ownerRect.left + ((ownerRect.right - ownerRect.left) -
                          (windowRect.right - windowRect.left)) / 2;
    y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) -
                         (windowRect.bottom - windowRect.top)) / 2;
  }
  HWND dialog = CreateWindowExW(
      WS_EX_DLGMODALFRAME, className, title.c_str(), style,
      x, y, windowRect.right - windowRect.left, windowRect.bottom - windowRect.top,
      owner, nullptr, g_instance, &data);
  if (dialog == nullptr) {
    return data.result;
  }
  if (owner != nullptr) {
    EnableWindow(owner, FALSE);
  }
  ShowWindow(dialog, SW_SHOW);
  UpdateWindow(dialog);

  MSG messageData{};
  while (IsWindow(dialog)) {
    const BOOL messageResult = GetMessageW(&messageData, nullptr, 0, 0);
    if (messageResult <= 0) {
      if (messageResult == 0) {
        PostQuitMessage(static_cast<int>(messageData.wParam));
      }
      break;
    }
    if (!IsDialogMessageW(dialog, &messageData)) {
      TranslateMessage(&messageData);
      DispatchMessageW(&messageData);
    }
  }
  if (owner != nullptr && IsWindow(owner)) {
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
  }
  return data.result;
}

void ShowControl(HWND hwnd, bool visible) {
  if (hwnd != nullptr) {
    ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
  }
}

void ShowControl(HWND parent, int id, bool visible) {
  ShowControl(GetDlgItem(parent, id), visible);
}

void HideNativeControls(HWND hwnd) {
  ShowControl(g_stepLabel, false);
  ShowControl(g_welcomeTitle, false);
  ShowControl(g_welcomeBody, false);
  ShowControl(g_downloadLabel, false);
  ShowControl(g_unpackDriveLabel, false);
  ShowControl(g_unpackTargetLabel, false);
  ShowControl(g_installLabel, false);
  ShowControl(g_downloadEdit, false);
  ShowControl(g_unpackDriveCombo, false);
  ShowControl(g_installEdit, false);
  ShowControl(g_logEdit, false);
  ShowControl(g_progress, false);
  ShowControl(g_statusLabel, false);
  ShowControl(g_previousButton, false);
  ShowControl(g_nextButton, false);
  ShowControl(hwnd, kDownloadBrowse, false);
  ShowControl(hwnd, kInstallBrowse, false);
  ShowControl(hwnd, kValidateButton, false);
  ShowControl(hwnd, kStartButton, false);
  ShowControl(hwnd, kUnpackButton, false);
  ShowControl(hwnd, kPauseButton, false);
  ShowControl(hwnd, kStopButton, false);
}

std::wstring WizardPageTitle(WizardPage page) {
  (void)page;
  return L"";
}

std::wstring UiStepForPage(WizardPage page) {
  switch (page) {
    case WizardPage::Welcome:
      return L"Добро пожаловать";
    case WizardPage::Folders:
      return L"Путь установки";
    case WizardPage::Activity:
      return g_workerRunning.load() ? L"Установка" : L"Проверка файлов";
  }
  return L"Добро пожаловать";
}

std::vector<std::wstring> AvailableDrives() {
  std::vector<std::wstring> drives;
  wchar_t buffer[512]{};
  const DWORD length = GetLogicalDriveStringsW(static_cast<DWORD>(std::size(buffer)), buffer);
  for (const wchar_t* drive = buffer; length > 0 && *drive != L'\0'; drive += wcslen(drive) + 1) {
    const UINT type = GetDriveTypeW(drive);
    if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
      drives.emplace_back(drive);
    }
  }
  return drives;
}

std::wstring JsonArray(const std::vector<std::wstring>& values) {
  std::wostringstream out;
  out << L"[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) {
      out << L",";
    }
    out << JsonString(values[i]);
  }
  out << L"]";
  return out.str();
}

std::filesystem::path FinalInstallFolder(const std::filesystem::path& selectedRoot) {
  if (selectedRoot.empty() || g_archiveFolderName.empty()) {
    return {};
  }
  auto result = modlist::ResolveArchiveInstallFolder(
      selectedRoot, Narrow(g_archiveFolderName));
  return result.ok() ? result.value() : std::filesystem::path{};
}

bool IsWindowEnabledSafe(HWND hwnd) {
  return hwnd != nullptr && IsWindowEnabled(hwnd) != FALSE;
}

void InvalidateDipArea(const D2D1_RECT_F& dipArea) {
  if (g_mainWindow == nullptr) {
    return;
  }
  RECT area = g_nativeView.ToPixels(g_mainWindow, dipArea);
  const int padding = MulDiv(2, static_cast<int>(GetDpiForWindow(g_mainWindow)), 96);
  InflateRect(&area, padding, padding);
  InvalidateRect(g_mainWindow, &area, FALSE);
}

void InvalidateActivityArea() {
  if (g_mainWindow == nullptr) {
    return;
  }
  const auto layout = g_nativeView.CalculateLayout(g_mainWindow);
  D2D1_RECT_F area = layout.statusArea;
  area.bottom = layout.progressBar.bottom;
  InvalidateDipArea(area);
}

void SendUiProgress(int percent, const std::wstring& status) {
  const int nextProgress = std::clamp(percent, 0, 100);
  bool changed = nextProgress != g_progressPercent;
  g_progressPercent = nextProgress;
  if (!status.empty() && status != g_statusText) {
    g_statusText = status;
    changed = true;
  }
  if (changed) {
    InvalidateActivityArea();
  }
}

void SendUiStatus(const std::wstring& status) {
  if (status == g_statusText) {
    return;
  }
  g_statusText = status;
  InvalidateActivityArea();
}

void SendUiLog(const std::wstring& message) {
  (void)message;
}

void SendUiStep(const std::wstring& stepName) {
  (void)stepName;
}

void SendUiPath(const std::wstring& fieldName, const std::wstring& path) {
  (void)path;
  if (g_mainWindow != nullptr) {
    if (fieldName == L"installFolder") {
      InvalidateDipArea(g_nativeView.CalculateLayout(g_mainWindow).finalPath);
    } else {
      InvalidateRect(g_mainWindow, nullptr, FALSE);
    }
  }
}

void SendUiOption(const std::wstring& optionName, bool value) {
  (void)optionName;
  (void)value;
}

void SendUiError(const std::wstring& title, const std::wstring& message) {
  ShowThemedMessage(g_mainWindow, title, message);
}

void SendUiCleanupConfirm(const PendingFolderCleanup& folders) {
  std::wstring message = UiText(
      "cleanup_intro", L"После предыдущей неудачной или остановленной установки остались файлы:");
  if (!folders.unpackFolder.empty()) {
    message += L"\n\n" + UiText("cleanup_unpack_folder", L"Папка распаковки:") +
               L"\n" + PathToDisplay(folders.unpackFolder);
  }
  if (!folders.installFolder.empty()) {
    message += L"\n\n" + UiText("cleanup_install_folder", L"Папка установки:") +
               L"\n" + PathToDisplay(folders.installFolder);
  }
  message += L"\n\n" + UiText("cleanup_question", L"Очистить указанные папки и продолжить установку?");
  if (ShowThemedMessage(g_mainWindow, UiText("cleanup_title", L"Очистить папки?"),
                        message, true) == IDYES) {
    ConfirmPendingFolderCleanup(g_mainWindow);
  } else {
    CancelPendingFolderCleanup();
  }
}

void SendUiButtonEnabled(const std::wstring& buttonName, bool enabled) {
  (void)buttonName;
  (void)enabled;
}

void SendUiState() {
  if (g_mainWindow != nullptr) {
    InvalidateRect(g_mainWindow, nullptr, FALSE);
  }
}

void Layout(HWND hwnd) {
  const auto layout = g_nativeView.CalculateLayout(hwnd);
  const auto move = [hwnd](HWND control, const D2D1_RECT_F& dipRect) {
    if (control == nullptr) {
      return;
    }
    const RECT rect = g_nativeView.ToPixels(hwnd, dipRect);
    MoveWindow(control, rect.left, rect.top, rect.right - rect.left,
               rect.bottom - rect.top, TRUE);
  };
  move(GetDlgItem(hwnd, kDrivePickerButton), layout.driveCombo);
  move(g_installEdit, layout.installEdit);
  move(GetDlgItem(hwnd, kInstallBrowse), layout.browseButton);
  move(g_logEdit, layout.logEdit);
  move(GetDlgItem(hwnd, kOpenLogButton), layout.openLogButton);
  move(GetDlgItem(hwnd, kStartButton), layout.startButton);
  move(GetDlgItem(hwnd, kStopButton), layout.stopButton);
}

std::wstring FormatBytes(uintmax_t bytes);
std::wstring FormatBytesPerSecond(uintmax_t bytesPerSecond);
std::wstring FormatEta(int seconds);
std::string HexDigest(const std::array<uint8_t, 32>& digest);
size_t SelectHashWorkerCount(const std::filesystem::path& folder, size_t fileCount);
std::optional<std::filesystem::path> FindFirstArchivePart(const std::filesystem::path& folder);
std::optional<modlist::Manifest> LoadPackageManifest(std::wstring& message);
std::optional<std::filesystem::path> ArchivePartFromManifest(const modlist::Manifest& manifest);

modlist::Result<modlist::PackageDiscovery> ReadPackageFromUi() {
  auto package = modlist::DiscoverPackageNear(PackageFolder());
  if (package.ok() && !package.value().firstArchivePart.has_value()) {
    package.value().firstArchivePart = FindFirstArchivePart(ArchiveFolder());
  }
  if (package.ok() && !package.value().firstArchivePart.has_value()) {
    std::wstring message;
    auto manifest = LoadPackageManifest(message);
    if (manifest.has_value()) {
      package.value().firstArchivePart = ArchivePartFromManifest(*manifest);
    }
  }
  return package;
}

std::optional<modlist::Manifest> LoadPackageManifest(std::wstring& message) {
  std::error_code ec;
  const auto path = ManifestPath();
  if (!std::filesystem::exists(path, ec) || ec) {
    message = L"Файл проверки не обнаружен";
    return std::nullopt;
  }

  modlist::ManifestLoader loader;
  auto manifest = loader.LoadFromFile(path);
  if (!manifest.ok()) {
    message = L"Файл проверки: " + Widen(manifest.error());
    return std::nullopt;
  }

  message = L"Файл проверки загружен: " + PathToDisplay(path);
  return std::move(manifest.value());
}

uintmax_t ManifestRequiredBytes(const modlist::Manifest& manifest) {
  uintmax_t total = 0;
  for (const auto& file : manifest.files) {
    total += file.size;
  }
  return total;
}

std::optional<std::filesystem::path> ArchivePartFromManifest(const modlist::Manifest& manifest) {
  if (manifest.extract.firstArchivePart.empty()) {
    return std::nullopt;
  }
  return ArchiveFolder() / manifest.extract.firstArchivePart;
}

bool VerifyPackageManifest(HWND hwnd, const modlist::Manifest& manifest) {
  PostStatus(hwnd, L"Verifying manifest SHA-256");
  PostLog(hwnd, L"Проверка SHA-256 для " + std::to_wstring(manifest.files.size()) + L" архива(ов)...");

  const uintmax_t totalBytes = ManifestRequiredBytes(manifest);
  std::atomic<uintmax_t> doneBytes{0};
  std::atomic_bool failed{false};
  const auto startedAt = std::chrono::steady_clock::now();
  std::mutex jobsMutex;
  std::deque<modlist::ManifestFile> jobs;
  for (const auto& file : manifest.files) {
    jobs.push_back(file);
  }

  auto reportFailure = [&](std::wstring message) {
    bool expected = false;
    if (failed.compare_exchange_strong(expected, true)) {
      PostLog(hwnd, std::move(message));
      PostValidationFailed(hwnd);
    }
  };

  auto postProgress = [&]() {
    const uintmax_t done = doneBytes.load();
    const int percent = totalBytes > 0 ? static_cast<int>((done * 100) / totalBytes) : 0;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    uintmax_t speed = 0;
    int eta = -1;
    if (elapsed > 0 && done > 0) {
      speed = done / static_cast<uintmax_t>(elapsed);
      if (speed > 0 && totalBytes > done) {
        eta = static_cast<int>((totalBytes - done) / speed);
      }
    }
    PostProgress(hwnd, (percent * 35) / 100);
    PostStatus(hwnd,
               L"Проверка " + std::to_wstring(percent) + L"% | " +
                   FormatBytes(done) + L" / " + FormatBytes(totalBytes) +
                   L" | " + FormatBytesPerSecond(speed) +
                   L" | Осталось: " + FormatEta(eta) +
                   L" | Прошло " + FormatEta(static_cast<int>(elapsed)));
  };

  const size_t workerCount = SelectHashWorkerCount(ArchiveFolder(), manifest.files.size());
  PostLog(hwnd, workerCount == 1
                    ? L"Manifest validation workers: 1 (sequential HDD-friendly read)"
                    : L"Manifest validation workers: " + std::to_wstring(workerCount) + L" (SSD parallel read)");

  auto worker = [&]() {
    std::vector<uint8_t> buffer(4 * 1024 * 1024);
    while (!g_stopRequested.load() && !failed.load()) {
      modlist::ManifestFile expected;
      {
        std::lock_guard<std::mutex> lock(jobsMutex);
        if (jobs.empty()) {
          return;
        }
        expected = jobs.front();
        jobs.pop_front();
      }

      if (!modlist::IsSafeManifestRelativePath(expected.path)) {
        reportFailure(L"Ошибка проверки для " + PathToDisplay(expected.path) + L": небезопасный путь");
        return;
      }

      const auto fullPath = ArchiveFolder() / expected.path;
      std::error_code ec;
      if (!std::filesystem::exists(fullPath, ec) || !std::filesystem::is_regular_file(fullPath, ec)) {
        reportFailure(L"Ошибка проверки для " + PathToDisplay(expected.path) + L": не хватает файла");
        return;
      }
      const auto actualSize = std::filesystem::file_size(fullPath, ec);
      if (ec || actualSize != expected.size) {
        reportFailure(L"Ошибка проверки для " + PathToDisplay(expected.path) + L": не совпадает размер");
        return;
      }

      PostLog(hwnd, L"Проверка " + PathToDisplay(expected.path));
      std::ifstream input(fullPath, std::ios::binary);
      if (!input) {
        reportFailure(L"Ошибка проверки для " + PathToDisplay(expected.path) + L": невозможно открыть файл");
        return;
      }

      modlist::Sha256 sha;
      while (input && !g_stopRequested.load() && !failed.load()) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read <= 0) {
          break;
        }
        sha.Update(buffer.data(), static_cast<size_t>(read));
        doneBytes += static_cast<uintmax_t>(read);
        postProgress();
      }
      if (g_stopRequested.load() || failed.load()) {
        return;
      }

      const auto actualHash = HexDigest(sha.Final());
      if (actualHash != expected.sha256) {
        reportFailure(L"Ошибка проверки для " + PathToDisplay(expected.path) + L": не совпадает SHA256");
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

  if (g_stopRequested.load()) {
    PostLog(hwnd, L"Проверка остановлена");
    return false;
  }
  if (failed.load()) {
    return false;
  }

  PostProgress(hwnd, 35);
  PostLog(hwnd, L"Проверка выполнена.");
  return true;
}

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
    if (name.ends_with(L".7z.001") || name.ends_with(L".zip.001") || name.ends_with(L".7z") || name.ends_with(L".zip")) {
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
  return name.find(L".7z.") != std::wstring::npos ||
         name.find(L".zip.") != std::wstring::npos ||
         name.ends_with(L".7z") ||
         name.ends_with(L".zip");
}

bool IsFirstSplitArchivePart(const std::filesystem::path& path) {
  std::wstring name = path.filename().wstring();
  for (auto& ch : name) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  return name.ends_with(L".001");
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

struct SpaceRequirements {
  uintmax_t archiveBytes{0};
  uintmax_t unpackedBytes{0};
  bool unpackedSizeKnown{false};
};

SpaceRequirements SpaceRequirementsFromManifest(const modlist::Manifest& manifest) {
  SpaceRequirements requirements;
  requirements.archiveBytes = ManifestRequiredBytes(manifest);
  requirements.unpackedBytes = manifest.unpackedSize > 0 ? manifest.unpackedSize : requirements.archiveBytes;
  requirements.unpackedSizeKnown = manifest.unpackedSize > 0;
  return requirements;
}

SpaceRequirements EstimateSpaceRequirements(const modlist::PackageDiscovery& package) {
  (void)package;
  std::wstring manifestMessage;
  auto manifest = LoadPackageManifest(manifestMessage);
  if (manifest.has_value()) {
    const auto requirements = SpaceRequirementsFromManifest(*manifest);
    AppendLog(manifestMessage);
    AppendLog(L"Размер архива по файлу проверки: " + FormatBytes(requirements.archiveBytes));
    if (requirements.unpackedSizeKnown) {
      AppendLog(L"Размер распакованной сборки: " + FormatBytes(requirements.unpackedBytes));
    } else {
      AppendLog(L"Размер распакованной сборки отсутствует в старом manifest; используется размер архива.");
    }
    return requirements;
  }

  SpaceRequirements requirements;
  requirements.archiveBytes = EstimateNearbyArchiveBytes(ArchiveFolder());
  requirements.unpackedBytes = requirements.archiveBytes;
  if (requirements.archiveBytes > 0) {
    AppendLog(L"Расчётный размер найденного архива: " + FormatBytes(requirements.archiveBytes));
  }
  return requirements;
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
  return FormatBytes(static_cast<uintmax_t>(bytesPerSecond)) + L"/s";
}

std::wstring FormatBytesPerSecond(uintmax_t bytesPerSecond) {
  if (bytesPerSecond == 0) {
    return L"0 B/s";
  }
  return FormatBytes(bytesPerSecond) + L"/s";
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

std::string HexDigest(const std::array<uint8_t, 32>& digest) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto byte : digest) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
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

std::wstring FormatEta(int seconds) {
  if (seconds < 0) {
    return L"неизвестно";
  }
  const int hours = seconds / 3600;
  const int minutes = (seconds % 3600) / 60;
  const int secs = seconds % 60;
  std::wostringstream out;
  if (hours > 0) {
    out << hours << L"ч " << minutes << L"м";
  } else if (minutes > 0) {
    out << minutes << L"м " << secs << L"с";
  } else {
    out << secs << L"с";
  }
  return out.str();
}

bool HasEnoughSpace(const std::filesystem::path& folder, uintmax_t requiredBytes, const std::wstring& label) {
  if (requiredBytes == 0) {
    AppendLog(label + L": необходимое место неизвестно, пока файлы сборки недоступны.");
    return true;
  }
  const uintmax_t free = FreeBytes(folder);
  AppendLog(label + L": свободно " + FormatBytes(free) + L"; минимум требуется " + FormatBytes(requiredBytes));
  return free >= requiredBytes;
}

std::optional<std::wstring> EnsureGameDocumentsFolders() {
  PWSTR documentsPath = nullptr;
  const HRESULT result =
      SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &documentsPath);
  if (FAILED(result) || documentsPath == nullptr) {
    if (documentsPath != nullptr) {
      CoTaskMemFree(documentsPath);
    }
    std::wostringstream message;
    message << L"Невозможно определить папку «Документы». Код ошибки: 0x"
            << std::hex << std::uppercase << static_cast<unsigned long>(result);
    return message.str();
  }

  const std::filesystem::path myGames =
      std::filesystem::path(documentsPath) / L"My Games";
  CoTaskMemFree(documentsPath);

  constexpr const wchar_t* gameFolders[] = {
      L"Skyrim Special Edition",
      L"Fallout4",
  };
  for (const auto* gameFolderName : gameFolders) {
    const auto gameFolder = myGames / gameFolderName;
    std::error_code createError;
    std::filesystem::create_directories(gameFolder, createError);
    if (createError) {
      return L"Невозможно создать папку игры:\n" + PathToDisplay(gameFolder) +
             L"\n\n" + Widen(createError.message());
    }

    std::error_code directoryError;
    if (!std::filesystem::is_directory(gameFolder, directoryError) || directoryError) {
      return L"Путь папки игры недоступен или не является папкой:\n" +
             PathToDisplay(gameFolder) +
             (directoryError ? L"\n\n" + Widen(directoryError.message()) : L"");
    }
    AppendLog(L"Папка документов игры готова: " + PathToDisplay(gameFolder));
  }
  return std::nullopt;
}

std::optional<std::wstring> FolderMustBeEmptyError(const std::filesystem::path& folder,
                                                   const std::wstring& label) {
  std::error_code ec;
  if (!std::filesystem::exists(folder, ec)) {
    return ec ? std::optional<std::wstring>(label + L": невозможно проверить папку - " + Widen(ec.message()))
              : std::nullopt;
  }
  if (!std::filesystem::is_directory(folder, ec) || ec) {
    return label + L": путь не является папкой.";
  }
  const bool empty = std::filesystem::is_empty(folder, ec);
  if (ec) {
    return label + L": невозможно проверить содержимое - " + Widen(ec.message());
  }
  if (!empty) {
    return label + L" должна быть пустой. Удалите файлы от предыдущей или незавершённой установки.";
  }
  return std::nullopt;
}

bool IsExistingNonEmptyDirectory(const std::filesystem::path& folder) {
  std::error_code existsEc;
  std::error_code directoryEc;
  std::error_code emptyEc;
  return std::filesystem::exists(folder, existsEc) && !existsEc &&
         std::filesystem::is_directory(folder, directoryEc) && !directoryEc &&
         !std::filesystem::is_empty(folder, emptyEc) && !emptyEc;
}

void ShowWizardPage(HWND hwnd, WizardPage page) {
  (void)page;
  g_page = WizardPage::Activity;
  const bool running = g_workerRunning.load();

  ShowControl(g_stepLabel, false);
  ShowControl(g_welcomeTitle, false);
  ShowControl(g_welcomeBody, false);
  ShowControl(g_downloadLabel, false);
  ShowControl(hwnd, kDownloadEdit, false);
  ShowControl(hwnd, kDownloadBrowse, false);
  ShowControl(g_unpackDriveLabel, false);
  ShowControl(g_unpackDriveCombo, false);
  ShowControl(hwnd, kDrivePickerButton, true);
  ShowControl(g_unpackTargetLabel, false);
  ShowControl(g_installLabel, false);
  ShowControl(hwnd, kInstallEdit, true);
  ShowControl(hwnd, kInstallBrowse, true);
  ShowControl(hwnd, kOpenLogButton, true);
  ShowControl(hwnd, kValidateButton, false);
  ShowControl(hwnd, kStartButton, true);
  ShowControl(hwnd, kUnpackButton, false);
  ShowControl(hwnd, kPauseButton, false);
  ShowControl(hwnd, kStopButton, true);
  ShowControl(g_progress, false);
  ShowControl(g_statusLabel, false);
  ShowControl(g_logEdit, true);
  ShowControl(g_previousButton, false);
  ShowControl(g_nextButton, false);

  EnableWindow(GetDlgItem(hwnd, kDrivePickerButton), !running);
  EnableWindow(GetDlgItem(hwnd, kInstallBrowse), !running);
  EnableWindow(g_installEdit, !running);
  EnableWindow(GetDlgItem(hwnd, kOpenLogButton), true);
  EnableWindow(GetDlgItem(hwnd, kStartButton), !running);
  EnableWindow(GetDlgItem(hwnd, kStopButton), running);

  SendUiStep(UiStepForPage(g_page));
  SendUiButtonEnabled(L"back", false);
  SendUiButtonEnabled(L"next", false);
  SendUiButtonEnabled(L"start", !running);
  SendUiButtonEnabled(L"cancel", running);
  SendUiButtonEnabled(L"browseInstall", !running);
  SendUiState();
  InvalidateRect(hwnd, nullptr, FALSE);
}

void GoToPreviousPage(HWND hwnd) {
  if (g_workerRunning) {
    return;
  }
  if (g_page == WizardPage::Folders) {
    ShowWizardPage(hwnd, WizardPage::Welcome);
  } else if (g_page == WizardPage::Activity) {
    ShowWizardPage(hwnd, WizardPage::Folders);
  }
}

void GoToNextPage(HWND hwnd) {
  if (g_workerRunning) {
    return;
  }
  if (g_page == WizardPage::Welcome) {
    ShowWizardPage(hwnd, WizardPage::Folders);
  } else if (g_page == WizardPage::Folders) {
    ShowWizardPage(hwnd, WizardPage::Activity);
  }
}

void FinishWorker(HWND hwnd, bool installSucceeded = false) {
  g_workerRunning = false;
  PostMessageW(hwnd, kWorkerFinishedMessage, installSucceeded ? 1 : 0, 0);
}

std::wstring FormatExtractionStatus(const std::wstring& label,
                                    int percent,
                                    uintmax_t bytesPerSecond = 0,
                                    int etaSeconds = -1,
                                    int elapsedSeconds = -1) {
  std::wostringstream out;
  out << label << L" " << percent << L"%";
  if (bytesPerSecond > 0) {
    out << L" | " << FormatBytesPerSecond(bytesPerSecond);
  }
  if (etaSeconds >= 0) {
    out << L" | Осталось: " << FormatEta(etaSeconds);
  }
  if (elapsedSeconds >= 0) {
    out << L" | Прошло " << FormatEta(elapsedSeconds);
  }
  return out.str();
}

bool RunExtractionStep(HWND hwnd,
                       modlist::SevenZipExtractor& extractor,
                       const modlist::ExtractionConfig& extraction,
                       const std::wstring& statusLabel,
                       int progressBase,
                       int progressSpan,
                       uintmax_t estimatedBytes) {
  PostLog(hwnd, L"Распаковка: " + PathToDisplay(extraction.archiveFirstPart));
  PostProgress(hwnd, progressBase);
  PostStatus(hwnd, FormatExtractionStatus(statusLabel, 0, 0, -1, 0));
  int lastPercent = -1;
  uintmax_t sampledSpeed = 0;
  int sampledEta = -1;
  const auto startedAt = std::chrono::steady_clock::now();
  auto etaSampledAt = startedAt;
  const auto result = extractor.Extract(
      extraction,
      [hwnd, statusLabel, progressBase, progressSpan, estimatedBytes, startedAt,
       &lastPercent, &sampledSpeed, &sampledEta, &etaSampledAt](int percent) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startedAt).count();
        if (percent != lastPercent) {
          lastPercent = percent;
          const int mapped = progressBase + (percent * progressSpan) / 100;
          PostProgress(hwnd, mapped);
          sampledSpeed = 0;
          sampledEta = -1;
          if (estimatedBytes > 0 && elapsed > 0 && percent > 0 && percent < 100) {
            const uintmax_t processed = (estimatedBytes * static_cast<uintmax_t>(percent)) / 100;
            sampledSpeed = processed / static_cast<uintmax_t>(elapsed);
            if (sampledSpeed > 0 && estimatedBytes > processed) {
              sampledEta = static_cast<int>((estimatedBytes - processed) / sampledSpeed);
            }
          }
          etaSampledAt = now;
        }

        int displayEta = sampledEta;
        if (displayEta >= 0) {
          const auto sinceSample = std::chrono::duration_cast<std::chrono::seconds>(now - etaSampledAt).count();
          displayEta = std::max(0, displayEta - static_cast<int>(sinceSample));
        }
        PostStatus(hwnd, FormatExtractionStatus(
                             statusLabel, std::max(0, lastPercent), sampledSpeed,
                             displayEta, static_cast<int>(elapsed)));
      });
  PostLog(hwnd, Widen(result.message));
  WriteLastSevenZipLog(result.output);
  if (!result.ok) {
    if (!result.outputLogPath.empty()) {
      PostLog(hwnd, L"Full 7-Zip output saved to: " + PathToDisplay(result.outputLogPath));
    }
    if (!result.output.empty()) {
      PostLog(hwnd, L"Ошибка 7-Zip, детали:");
      PostLog(hwnd, TailForLog(result.output, 2000));
    } else {
      PostLog(hwnd, L"7-Zip produced no captured output. Check the saved log path above.");
    }
  } else if (result.exitCode == 1 && !result.outputLogPath.empty()) {
    PostLog(hwnd, L"Warning details saved to: " + PathToDisplay(result.outputLogPath));
  }
  if (result.ok) {
    PostProgress(hwnd, progressBase + progressSpan);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    PostStatus(hwnd, FormatExtractionStatus(statusLabel, 100, 0, -1, static_cast<int>(elapsed)));
  }
  return result.ok;
}

bool RunPipelinedExtractionStep(HWND hwnd,
                                const modlist::PipelinedExtractionConfig& extraction,
                                int progressBase,
                                int progressSpan) {
  PostLog(hwnd, L"Асинхронная проверка и распаковка: " +
                    PathToDisplay(extraction.manifest->extract.firstArchivePart));
  PostLog(hwnd, L"Каждый блок SHA-256 проверяется до передачи данных в 7-Zip.");
  PostLog(hwnd, L"Потоки распаковки 7-Zip: " +
                    std::to_wstring(extraction.decoderThreads));
  PostProgress(hwnd, progressBase);
  const auto startedAt = std::chrono::steady_clock::now();
  struct PipelineActivityLogState {
    std::vector<bool> announcedArchives;
    std::vector<bool> completedArchives;
    std::vector<bool> extractedArchives;
    std::vector<std::filesystem::path> validationNames;
    std::vector<std::filesystem::path> extractionNames;
    size_t nextAnnouncedArchive{0};
    size_t nextCompletedArchive{0};
    size_t nextExtractedArchive{0};
  };
  const auto activityLog = std::make_shared<PipelineActivityLogState>();
  modlist::PipelinedSevenZipExtractor extractor;
  const auto result = extractor.Extract(
      extraction,
      [hwnd, progressBase, progressSpan, startedAt, activityLog](const auto& progress) {
        const int validationPercent = progress.validationTotalBytes > 0
            ? static_cast<int>(std::min<uint64_t>(
                  100, progress.validatedBytes * 100 / progress.validationTotalBytes))
            : 0;
        const int overallPercent = std::min(validationPercent, progress.extractionPercent);
        PostProgress(hwnd, progressBase + overallPercent * progressSpan / 100);

        if (progress.validationArchiveCount > 0 &&
            activityLog->announcedArchives.size() != progress.validationArchiveCount) {
          activityLog->announcedArchives.assign(progress.validationArchiveCount, false);
          activityLog->completedArchives.assign(progress.validationArchiveCount, false);
          activityLog->validationNames.resize(progress.validationArchiveCount);
        }
        if (progress.validationArchiveIndex > 0 &&
            progress.validationArchiveIndex <= activityLog->announcedArchives.size()) {
          const size_t archiveIndex = progress.validationArchiveIndex - 1;
          activityLog->validationNames[archiveIndex] = progress.validationArchive;
          activityLog->announcedArchives[archiveIndex] = true;
          if (progress.validationArchiveTotalBytes > 0 &&
              progress.validationArchiveBytes >= progress.validationArchiveTotalBytes) {
            activityLog->completedArchives[archiveIndex] = true;
          }
          while (activityLog->nextAnnouncedArchive <
                     activityLog->announcedArchives.size() &&
                 activityLog->announcedArchives[activityLog->nextAnnouncedArchive]) {
            const size_t next = activityLog->nextAnnouncedArchive++;
            PostLog(hwnd, L"Проверка архива " +
                              std::to_wstring(next + 1) + L"/" +
                              std::to_wstring(progress.validationArchiveCount) + L": " +
                              PathToDisplay(activityLog->validationNames[next]));
          }
          while (activityLog->nextCompletedArchive <
                     activityLog->completedArchives.size() &&
                 activityLog->completedArchives[activityLog->nextCompletedArchive]) {
            const size_t next = activityLog->nextCompletedArchive++;
            PostLog(hwnd, L"Архив проверен: " +
                              PathToDisplay(activityLog->validationNames[next]));
          }
        }

        if (progress.extractionArchiveCount > 0 &&
            activityLog->extractedArchives.size() != progress.extractionArchiveCount) {
          activityLog->extractedArchives.assign(progress.extractionArchiveCount, false);
          activityLog->extractionNames.resize(progress.extractionArchiveCount);
        }
        if (progress.extractionArchiveIndex > 0 &&
            progress.extractionArchiveIndex <= activityLog->extractedArchives.size()) {
          const size_t archiveIndex = progress.extractionArchiveIndex - 1;
          activityLog->extractionNames[archiveIndex] = progress.extractionArchive;
          activityLog->extractedArchives[archiveIndex] = true;
          while (activityLog->nextExtractedArchive <
                     activityLog->extractedArchives.size() &&
                 activityLog->extractedArchives[activityLog->nextExtractedArchive]) {
            const size_t next = activityLog->nextExtractedArchive++;
            PostLog(hwnd, L"Распаковка архива " +
                              std::to_wstring(next + 1) + L"/" +
                              std::to_wstring(progress.extractionArchiveCount) + L": " +
                              PathToDisplay(activityLog->extractionNames[next]));
          }
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - startedAt).count();
        uintmax_t speed = 0;
        int eta = -1;
        if (elapsed > 0 && progress.validatedBytes > 0) {
          speed = progress.validatedBytes / static_cast<uintmax_t>(elapsed);
          if (speed > 0 && progress.validationTotalBytes > progress.validatedBytes) {
            eta = static_cast<int>(
                (progress.validationTotalBytes - progress.validatedBytes) / speed);
          }
        }
        std::wostringstream status;
        status << L"Проверка " << validationPercent << L"% | Распаковка "
               << progress.extractionPercent << L"%";
        if (speed > 0) {
          status << L" | " << FormatBytesPerSecond(speed);
        }
        if (eta >= 0) {
          status << L" | Осталось: " << FormatEta(eta);
        }
        status << L" | Прошло " << FormatEta(static_cast<int>(elapsed));
        PostStatus(hwnd, status.str());
      });

  PostLog(hwnd, Widen(result.message));
  WriteLastSevenZipLog(result.output);
  if (!result.ok) {
    PostLog(hwnd, L"Ошибка асинхронной проверки/распаковки: " + Widen(result.message));
    if (result.message.find("SHA256 mismatch") != std::string::npos) {
      PostValidationFailed(hwnd);
    }
    return false;
  }
  PostProgress(hwnd, progressBase + progressSpan);
  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - startedAt).count();
  PostStatus(hwnd, L"Проверка 100% | Распаковка 100% | Прошло " +
                       FormatEta(static_cast<int>(elapsed)));
  return true;
}

bool ExtractArchiveChain(HWND hwnd,
                         modlist::SevenZipExtractor& extractor,
                         const std::filesystem::path& sevenZipExe,
                         std::filesystem::path archiveFirstPart,
                         std::filesystem::path installFolder,
                         int progressBase = 0,
                         int progressSpan = 100,
                         uintmax_t estimatedUnpackedBytes = 0) {
  modlist::ExtractionConfig extraction;
  extraction.sevenZipExe = sevenZipExe;
  extraction.archiveFirstPart = std::move(archiveFirstPart);
  extraction.installFolder = installFolder;
  extraction.cancelRequested = &g_stopRequested;
  extraction.useSameDiskTemp = true;

  const bool splitArchive = IsFirstSplitArchivePart(extraction.archiveFirstPart);
  std::error_code sizeEc;
  if (estimatedUnpackedBytes == 0) {
    if (splitArchive) {
      estimatedUnpackedBytes = EstimateNearbyArchiveBytes(extraction.archiveFirstPart.parent_path());
    } else {
      estimatedUnpackedBytes = std::filesystem::file_size(extraction.archiveFirstPart, sizeEc);
    }
    if (sizeEc) {
      estimatedUnpackedBytes = 0;
    }
  }
  if (!RunExtractionStep(
          hwnd, extractor, extraction, L"Распаковано",
          progressBase, progressSpan, estimatedUnpackedBytes)) {
    return false;
  }

  return true;
}

std::wstring NormalizedPathText(const std::filesystem::path& path) {
  std::error_code ec;
  auto normalized = std::filesystem::absolute(path, ec).lexically_normal().wstring();
  if (ec) {
    normalized = path.lexically_normal().wstring();
  }
  for (auto& ch : normalized) {
    ch = static_cast<wchar_t>(std::towlower(ch));
  }
  while (!normalized.empty() && (normalized.back() == L'\\' || normalized.back() == L'/')) {
    normalized.pop_back();
  }
  return normalized;
}

bool IsSameFolder(const std::filesystem::path& a, const std::filesystem::path& b) {
  return NormalizedPathText(a) == NormalizedPathText(b);
}

bool IsChildFolder(const std::filesystem::path& child, const std::filesystem::path& parent) {
  const auto childText = NormalizedPathText(child);
  const auto parentText = NormalizedPathText(parent);
  return childText.size() > parentText.size() &&
         childText.starts_with(parentText) &&
         (childText[parentText.size()] == L'\\' || childText[parentText.size()] == L'/');
}

struct InstallProgress {
  HWND hwnd{nullptr};
  uintmax_t totalBytes{0};
  uintmax_t doneBytes{0};
  int lastPercent{-1};
  std::chrono::steady_clock::time_point startedAt{std::chrono::steady_clock::now()};
};

bool ReadDirectoryEntries(const std::filesystem::path& folder,
                          std::vector<std::filesystem::path>& entries,
                          std::wstring& error) {
  std::error_code ec;
  auto iterator = std::filesystem::directory_iterator(folder, ec);
  const auto end = std::filesystem::directory_iterator();
  if (ec) {
    error = L"Невозможно прочитать распакованную папку: " + Widen(ec.message());
    return false;
  }
  while (iterator != end) {
    entries.push_back(iterator->path());
    iterator.increment(ec);
    if (ec) {
      error = L"Невозможно прочитать распакованную папку: " + Widen(ec.message());
      return false;
    }
  }
  return true;
}

std::optional<uintmax_t> EstimateInstallBytes(const std::filesystem::path& path,
                                              std::wstring& error) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    error = ec ? L"Невозможно прочитать распакованный файл: " + Widen(ec.message())
               : L"Невозможно прочитать распакованный файл: " + PathToDisplay(path);
    return std::nullopt;
  }
  if (std::filesystem::is_regular_file(path, ec)) {
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
      error = L"Невозможно прочитать распакованный файл: " + PathToDisplay(path);
      return std::nullopt;
    }
    return size;
  }
  if (ec || !std::filesystem::is_directory(path, ec)) {
    if (ec) {
      error = L"Невозможно прочитать распакованный файл: " + PathToDisplay(path);
      return std::nullopt;
    }
    return 0;
  }

  uintmax_t total = 0;
  auto iterator = std::filesystem::recursive_directory_iterator(path, ec);
  const auto end = std::filesystem::recursive_directory_iterator();
  if (ec) {
    error = L"Невозможно прочитать распакованную папку: " + Widen(ec.message());
    return std::nullopt;
  }
  while (iterator != end) {
    const auto entry = *iterator;
    if (entry.is_regular_file(ec)) {
      const auto size = entry.file_size(ec);
      if (ec) {
        error = L"Невозможно прочитать распакованный файл: " + PathToDisplay(entry.path());
        return std::nullopt;
      }
      total += size;
    } else if (ec) {
      error = L"Невозможно прочитать распакованный файл: " + PathToDisplay(entry.path());
      return std::nullopt;
    }
    iterator.increment(ec);
    if (ec) {
      error = L"Невозможно прочитать распакованную папку: " + Widen(ec.message());
      return std::nullopt;
    }
  }
  return total;
}

void UpdateInstallProgress(InstallProgress& progress, uintmax_t bytes, bool force = false) {
  progress.doneBytes = std::min(progress.totalBytes, progress.doneBytes + bytes);
  const int percent = progress.totalBytes > 0
                          ? static_cast<int>((progress.doneBytes * 100) / progress.totalBytes)
                          : 0;
  if (!force && percent == progress.lastPercent) {
    return;
  }
  progress.lastPercent = percent;

  const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::steady_clock::now() - progress.startedAt).count();
  uintmax_t speed = 0;
  int eta = -1;
  if (elapsed > 0 && progress.doneBytes > 0) {
    speed = progress.doneBytes / static_cast<uintmax_t>(elapsed);
    if (speed > 0 && progress.totalBytes > progress.doneBytes) {
      eta = static_cast<int>((progress.totalBytes - progress.doneBytes) / speed);
    }
  }

  std::wostringstream status;
  status << L"Установка " << percent << L"%";
  if (progress.totalBytes > 0) {
    status << L" | " << FormatBytes(progress.doneBytes) << L" / " << FormatBytes(progress.totalBytes);
  }
  if (speed > 0) {
    status << L" | " << FormatBytesPerSecond(speed);
  }
  if (eta >= 0) {
    status << L" | Осталось: " << FormatEta(eta);
  }
  status << L" | Прошло " << FormatEta(static_cast<int>(elapsed));

  PostProgress(progress.hwnd, 95 + (percent * 5) / 100);
  PostStatus(progress.hwnd, status.str());
}

bool MoveWholeEntry(const std::filesystem::path& source,
                    const std::filesystem::path& target,
                    std::wstring& error) {
  if (g_stopRequested.load()) {
    error = L"Установка остановлена пользователем";
    return false;
  }
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) {
    error = L"Невозможно создать папку установки: " + Widen(ec.message());
    return false;
  }

  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (!std::filesystem::is_directory(source, ec)) {
    flags |= MOVEFILE_REPLACE_EXISTING;
  }
  if (!MoveFileExW(source.wstring().c_str(), target.wstring().c_str(), flags)) {
    error = L"Невозможно переместить " + PathToDisplay(source) + L" в " + PathToDisplay(target) +
            L" (Windows error " + std::to_wstring(GetLastError()) + L").";
    return false;
  }
  return true;
}

bool CopyFileWithProgress(const std::filesystem::path& source,
                          const std::filesystem::path& target,
                          InstallProgress& progress,
                          std::wstring& error) {
  constexpr size_t kBufferSize = 4 * 1024 * 1024;
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) {
    error = L"Невозможно создать папку установки: " + Widen(ec.message());
    return false;
  }

  std::ifstream input(source, std::ios::binary);
  if (!input) {
    error = L"Невозможно прочитать распакованный файл: " + PathToDisplay(source);
    return false;
  }
  std::ofstream output(target, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = L"Невозможно записать файл: " + PathToDisplay(target);
    return false;
  }

  std::vector<char> buffer(kBufferSize);
  while (input) {
    if (g_stopRequested.load()) {
      error = L"Установка остановлена пользователем";
      return false;
    }
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = input.gcount();
    if (read <= 0) {
      break;
    }
    output.write(buffer.data(), read);
    if (!output) {
      error = L"Невозможно записать файл: " + PathToDisplay(target);
      return false;
    }
    UpdateInstallProgress(progress, static_cast<uintmax_t>(read));
  }
  if (!input.eof()) {
    error = L"Невозможно скопировать установочный файл: " + PathToDisplay(source);
    return false;
  }
  output.close();
  if (!output) {
    error = L"Невозможно завершить запись файла: " + PathToDisplay(target);
    return false;
  }
  const auto sourceTime = std::filesystem::last_write_time(source, ec);
  if (!ec) {
    std::filesystem::last_write_time(target, sourceTime, ec);
  }
  return true;
}

bool InstallEntry(HWND hwnd,
                  const std::filesystem::path& source,
                  const std::filesystem::path& target,
                  bool sameDrive,
                  InstallProgress& progress,
                  std::wstring& error) {
  if (g_stopRequested.load()) {
    error = L"Установка остановлена пользователем";
    return false;
  }
  std::error_code ec;
  if (sameDrive && !std::filesystem::exists(target, ec)) {
    const auto movedBytes = EstimateInstallBytes(source, error);
    if (!movedBytes.has_value()) {
      return false;
    }
    if (!MoveWholeEntry(source, target, error)) {
      return false;
    }
    UpdateInstallProgress(progress, *movedBytes, true);
    return true;
  }

  if (std::filesystem::is_directory(source, ec) && !ec) {
    std::filesystem::create_directories(target, ec);
    if (ec) {
      error = L"Невозможно создать папку установки: " + Widen(ec.message());
      return false;
    }
    std::vector<std::filesystem::path> entries;
    if (!ReadDirectoryEntries(source, entries, error)) {
      return false;
    }
    for (const auto& entry : entries) {
      if (g_stopRequested.load()) {
        error = L"Установка остановлена пользователем";
        return false;
      }
      if (!InstallEntry(hwnd, entry, target / entry.filename(), sameDrive, progress, error)) {
        return false;
      }
    }
    std::filesystem::remove(source, ec);
    if (ec) {
      error = L"Невозможно удалить источник установки: " + Widen(ec.message());
      return false;
    }
    return true;
  }

  if (sameDrive) {
    const auto movedBytes = EstimateInstallBytes(source, error);
    if (!movedBytes.has_value()) {
      return false;
    }
    if (!MoveWholeEntry(source, target, error)) {
      return false;
    }
    UpdateInstallProgress(progress, *movedBytes, true);
    return true;
  }

  if (!CopyFileWithProgress(source, target, progress, error)) {
    return false;
  }
  std::filesystem::remove(source, ec);
  if (ec) {
    error = L"Невозможно удалить скопированные, распакованные файлы: " + Widen(ec.message());
    return false;
  }
  return true;
}

bool InstallExtractedFiles(HWND hwnd, const std::filesystem::path& unpackFolder, const std::filesystem::path& installFolder) {
  PostProgress(hwnd, 95);
  PostStatus(hwnd, L"Установка 0%");
  if (IsSameFolder(unpackFolder, installFolder)) {
    PostLog(hwnd, L"Папка установки является папкой распаковки; перемещение не требуется");
    PostProgress(hwnd, 100);
    PostStatus(hwnd, L"Установка 100%");
    return true;
  }
  if (IsChildFolder(installFolder, unpackFolder)) {
    PostLog(hwnd, L"Папка установки не может располагаться в папке распаковки");
    return false;
  }
  if (const auto emptyError = FolderMustBeEmptyError(installFolder, L"Папка установки");
      emptyError.has_value()) {
    PostLog(hwnd, *emptyError);
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(installFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Невозможно создать папку установки: " + Widen(ec.message()));
    return false;
  }

  modlist::PathValidator validator;
  const bool sameDrive = validator.IsSameDrive(unpackFolder, installFolder);
  PostLog(hwnd, sameDrive ? L"Installing with same-drive cut/move; files will not be copied."
                          : L"Установка между дисками; Windows должна скопировать файлы, затем удалить распакованные файлы");

  InstallProgress installProgress;
  installProgress.hwnd = hwnd;
  std::wstring error;
  std::vector<std::filesystem::path> entries;
  if (!ReadDirectoryEntries(unpackFolder, entries, error)) {
    PostLog(hwnd, error);
    return false;
  }
  for (const auto& entry : entries) {
    if (entry.filename() != ".install_temp") {
      const auto entryBytes = EstimateInstallBytes(entry, error);
      if (!entryBytes.has_value()) {
        PostLog(hwnd, error);
        return false;
      }
      installProgress.totalBytes += *entryBytes;
    }
  }
  PostLog(hwnd, L"Размер сборки: " + FormatBytes(installProgress.totalBytes));
  if (!sameDrive) {
    const uintmax_t installFree = FreeBytes(installFolder);
    PostLog(hwnd, L"Свободное место: " + FormatBytes(installFree) + L"; распакованная сборка: " + FormatBytes(installProgress.totalBytes));
    if (installProgress.totalBytes > 0 && installFree < installProgress.totalBytes) {
      PostLog(hwnd, L"Не хватает места, чтобы установить распакованные файлы");
      return false;
    }
  }
  UpdateInstallProgress(installProgress, 0, true);

  for (const auto& entry : entries) {
    if (g_stopRequested.load()) {
      PostLog(hwnd, L"Установка остановлена пользователем");
      return false;
    }
    if (entry.filename() == ".install_temp") {
      std::filesystem::remove_all(entry, ec);
      if (ec) {
        PostLog(hwnd, L"Невозможно удалить временную папку: " + Widen(ec.message()));
        return false;
      }
    } else {
      const auto target = installFolder / entry.filename();
      if (!InstallEntry(hwnd, entry, target, sameDrive, installProgress, error)) {
        PostLog(hwnd, error);
        return false;
      }
    }
  }

  std::filesystem::remove(unpackFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Установка выполнена, но невозможно удалить папку распаковки: " +
                      Widen(ec.message()));
  }

  PostProgress(hwnd, 100);
  PostStatus(hwnd, L"Установка 100%");
  PostLog(hwnd, L"Установка выполнена");
  return true;
}

void RunInstallWorker(HWND hwnd,
                      modlist::PackageDiscovery package,
                      std::filesystem::path unpackFolder,
                      std::filesystem::path installFolder) {
  g_workerRunning = true;
  PostProgress(hwnd, 0);
  PostLog(hwnd, L"Начало проверки...");

  std::optional<modlist::Manifest> manifest;
  {
    std::wstring manifestMessage;
    manifest = LoadPackageManifest(manifestMessage);
    PostLog(hwnd, manifestMessage);
  }
  if (!manifest.has_value()) {
    PostLog(hwnd, L"Необходим файл data\\package\\manifest.json.");
    PostValidationFailed(hwnd);
    FinishWorker(hwnd);
    return;
  }
  modlist::PipelinedSevenZipExtractor pipelineExtractor;
  const auto pipelineLibrary = pipelineExtractor.LocateLibrary(ExeFolder());
  const bool usePipeline =
      modlist::PipelinedSevenZipExtractor::CanUse(*manifest) && pipelineLibrary.ok();
  if (usePipeline) {
    PostLog(hwnd, L"Доступна асинхронная проверка SHA-256 во время распаковки.");
  } else {
    if (!pipelineLibrary.ok()) {
      PostLog(hwnd, L"Асинхронная проверка недоступна: " + Widen(pipelineLibrary.error()));
    } else {
      PostLog(hwnd, L"Manifest не содержит совместимые блоки SHA-256; используется обычная проверка.");
    }
    if (!VerifyPackageManifest(hwnd, *manifest)) {
      FinishWorker(hwnd);
      return;
    }
  }

  PostLog(hwnd, L"Поиск архива для распаковки...");
  auto firstArchivePart = package.firstArchivePart;
  firstArchivePart = ArchivePartFromManifest(*manifest);
  if (!firstArchivePart.has_value()) {
    firstArchivePart = FindFirstArchivePart(ArchiveFolder());
  }
  if (!firstArchivePart.has_value()) {
    PostLog(hwnd, L"Архив не найден/не скачан. Ошибка распаковки.");
    FinishWorker(hwnd);
    return;
  }

  PostLog(hwnd, L"Выбранная папка установки: " + PathToDisplay(installFolder));

  if (const auto emptyError = FolderMustBeEmptyError(unpackFolder, L"Папка распаковки");
      emptyError.has_value()) {
    PostLog(hwnd, *emptyError);
    FinishWorker(hwnd);
    return;
  }
  if (!IsSameFolder(unpackFolder, installFolder)) {
    if (const auto emptyError = FolderMustBeEmptyError(installFolder, L"Папка установки");
        emptyError.has_value()) {
      PostLog(hwnd, *emptyError);
      FinishWorker(hwnd);
      return;
    }
  }

  modlist::PathValidator pathValidator;
  const bool sameVolume = pathValidator.IsSameDrive(unpackFolder, installFolder);
  const auto requirements = SpaceRequirementsFromManifest(*manifest);
  const auto plan = modlist::PlanInstallSpace(requirements.unpackedBytes, sameVolume);
  if (plan.unpackRequiredBytes > 0) {
    const uintmax_t unpackFree = FreeBytes(unpackFolder);
    PostLog(hwnd, L"Свободное место для распаковки: " + FormatBytes(unpackFree) +
                      L"; требуется с запасом: " + FormatBytes(plan.unpackRequiredBytes));
    if (unpackFree < plan.unpackRequiredBytes) {
      PostLog(hwnd, L"Не хватает места для распаковки архива");
      FinishWorker(hwnd);
      return;
    }
  }
  if (plan.installRequiredBytes > 0) {
    const uintmax_t installFree = FreeBytes(installFolder);
    PostLog(hwnd, L"Свободное место для установки: " + FormatBytes(installFree) +
                      L"; требуется: " + FormatBytes(plan.installRequiredBytes));
    if (installFree < plan.installRequiredBytes) {
      PostLog(hwnd, L"Не хватает места для установки распакованных файлов");
      FinishWorker(hwnd);
      return;
    }
  } else if (requirements.unpackedBytes > 0) {
    PostLog(hwnd, L"Распаковка и установка находятся на одном томе; вторая полная копия не требуется.");
  }

  bool extracted = false;
  if (usePipeline) {
    modlist::PipelinedExtractionConfig extraction;
    extraction.sevenZipLibrary = pipelineLibrary.value();
    extraction.archiveFolder = ArchiveFolder();
    extraction.installFolder = unpackFolder;
    extraction.manifest = &*manifest;
    extraction.cancelRequested = &g_stopRequested;
    extracted = RunPipelinedExtractionStep(hwnd, extraction, 0, 95);
  } else {
    modlist::SevenZipExtractor extractor;
    auto sevenZip = extractor.LocateExecutable(ExeFolder());
    if (!sevenZip.ok()) {
      PostLog(hwnd, L"Ошибка 7-Zip: " + Widen(sevenZip.error()));
      FinishWorker(hwnd);
      return;
    }
    extracted = ExtractArchiveChain(
        hwnd, extractor, sevenZip.value(), *firstArchivePart, unpackFolder,
        35, 60, requirements.unpackedBytes);
  }
  if (!extracted) {
    PostProgress(hwnd, 0);
    FinishWorker(hwnd);
    return;
  }

  const bool installed = InstallExtractedFiles(hwnd, unpackFolder, installFolder);
  PostProgress(hwnd, installed ? 100 : 0);
  FinishWorker(hwnd, installed);
}

bool ValidateFolders(const SpaceRequirements& requirements = {},
                     bool allowCleanupPrompt = false) {
  modlist::PathValidator validator;
  bool ok = true;
  std::vector<std::wstring> errors;
  std::vector<std::wstring> cleanupErrors;
  PendingFolderCleanup pendingCleanup;
  g_pendingFolderCleanup.reset();

  auto addError = [&](const std::wstring& message) {
    AppendLog(message);
    errors.push_back(message);
    ok = false;
  };

  const auto installText = GetText(g_installEdit);
  const auto selectedInstallRoot = std::filesystem::path(installText);
  const auto installFolder = FinalInstallFolder(selectedInstallRoot);
  const auto unpackFolder = SelectedUnpackFolder();
  if (!unpackFolder.empty()) {
    const auto result = validator.ValidateInstallFolder(unpackFolder);
    AppendLog(L"Unpack folder: " + PathToDisplay(unpackFolder) + L" - " + Widen(result.message));
    if (result.warning) {
      AppendLog(L"Warning: " + Widen(result.message));
    }
    if (!result.ok) {
      addError(L"Папка распаковки недоступна: " + Widen(result.message));
    }
    if (result.ok) {
      if (const auto emptyError = FolderMustBeEmptyError(unpackFolder, L"Папка распаковки");
          emptyError.has_value()) {
        const auto message = *emptyError + L"\n" + PathToDisplay(unpackFolder);
        if (allowCleanupPrompt && IsExistingNonEmptyDirectory(unpackFolder)) {
          AppendLog(message);
          cleanupErrors.push_back(message);
          pendingCleanup.unpackFolder = unpackFolder;
          ok = false;
        } else {
          addError(message);
        }
      }
    }
  } else {
    addError(L"Диск для распаковки не выбран.");
  }

  if (!installText.empty() && !installFolder.empty()) {
    const auto result = validator.ValidateInstallFolder(installFolder);
    AppendLog(L"Итоговая папка установки: " + PathToDisplay(installFolder) +
              L" - " + Widen(result.message));
    if (result.warning) {
      AppendLog(L"Warning: " + Widen(result.message));
    }
    if (!result.ok) {
      addError(L"Папка установки недоступна: " + Widen(result.message));
    }
    if (!unpackFolder.empty() && IsChildFolder(installFolder, unpackFolder)) {
      addError(L"Папка установки не может располагаться в папке распаковки.");
    }
    if (result.ok && (unpackFolder.empty() || !IsSameFolder(unpackFolder, installFolder))) {
      if (const auto emptyError = FolderMustBeEmptyError(installFolder, L"Папка установки");
          emptyError.has_value()) {
        const auto message = *emptyError + L"\n" + PathToDisplay(installFolder);
        if (allowCleanupPrompt && IsExistingNonEmptyDirectory(installFolder)) {
          AppendLog(message);
          cleanupErrors.push_back(message);
          pendingCleanup.installFolder = installFolder;
          ok = false;
        } else {
          addError(message);
        }
      }
    }
  } else {
    addError(installText.empty()
                 ? L"Папка установки не выбрана."
                 : L"Не удалось определить имя папки сборки из manifest.json.");
  }

  if (!unpackFolder.empty() && !installFolder.empty()) {
    const bool sameVolume = validator.IsSameDrive(unpackFolder, installFolder);
    const auto plan = modlist::PlanInstallSpace(requirements.unpackedBytes, sameVolume);
    if (!HasEnoughSpace(unpackFolder, plan.unpackRequiredBytes, L"Папка распаковки")) {
      errors.push_back(L"Недостаточно свободного места для распаковки.");
      ok = false;
    }
    if (plan.installRequiredBytes > 0) {
      if (!HasEnoughSpace(installFolder, plan.installRequiredBytes, L"Папка установки")) {
        errors.push_back(L"Недостаточно свободного места для установки.");
        ok = false;
      }
    } else if (requirements.unpackedBytes > 0) {
      AppendLog(L"Папки находятся на одном томе: установка будет перемещением и не требует второй полной копии.");
    }
  }

  if (!pendingCleanup.empty()) {
    if (errors.empty()) {
      g_pendingFolderCleanup = pendingCleanup;
      SendUiCleanupConfirm(pendingCleanup);
    } else {
      errors.insert(errors.end(), cleanupErrors.begin(), cleanupErrors.end());
    }
  }

  if (!ok && !errors.empty()) {
    std::wostringstream message;
    for (size_t i = 0; i < errors.size(); ++i) {
      if (i > 0) {
        message << L"\n\n";
      }
      message << errors[i];
    }
    SendUiError(UiText("path_error_title", L"Ошибка пути"), message.str());
  }

  return ok;
}

void ValidatePackage() {
  SendMessageW(g_progress, PBM_SETPOS, 0, 0);
  SendUiProgress(0, g_statusText);
  AppendLog(L"Проверка архива");
  auto package = ReadPackageFromUi();
  if (!package.ok()) {
    AppendLog(L"Ошибка сборки: " + Widen(package.error()));
    return;
  }

  if (package.value().firstArchivePart.has_value()) {
    AppendLog(L"Первая часть архива: " + PathToDisplay(*package.value().firstArchivePart));
  } else {
    AppendLog(L"Архив не найден/не скачан. Ошибка распаковки.");
  }

  std::wstring manifestMessage;
  auto manifest = LoadPackageManifest(manifestMessage);
  AppendLog(manifestMessage);
  if (manifest.has_value()) {
    AppendLog(L"Manifest files: " + std::to_wstring(manifest->files.size()));
    AppendLog(L"Manifest archive entry: " + PathToDisplay(manifest->extract.firstArchivePart));
    if (manifest->unpackedSize > 0) {
      AppendLog(L"Размер распакованной сборки: " + FormatBytes(manifest->unpackedSize));
    }
    if (!VerifyPackageManifest(GetParent(g_progress), *manifest)) {
      SendMessageW(g_progress, PBM_SETPOS, 0, 0);
      SendUiProgress(0, L"Ошибка проверки");
      return;
    }
  }

  const auto requirements = EstimateSpaceRequirements(package.value());
  ValidateFolders(requirements);

  modlist::SevenZipExtractor extractor;
  auto sevenZip = extractor.LocateExecutable(ExeFolder());
  if (sevenZip.ok()) {
    AppendLog(L"7-Zip: " + PathToDisplay(sevenZip.value()));
  } else {
    AppendLog(L"7-Zip warning: " + Widen(sevenZip.error()));
  }
  SendMessageW(g_progress, PBM_SETPOS, 100, 0);
  SendUiProgress(100, g_statusText);
}

void SetControlsRunning(HWND hwnd, bool running) {
  (void)running;
  SetWindowTextW(GetDlgItem(hwnd, kPauseButton), L"Пауза");
  ShowWizardPage(hwnd, g_page);
}

void TogglePause(HWND hwnd) {
  (void)hwnd;
  AppendLog(L"Пауза недоступна во время проверки.");
}

void StopInstall() {
  if (!g_workerRunning) {
    AppendLog(L"Нет активных операций для остановки.");
    return;
  }
  g_stopRequested = true;
  AppendLog(L"Остановка. Существующие файлы сборки останутся без изменений.");
}

void StartInstall(HWND hwnd) {
  if (g_installCompleted) {
    SendMessageW(hwnd, WM_CLOSE, 0, 0);
    return;
  }
  if (g_workerRunning) {
    AppendLog(L"Установщик уже запущен.");
    return;
  }
  if (const auto documentsError = EnsureGameDocumentsFolders();
      documentsError.has_value()) {
    AppendLog(*documentsError);
    SendUiError(UiText("documents_error_title", L"Ошибка папки документов"), *documentsError);
    return;
  }
  AppendLog(L"Начало установки...");
  auto package = ReadPackageFromUi();
  if (!package.ok()) {
    AppendLog(L"Ошибка сборки: " + Widen(package.error()));
    return;
  }
  std::wstring manifestMessage;
  auto manifest = LoadPackageManifest(manifestMessage);
  if (!manifest.has_value()) {
    AppendLog(manifestMessage);
    AppendLog(L"Необходим файл data\\package\\manifest.json.");
    return;
  }
  g_archiveFolderName = Widen(manifest->archiveName);
  SendUiState();
  const auto requirements = EstimateSpaceRequirements(package.value());
  if (!ValidateFolders(requirements, true)) {
    if (!g_pendingFolderCleanup.has_value()) {
      AppendLog(L"Исправьте ошибки перед началом.");
    }
    return;
  }

  const auto unpackFolder = SelectedUnpackFolder();
  const auto installFolder =
      FinalInstallFolder(std::filesystem::path(GetText(g_installEdit)));
  g_stopRequested = false;
  g_workerRunning = true;
  SetControlsRunning(hwnd, true);
  g_closeAfterWorker = false;
  std::thread(RunInstallWorker, hwnd, std::move(package.value()), unpackFolder, installFolder).detach();
}

std::optional<std::wstring> ClearFolderContents(const std::filesystem::path& folder,
                                                const std::wstring& label) {
  std::error_code existsEc;
  if (!std::filesystem::exists(folder, existsEc)) {
    if (existsEc) {
      return L"Невозможно проверить " + label + L":\n" + PathToDisplay(folder) +
             L"\n\n" + Widen(existsEc.message());
    }
    return std::nullopt;
  }

  std::error_code directoryEc;
  if (!std::filesystem::is_directory(folder, directoryEc) || directoryEc) {
    return label + L" больше не является папкой:\n" + PathToDisplay(folder);
  }

  std::error_code readEc;
  std::vector<std::filesystem::path> entries;
  auto iterator = std::filesystem::directory_iterator(folder, readEc);
  const auto end = std::filesystem::directory_iterator();
  while (!readEc && iterator != end) {
    entries.push_back(iterator->path());
    iterator.increment(readEc);
  }
  if (readEc) {
    return L"Невозможно прочитать " + label + L":\n" + PathToDisplay(folder) +
           L"\n\n" + Widen(readEc.message());
  }

  for (const auto& entry : entries) {
    std::error_code removeEc;
    std::filesystem::remove_all(entry, removeEc);
    if (removeEc) {
      return L"Невозможно удалить:\n" + PathToDisplay(entry) +
             L"\n\n" + Widen(removeEc.message());
    }
  }
  return std::nullopt;
}

void ConfirmPendingFolderCleanup(HWND hwnd) {
  if (g_workerRunning) {
    SendUiError(UiText("installer_busy_title", L"Установщик занят"),
                UiText("installer_busy_message", L"Установщик уже запущен."));
    return;
  }

  if (!g_pendingFolderCleanup.has_value()) {
    SendUiError(UiText("cleanup_cancelled_title", L"Очистка отменена"),
                L"Папки для очистки больше не выбраны.");
    return;
  }

  const auto pending = *g_pendingFolderCleanup;
  g_pendingFolderCleanup.reset();
  const auto currentUnpackFolder = SelectedUnpackFolder();
  const auto selectedRoot = std::filesystem::path(GetText(g_installEdit));
  const auto currentInstallFolder = FinalInstallFolder(selectedRoot);

  const bool safeUnpack =
      pending.unpackFolder.empty() ||
      (!currentUnpackFolder.empty() &&
       IsSameFolder(pending.unpackFolder, currentUnpackFolder) &&
       IsSameFolder(currentUnpackFolder.parent_path(), currentUnpackFolder.root_path()) &&
       !IsSameFolder(currentUnpackFolder, currentUnpackFolder.root_path()) &&
       currentUnpackFolder.filename() == kUnpackFolderName);
  const bool safeInstall =
      pending.installFolder.empty() ||
      (!selectedRoot.empty() && !currentInstallFolder.empty() &&
       IsSameFolder(pending.installFolder, currentInstallFolder) &&
       IsSameFolder(currentInstallFolder.parent_path(), selectedRoot) &&
       !IsSameFolder(currentInstallFolder, currentInstallFolder.root_path()));
  if (!safeUnpack || !safeInstall) {
    AppendLog(L"Очистка отменена: выбранные папки изменились.");
    SendUiError(UiText("cleanup_cancelled_title", L"Очистка отменена"),
                L"Выбранные папки изменились. Нажмите «Установить» и проверьте пути ещё раз.");
    return;
  }

  if (!pending.unpackFolder.empty()) {
    AppendLog(L"Пользователь подтвердил очистку: " + PathToDisplay(currentUnpackFolder));
    if (const auto error = ClearFolderContents(currentUnpackFolder, L"папку распаковки");
        error.has_value()) {
      AppendLog(*error);
      SendUiError(UiText("cleanup_error_title", L"Ошибка очистки"), *error);
      return;
    }
    AppendLog(L"Папка распаковки очищена.");
  }

  if (!pending.installFolder.empty()) {
    AppendLog(L"Пользователь подтвердил очистку: " + PathToDisplay(currentInstallFolder));
    if (const auto error = ClearFolderContents(currentInstallFolder, L"папку установки");
        error.has_value()) {
      AppendLog(*error);
      SendUiError(UiText("cleanup_error_title", L"Ошибка очистки"), *error);
      return;
    }
    AppendLog(L"Папка установки очищена.");
  }

  StartInstall(hwnd);
}

void CancelPendingFolderCleanup() {
  if (g_pendingFolderCleanup.has_value()) {
    AppendLog(L"Очистка отменена пользователем. Файлы не изменены.");
    g_pendingFolderCleanup.reset();
    SendUiState();
  }
}

void RunUnpackWorker(HWND hwnd, std::filesystem::path archiveFirstPart, std::filesystem::path unpackFolder) {
  g_workerRunning = true;
  PostProgress(hwnd, 0);
  PostLog(hwnd, L"Распаковка архива: " + PathToDisplay(archiveFirstPart));

  if (const auto emptyError = FolderMustBeEmptyError(unpackFolder, L"Папка распаковки");
      emptyError.has_value()) {
    PostLog(hwnd, *emptyError);
    g_workerRunning = false;
    PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
    return;
  }

  const uintmax_t archiveBytes = EstimateNearbyArchiveBytes(archiveFirstPart.parent_path());
  if (archiveBytes > 0) {
    const uintmax_t requiredBytes = modlist::ExtractionSpaceRequirement(archiveBytes);
    const uintmax_t unpackFree = FreeBytes(unpackFolder);
    PostLog(hwnd, L"Свободное место для распаковки: " + FormatBytes(unpackFree) +
                      L"; требуется с запасом: " + FormatBytes(requiredBytes));
    if (unpackFree < requiredBytes) {
      PostLog(hwnd, L"Не хватает места для распаковки архива");
      g_workerRunning = false;
      PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
      return;
    }
  }

  modlist::SevenZipExtractor extractor;
  auto sevenZip = extractor.LocateExecutable(ExeFolder());
  if (!sevenZip.ok()) {
    PostLog(hwnd, L"Ошибка 7-Zip: " + Widen(sevenZip.error()));
    g_workerRunning = false;
    PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
    return;
  }

  const bool extracted = ExtractArchiveChain(hwnd, extractor, sevenZip.value(), std::move(archiveFirstPart), std::move(unpackFolder));
  PostProgress(hwnd, extracted ? 100 : 0);
  g_workerRunning = false;
  PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
}

void UnpackOnly(HWND hwnd) {
  if (g_workerRunning) {
    AppendLog(L"Установщик уже запущен.");
    return;
  }

  const auto unpackFolder = SelectedUnpackFolder();
  const auto installFolder = std::filesystem::path(GetText(g_installEdit));
  if (unpackFolder.empty() || installFolder.empty()) {
    AppendLog(L"Выберите диск для распаковки и папку для установки перед распаковкой.");
    return;
  }

  modlist::PathValidator validator;
  const auto unpack = validator.ValidateInstallFolder(unpackFolder);
  AppendLog(L"Unpack folder: " + PathToDisplay(unpackFolder) + L" - " + Widen(unpack.message));
  if (!unpack.ok) {
    return;
  }
  const auto install = validator.ValidateInstallFolder(installFolder);
  AppendLog(L"Install folder: " + Widen(install.message));
  if (!install.ok) {
    return;
  }

  auto archive = FindFirstArchivePart(ArchiveFolder());
  if (!archive.has_value()) {
    std::wstring manifestMessage;
    auto manifest = LoadPackageManifest(manifestMessage);
    AppendLog(manifestMessage);
    if (manifest.has_value()) {
      archive = ArchivePartFromManifest(*manifest);
    } else if (PackageManifestFileExists()) {
      return;
    }
  }
  if (!archive.has_value()) {
    AppendLog(L"Архив не найден/не скачан. Ошибка распаковки.");
    return;
  }

  g_workerRunning = true;
  g_stopRequested = false;
  SetControlsRunning(hwnd, true);
  g_closeAfterWorker = false;
  std::thread(RunUnpackWorker, hwnd, *archive, unpackFolder).detach();
}

void OpenLogFile() {
  const auto path = AppLogPath();
  ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: {
      g_mainWindow = hwnd;
      INITCOMMONCONTROLSEX controls{};
      controls.dwSize = sizeof(controls);
      controls.dwICC = ICC_PROGRESS_CLASS;
      InitCommonControlsEx(&controls);
      std::filesystem::create_directories(DataFolder() / "logs");
      std::filesystem::create_directories(PackageFolder());
      std::filesystem::create_directories(ArchiveFolder());
      std::filesystem::create_directories(DataFolder() / "tools" / "7zip");
      ApplyWindowFrameTheme(hwnd);
      std::wstring themeWarning;
      if (!g_nativeView.Initialize(hwnd, UiFolder() / "style.css", &themeWarning)) {
        return -1;
      }
      std::wstring stringsWarning;
      g_strings.Load(UiFolder() / "strings.json", &stringsWarning);
      g_statusText = UiText("status_idle", L"Ожидание | Ожидание проверки");
      SetWindowTextW(hwnd, UiText("window_title", L"Modlist Installer Beta").c_str());
      g_contentBrush = CreateSolidBrush(kContentColor);
      g_headerBrush = CreateSolidBrush(kHeaderColor);
      g_panelBrush = CreateSolidBrush(kPanelColor);
      g_footerBrush = CreateSolidBrush(kFooterColor);
      g_editBrush = CreateSolidBrush(g_nativeView.InputColor());
      g_stepLabel = CreateLabel(hwnd, L"", 16, 18, 720, 24);
      g_welcomeTitle = CreateLabel(hwnd, UiText("app_title", L"Modlist Installer Beta").c_str(), 16, 92, 720, 38);
      g_welcomeBody = CreateLabel(hwnd, L"", 16, 146, 720, 90);
      g_downloadLabel = CreateLabel(hwnd, L"Package", 16, 112, 100, 20);
      g_unpackDriveLabel = CreateLabel(hwnd, L"Unpack drive", 16, 136, 100, 20);
      g_unpackTargetLabel = CreateLabel(hwnd, UiText("unpack_target_empty", L"Выберите диск").c_str(), 160, 136, 420, 20);
      g_installLabel = CreateLabel(hwnd, L"Установка", 16, 178, 100, 20);
      g_downloadEdit = CreateEdit(hwnd, kDownloadEdit, 120, 22, 420, 25);
      g_unpackDriveCombo = CreateCombo(hwnd, kUnpackDriveCombo, 120, 132, 120, 180);
      CreateButton(hwnd, kDrivePickerButton, UiText("button_choose", L"Выберите").c_str(), 120, 132, 126, 32);
      g_installEdit = CreateEdit(hwnd, kInstallEdit, 120, 57, 420, 25);
      CreateButton(hwnd, kDownloadBrowse, UiText("button_browse", L"Обзор").c_str(), 550, 22, 88, 25);
      CreateButton(hwnd, kInstallBrowse, UiText("button_browse", L"Обзор").c_str(), 550, 57, 88, 25);
      CreateButton(hwnd, kValidateButton, UiText("button_validate", L"Проверка").c_str(), 120, 96, 120, 30);
      CreateButton(hwnd, kStartButton, UiText("button_install", L"Установить").c_str(), 252, 96, 120, 30);
      CreateButton(hwnd, kUnpackButton, UiText("button_unpack", L"Распаковка").c_str(), 384, 96, 120, 30);
      CreateButton(hwnd, kPauseButton, UiText("button_pause", L"Пауза").c_str(), 516, 96, 120, 30);
      CreateButton(hwnd, kStopButton, UiText("button_stop", L"Остановить").c_str(), 648, 96, 104, 30);
      CreateButton(hwnd, kOpenLogButton, UiText("button_open_log", L"Открыть лог").c_str(), 16, 450, 112, 30);
      g_previousButton = CreateButton(hwnd, kPreviousButton, UiText("button_previous", L"Предыдущий").c_str(), 632, 450, 100, 30);
      g_nextButton = CreateButton(hwnd, kNextButton, UiText("button_next", L"Дальше").c_str(), 744, 450, 100, 30);
      g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                   16, 142, 622, 20, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgress)), g_instance, nullptr);
      g_statusLabel = CreateWindowExW(0, L"STATIC", g_statusText.c_str(),
                                      WS_CHILD | WS_VISIBLE,
                                      16, 170, 622, 22, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabel)), g_instance, nullptr);
      g_logEdit = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                  16, 177, 622, 258, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLogEdit)), g_instance, nullptr);
      RecreateUiFonts(hwnd);
      SetWindowTheme(g_unpackDriveCombo, L"DarkMode_CFD", nullptr);
      SetWindowTheme(g_installEdit, L"DarkMode_Explorer", nullptr);
      SetWindowTheme(g_logEdit, L"DarkMode_Explorer", nullptr);
      const int comboItemHeight = MulDiv(
          static_cast<int>(g_nativeView.Theme().controlHeight) - 2,
          static_cast<int>(GetDpiForWindow(hwnd)), 96);
      SendMessageW(g_unpackDriveCombo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), comboItemHeight);
      SendMessageW(g_unpackDriveCombo, CB_SETITEMHEIGHT, 0, comboItemHeight);
      SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
      SendMessageW(g_progress, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(RGB(159, 196, 216)));
      SendMessageW(g_progress, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(kContentColor));
      SetText(g_downloadEdit, PackageFolder().wstring());
      PopulateDriveCombo();
      SetText(g_installEdit, L"");
      AppendLog(L"App log: " + PathToDisplay(AppLogPath()));
      AppendLog(L"Native CSS theme: " + PathToDisplay(UiFolder() / "style.css"));
      AppendLog(L"Native UI strings: " + PathToDisplay(UiFolder() / "strings.json"));
      if (!themeWarning.empty()) {
        AppendLog(themeWarning);
      }
      if (!stringsWarning.empty()) {
        AppendLog(stringsWarning);
      }
      AppendLog(L"Manifest auto-detected at: " + PathToDisplay(ManifestPath()));
      AppendLog(L"Archive parts are detected in: " + PathToDisplay(ArchiveFolder()));
      std::wstring manifestMessage;
      auto manifest = LoadPackageManifest(manifestMessage);
      AppendLog(manifestMessage);
      if (manifest.has_value()) {
        g_archiveFolderName = Widen(manifest->archiveName);
        AppendLog(L"Manifest archive size: " + FormatBytes(ManifestRequiredBytes(*manifest)));
        AppendLog(L"Manifest archive entry: " + PathToDisplay(manifest->extract.firstArchivePart));
        AppendLog(L"Папка сборки: " + g_archiveFolderName);
        if (manifest->unpackedSize > 0) {
          AppendLog(L"Unpacked payload size: " + FormatBytes(manifest->unpackedSize));
        }
      } else {
        auto package = ReadPackageFromUi();
        if (!package.ok()) {
          AppendLog(L"Package auto-detect: " + Widen(package.error()));
        }
      }
      AppendLog(L"Выберите диск для распаковки; папка будет создана как <drive>:\\Unpacked.");
      Layout(hwnd);
      ShowWizardPage(hwnd, WizardPage::Activity);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      BeginPaint(hwnd, &paint);
      const auto unpackFolder = SelectedUnpackFolder();
      const auto finalFolder = FinalInstallFolder(std::filesystem::path(GetText(g_installEdit)));
      modlist::NativeInstallerViewState state;
      state.title = UiText("app_title", L"Modlist Installer Beta");
      state.version = UiText("app_version", L"Modlist Installer v0.3.0 by WallHead");
      state.unpackNote = UiText(
          "unpack_note",
          L"Распаковка должна происходить по короткому пути. После распаковки установщик перенесет все файлы в папку установки.");
      state.unpackDriveLabel = UiText("unpack_drive_label", L"Диск для распаковки");
      state.installFolderLabel = UiText("install_folder_label", L"Папка установки");
      state.finalPathLabel = UiText("final_path_label", L"Итоговый путь");
      state.unpackTarget = unpackFolder.empty()
                               ? UiText("unpack_target_empty", L"Выберите диск")
                               : g_strings.Format("unpack_target_format", L"Папка: {path}",
                                                  {{L"path", unpackFolder.wstring()}});
      state.finalInstallFolder = finalFolder.empty()
                                     ? UiText("final_path_empty", L"Выберите папку установки")
                                     : finalFolder.wstring();
      state.status = g_statusText;
      state.progress = g_progressPercent;
      g_nativeView.Paint(hwnd, state, &paint.rcPaint);
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      HWND control = reinterpret_cast<HWND>(lParam);
      SetBkMode(dc, TRANSPARENT);
      if (control == g_logEdit) {
        SetBkMode(dc, OPAQUE);
        SetTextColor(dc, g_nativeView.MutedColor());
        SetBkColor(dc, g_nativeView.InputColor());
        return reinterpret_cast<LRESULT>(g_editBrush);
      }
      if (control == g_stepLabel) {
        SetTextColor(dc, kAccentTextColor);
        return reinterpret_cast<LRESULT>(g_headerBrush);
      } else if (control == g_welcomeTitle) {
        SetTextColor(dc, kPrimaryTextColor);
      } else if (control == g_welcomeBody || control == g_statusLabel || control == g_unpackTargetLabel) {
        SetTextColor(dc, kMutedTextColor);
      } else {
        SetTextColor(dc, kPrimaryTextColor);
      }
      return reinterpret_cast<LRESULT>(g_panelBrush);
    }
    case WM_CTLCOLOREDIT: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, g_nativeView.TextColor());
      SetBkColor(dc, g_nativeView.InputColor());
      return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_CTLCOLORLISTBOX: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, g_nativeView.TextColor());
      SetBkColor(dc, g_nativeView.InputColor());
      return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_MEASUREITEM: {
      auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
      if (item != nullptr && item->CtlType == ODT_COMBOBOX) {
        item->itemHeight = MulDiv(
            static_cast<int>(g_nativeView.Theme().controlHeight) - 2,
            static_cast<int>(GetDpiForWindow(hwnd)), 96);
        return TRUE;
      }
      return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (item != nullptr && item->CtlType == ODT_BUTTON) {
        DrawNsisButton(*item);
        return TRUE;
      }
      if (item != nullptr && item->CtlType == ODT_COMBOBOX) {
        return g_nativeView.DrawComboItem(*item) ? TRUE : FALSE;
      }
      return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    case WM_SIZE:
      g_nativeView.Resize(LOWORD(lParam), HIWORD(lParam));
      Layout(hwnd);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_DPICHANGED: {
      g_nativeView.DiscardDeviceResources();
      const auto* suggested = reinterpret_cast<const RECT*>(lParam);
      if (suggested != nullptr) {
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left,
                     suggested->bottom - suggested->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
      }
      RecreateUiFonts(hwnd);
      const int comboItemHeight = MulDiv(
          static_cast<int>(g_nativeView.Theme().controlHeight) - 2,
          static_cast<int>(GetDpiForWindow(hwnd)), 96);
      SendMessageW(g_unpackDriveCombo, CB_SETITEMHEIGHT,
                   static_cast<WPARAM>(-1), comboItemHeight);
      SendMessageW(g_unpackDriveCombo, CB_SETITEMHEIGHT, 0, comboItemHeight);
      Layout(hwnd);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
      const UINT dpi = GetDpiForWindow(hwnd);
      info->ptMinTrackSize.x = MulDiv(760, static_cast<int>(dpi), 96);
      info->ptMinTrackSize.y = MulDiv(600, static_cast<int>(dpi), 96);
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == kDownloadBrowse) {
        AppendLog(L"Package folder selection is disabled; manifest is loaded beside the executable.");
      } else if (id == kInstallBrowse) {
        if (auto path = PickFolder(hwnd)) {
          SetText(g_installEdit, path->wstring());
          SendUiPath(L"installFolder", path->wstring());
        }
      } else if (id == kUnpackDriveCombo && HIWORD(wParam) == CBN_SELCHANGE) {
        UpdateUnpackTargetLabel();
      } else if (id == kDrivePickerButton) {
        ShowDriveMenu(hwnd);
      } else if (id == kInstallEdit && HIWORD(wParam) == EN_CHANGE) {
        InvalidateRect(hwnd, nullptr, FALSE);
      } else if (id == kOpenLogButton) {
        OpenLogFile();
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
      } else if (id == kPreviousButton) {
        GoToPreviousPage(hwnd);
      } else if (id == kNextButton) {
        GoToNextPage(hwnd);
      }
      SendUiState();
      return 0;
    }
    case kLogMessage: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      AppendLog(*text);
      return 0;
    }
    case kProgressMessage:
      SendMessageW(g_progress, PBM_SETPOS, static_cast<int>(wParam), 0);
      SendUiProgress(static_cast<int>(wParam), g_statusText);
      return 0;
    case kStatusMessage: {
      std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
      SetWindowTextW(g_statusLabel, text->c_str());
      SendUiStatus(*text);
      return 0;
    }
    case kValidationFailedMessage:
      SendUiError(UiText("validation_error_title", L"Ошибка проверки"),
                  UiText("validation_error_message",
                         L"Проверка завершилась ошибкой. Перехешируйте торрент файлы."));
      return 0;
    case kWorkerFinishedMessage: {
      SetControlsRunning(hwnd, false);
      if (wParam != 0 && !g_closeAfterWorker) {
        g_installCompleted = true;
        SetWindowTextW(GetDlgItem(hwnd, kStartButton), UiText("button_close", L"Закрыть").c_str());
        SendUiState();
        ShowThemedMessage(hwnd, UiText("window_title", L"Modlist Installer Beta"),
                          UiText("install_complete_message", L"Установка завершена!"));
      } else {
        SendUiState();
      }
      if (g_closeAfterWorker) {
        DestroyWindow(hwnd);
      }
      return 0;
    }
    case WM_CLOSE:
      if (g_workerRunning) {
        g_closeAfterWorker = true;
        StopInstall();
        SetWindowTextW(hwnd, UiText("window_title_stopping", L"Modlist Installer Beta - остановка...").c_str());
        AppendLog(L"Ожидание остановки проверки перед закрытием.");
        return 0;
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      DeleteObject(g_contentBrush);
      DeleteObject(g_headerBrush);
      DeleteObject(g_panelBrush);
      DeleteObject(g_footerBrush);
      DeleteObject(g_editBrush);
      DeleteObject(g_stepFont);
      DeleteObject(g_titleFont);
      DeleteObject(g_bodyFont);
      DeleteObject(g_labelFont);
      g_nativeView.DiscardDeviceResources();
      g_mainWindow = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wParam, lParam);
  }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  g_instance = instance;
  OleInitialize(nullptr);

  const wchar_t className[] = L"ModlistInstallerWindow";
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = WindowProc;
  windowClass.hInstance = instance;
  windowClass.lpszClassName = className;
  windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_MODLIST_INSTALLER));
  windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  RegisterClassW(&windowClass);

  constexpr DWORD windowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
  const UINT dpi = GetDpiForSystem();
  RECT windowRect{0, 0, MulDiv(920, static_cast<int>(dpi), 96),
                  MulDiv(660, static_cast<int>(dpi), 96)};
  AdjustWindowRectExForDpi(&windowRect, windowStyle, FALSE, 0, dpi);
  HWND hwnd = CreateWindowExW(0, className, L"Modlist Installer Beta",
                              windowStyle,
                              CW_USEDEFAULT, CW_USEDEFAULT,
                              windowRect.right - windowRect.left,
                              windowRect.bottom - windowRect.top,
                              nullptr, nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    OleUninitialize();
    return 1;
  }

  RECT currentRect{};
  GetWindowRect(hwnd, &currentRect);
  const UINT windowDpi = GetDpiForWindow(hwnd);
  RECT initialRect{0, 0, MulDiv(920, static_cast<int>(windowDpi), 96),
                   MulDiv(660, static_cast<int>(windowDpi), 96)};
  AdjustWindowRectExForDpi(&initialRect, windowStyle, FALSE, 0, windowDpi);
  SetWindowPos(hwnd, nullptr, currentRect.left, currentRect.top,
               initialRect.right - initialRect.left,
               initialRect.bottom - initialRect.top,
               SWP_NOACTIVATE | SWP_NOZORDER);

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
