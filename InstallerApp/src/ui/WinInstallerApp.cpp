#define NOMINMAX

#include "app/PackageDiscovery.h"
#include "downloader/LibtorrentDownloader.h"
#include "extractor/SevenZipExtractor.h"
#include "manifest/Manifest.h"
#include "paths/PathValidator.h"
#include "resource.h"
#include "verifier/Verifier.h"

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
#include <limits>
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
constexpr int kPreviousButton = 1015;
constexpr int kNextButton = 1016;
constexpr int kUnpackDriveCombo = 1017;

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
WizardPage g_page = WizardPage::Welcome;
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
std::mutex g_downloaderMutex;
std::shared_ptr<modlist::LibtorrentDownloader> g_activeDownloader;

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

void PostValidationFailed(HWND hwnd) {
  PostMessageW(hwnd, kValidationFailedMessage, 0, 0);
}

std::wstring PathToDisplay(const std::filesystem::path& path) {
  return path.wstring();
}

std::filesystem::path ExeFolder() {
  return ModuleFolder();
}

std::filesystem::path ArchiveFolder() {
  return ExeFolder();
}

std::filesystem::path PackageFolder() {
  return ExeFolder() / "package";
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

HWND CreateCombo(HWND parent, int id, int x, int y, int width, int height) {
  return CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
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
  return std::filesystem::path(drive) / "Sky";
}

void UpdateUnpackTargetLabel() {
  if (g_unpackTargetLabel == nullptr) {
    return;
  }
  const auto folder = SelectedUnpackFolder();
  if (folder.empty()) {
    SetText(g_unpackTargetLabel, L"Target: choose a drive");
  } else {
    SetText(g_unpackTargetLabel, L"Target: " + folder.wstring());
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

LRESULT CALLBACK ButtonSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData) {
  (void)subclassId;
  (void)refData;
  switch (message) {
    case WM_MOUSEMOVE: {
      if (g_hotButton != hwnd) {
        if (g_hotButton != nullptr) {
          InvalidateRect(g_hotButton, nullptr, TRUE);
        }
        g_hotButton = hwnd;
        InvalidateRect(hwnd, nullptr, TRUE);
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
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      break;
    case WM_ENABLE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      InvalidateRect(hwnd, nullptr, TRUE);
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

HFONT CreateUiFont(int pointSize, int weight = FW_NORMAL, const wchar_t* family = L"Segoe UI") {
  HDC screen = GetDC(nullptr);
  const int height = -MulDiv(pointSize, GetDeviceCaps(screen, LOGPIXELSY), 72);
  ReleaseDC(nullptr, screen);
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
  const bool disabled = (item.itemState & ODS_DISABLED) != 0;
  const bool pressed = (item.itemState & ODS_SELECTED) != 0;
  const bool focused = (item.itemState & ODS_FOCUS) != 0;
  const bool hot = item.hwndItem == g_hotButton && !disabled;
  const int controlId = GetDlgCtrlID(item.hwndItem);
  const bool primary = controlId == kStartButton || controlId == kNextButton;
  const bool danger = controlId == kStopButton;

  RECT rect = item.rcItem;
  COLORREF top = kButtonTopColor;
  COLORREF bottom = kButtonBottomColor;
  COLORREF border = kButtonBorderColor;
  COLORREF textColor = kPrimaryTextColor;
  if (disabled) {
    top = RGB(22, 27, 30);
    bottom = RGB(18, 22, 24);
    border = kLineColor;
    textColor = kButtonDisabledTextColor;
  } else if (pressed) {
    top = kButtonPressedTopColor;
    bottom = kButtonPressedBottomColor;
    border = primary ? kAccentTextColor : kStrongLineColor;
  } else if (hot) {
    top = kButtonHoverTopColor;
    bottom = kButtonHoverBottomColor;
    border = danger ? kDangerColor : kAccentTextColor;
  } else if (primary) {
    top = RGB(27, 40, 45);
    bottom = RGB(23, 32, 37);
    border = kAccentTextColor;
    textColor = kAccentStrongColor;
  }

  FillVerticalGradient(item.hDC, rect, top, bottom);

  HPEN borderPen = CreatePen(PS_SOLID, 1, border);
  HPEN oldPen = static_cast<HPEN>(SelectObject(item.hDC, borderPen));
  HBRUSH oldBrush = static_cast<HBRUSH>(SelectObject(item.hDC, GetStockObject(NULL_BRUSH)));
  Rectangle(item.hDC, rect.left, rect.top, rect.right, rect.bottom);
  SelectObject(item.hDC, oldBrush);
  SelectObject(item.hDC, oldPen);
  DeleteObject(borderPen);

  HPEN lightPen = CreatePen(PS_SOLID, 1, pressed ? kLineColor : RGB(52, 61, 66));
  HPEN shadowPen = CreatePen(PS_SOLID, 1, pressed ? kStrongLineColor : RGB(9, 12, 13));
  oldPen = static_cast<HPEN>(SelectObject(item.hDC, lightPen));
  MoveToEx(item.hDC, rect.left + 1, rect.bottom - 2, nullptr);
  LineTo(item.hDC, rect.left + 1, rect.top + 1);
  LineTo(item.hDC, rect.right - 2, rect.top + 1);
  SelectObject(item.hDC, shadowPen);
  MoveToEx(item.hDC, rect.left + 2, rect.bottom - 2, nullptr);
  LineTo(item.hDC, rect.right - 2, rect.bottom - 2);
  LineTo(item.hDC, rect.right - 2, rect.top + 1);
  SelectObject(item.hDC, oldPen);
  DeleteObject(lightPen);
  DeleteObject(shadowPen);

  wchar_t text[128]{};
  GetWindowTextW(item.hwndItem, text, static_cast<int>(sizeof(text) / sizeof(text[0])));
  RECT textRect = rect;
  if (pressed) {
    OffsetRect(&textRect, 1, 1);
  }

  SetBkMode(item.hDC, TRANSPARENT);
  SetTextColor(item.hDC, textColor);
  HFONT oldFont = static_cast<HFONT>(SelectObject(item.hDC, g_bodyFont));
  DrawTextW(item.hDC, text, -1, &textRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(item.hDC, oldFont);

  if (focused && !disabled) {
    RECT focusRect = rect;
    InflateRect(&focusRect, -4, -4);
    HPEN focusPen = CreatePen(PS_DOT, 1, kAccentTextColor);
    HPEN oldFocusPen = static_cast<HPEN>(SelectObject(item.hDC, focusPen));
    HBRUSH oldFocusBrush = static_cast<HBRUSH>(SelectObject(item.hDC, GetStockObject(NULL_BRUSH)));
    Rectangle(item.hDC, focusRect.left, focusRect.top, focusRect.right, focusRect.bottom);
    SelectObject(item.hDC, oldFocusBrush);
    SelectObject(item.hDC, oldFocusPen);
    DeleteObject(focusPen);
  }
}

void ShowControl(HWND hwnd, bool visible) {
  if (hwnd != nullptr) {
    ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
  }
}

void ShowControl(HWND parent, int id, bool visible) {
  ShowControl(GetDlgItem(parent, id), visible);
}

std::wstring WizardPageTitle(WizardPage page) {
  switch (page) {
    case WizardPage::Welcome:
      return L"Step 1 of 3 - Welcome";
    case WizardPage::Folders:
      return L"Step 2 of 3 - Folders";
    case WizardPage::Activity:
      return L"Step 3 of 3 - Validation and install";
  }
  return L"Modlist Installer";
}

void Layout(HWND hwnd) {
  RECT rect{};
  GetClientRect(hwnd, &rect);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int margin = 16;
  const int railWidth = 116;
  const int contentX = railWidth + 28;
  const int contentRightPadding = 28;
  const int navY = height - 48;
  const int labelWidth = 115;
  const int buttonWidth = 88;
  const int rowHeight = 25;
  const int editX = contentX + labelWidth;
  const int editWidth = width - editX - buttonWidth - contentRightPadding - 8;
  const int buttonX = editX + editWidth + 8;
  const int contentWidth = width - contentX - contentRightPadding;

  MoveWindow(g_stepLabel, contentX, 24, contentWidth, 24, TRUE);

  MoveWindow(g_welcomeTitle, contentX, 96, contentWidth, 42, TRUE);
  MoveWindow(g_welcomeBody, contentX, 154, contentWidth, 96, TRUE);

  MoveWindow(g_downloadLabel, contentX, 94, labelWidth, 20, TRUE);
  MoveWindow(GetDlgItem(hwnd, kDownloadEdit), editX, 90, editWidth, rowHeight, TRUE);
  MoveWindow(GetDlgItem(hwnd, kDownloadBrowse), buttonX, 90, buttonWidth, rowHeight, TRUE);
  MoveWindow(g_unpackDriveLabel, contentX, 94, labelWidth, 20, TRUE);
  MoveWindow(g_unpackDriveCombo, editX, 90, 120, 180, TRUE);
  MoveWindow(g_unpackTargetLabel, editX + 136, 94, editWidth - 136 + buttonWidth + 8, 20, TRUE);
  MoveWindow(g_installLabel, contentX, 136, labelWidth, 20, TRUE);
  MoveWindow(GetDlgItem(hwnd, kInstallEdit), editX, 132, editWidth, rowHeight, TRUE);
  MoveWindow(GetDlgItem(hwnd, kInstallBrowse), buttonX, 132, buttonWidth, rowHeight, TRUE);

  MoveWindow(GetDlgItem(hwnd, kValidateButton), contentX, 74, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kStartButton), contentX + 132, 74, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kUnpackButton), contentX + 264, 74, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kPauseButton), contentX + 396, 74, 120, 30, TRUE);
  MoveWindow(GetDlgItem(hwnd, kStopButton), contentX + 264, 74, 92, 30, TRUE);
  MoveWindow(g_progress, contentX, 122, contentWidth, 20, TRUE);
  MoveWindow(g_statusLabel, contentX, 150, contentWidth, 22, TRUE);
  MoveWindow(g_logEdit, contentX, 182, contentWidth, navY - 198, TRUE);

  MoveWindow(g_previousButton, width - 228, navY, 100, 30, TRUE);
  MoveWindow(g_nextButton, width - 116, navY, 100, 30, TRUE);
}

std::wstring StageToText(modlist::DownloadStage stage);
std::wstring FormatBytes(uintmax_t bytes);
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
    message = L"No packer manifest found at: " + PathToDisplay(path);
    return std::nullopt;
  }

  modlist::ManifestLoader loader;
  auto manifest = loader.LoadFromFile(path);
  if (!manifest.ok()) {
    message = L"Manifest error: " + Widen(manifest.error());
    return std::nullopt;
  }

  message = L"Manifest loaded: " + PathToDisplay(path);
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
  PostLog(hwnd, L"Verifying manifest SHA-256 for " + std::to_wstring(manifest.files.size()) + L" archive file(s)...");
  modlist::Verifier verifier;
  const auto summary = verifier.Verify(ArchiveFolder(), manifest.files);
  if (summary.ok) {
    PostLog(hwnd, L"Manifest verification completed.");
    return true;
  }

  for (const auto& file : summary.files) {
    if (file.message != "OK") {
      PostLog(hwnd, L"Manifest verification failed for " + PathToDisplay(file.path) + L": " + Widen(file.message));
    }
  }
  PostValidationFailed(hwnd);
  return false;
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
  std::wstring manifestMessage;
  auto manifest = LoadPackageManifest(manifestMessage);
  if (manifest.has_value()) {
    const uintmax_t manifestBytes = ManifestRequiredBytes(*manifest);
    AppendLog(manifestMessage);
    AppendLog(L"Manifest archive size: " + FormatBytes(manifestBytes));
    return manifestBytes;
  }

  auto torrentSize = modlist::LibtorrentDownloader::ReadTorrentPayloadSize(package.torrentFile);
  if (torrentSize.ok()) {
    AppendLog(L"Torrent payload size: " + FormatBytes(torrentSize.value()));
    return torrentSize.value();
  }

  AppendLog(L"Torrent size warning: " + Widen(torrentSize.error()));
  const uintmax_t archiveBytes = EstimateNearbyArchiveBytes(ArchiveFolder());
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

std::wstring FormatDownloadStatus(const modlist::DownloadStatus& status, int elapsedSeconds = -1) {
  std::wostringstream out;
  out << StageToText(status.stage)
      << L" | " << static_cast<int>(status.progress * 100.0f) << L"%";
  if (status.totalBytes > 0) {
    out << L" | " << FormatBytes(static_cast<uintmax_t>(status.downloadedBytes))
        << L" / " << FormatBytes(static_cast<uintmax_t>(status.totalBytes));
    if (status.downloadRateBytesPerSecond > 0) {
      out << L" | " << FormatBytesPerSecond(status.downloadRateBytesPerSecond);
    }
    if (status.etaSeconds >= 0) {
      out << L" | ETA " << FormatEta(status.etaSeconds);
    }
  }
  if (elapsedSeconds >= 0) {
    out << L" | Elapsed " << FormatEta(elapsedSeconds);
  }
  return out.str();
}

void ShowWizardPage(HWND hwnd, WizardPage page) {
  g_page = page;
  SetText(g_stepLabel, WizardPageTitle(page));

  const bool welcome = page == WizardPage::Welcome;
  const bool folders = page == WizardPage::Folders;
  const bool activity = page == WizardPage::Activity;
  const bool running = g_workerRunning.load();

  ShowControl(g_welcomeTitle, welcome);
  ShowControl(g_welcomeBody, welcome);

  ShowControl(g_downloadLabel, false);
  ShowControl(hwnd, kDownloadEdit, false);
  ShowControl(hwnd, kDownloadBrowse, false);
  ShowControl(g_unpackDriveLabel, folders);
  ShowControl(g_unpackDriveCombo, folders);
  ShowControl(g_unpackTargetLabel, folders);
  ShowControl(g_installLabel, folders);
  ShowControl(hwnd, kInstallEdit, folders);
  ShowControl(hwnd, kInstallBrowse, folders);

  ShowControl(hwnd, kValidateButton, activity);
  ShowControl(hwnd, kStartButton, activity);
  ShowControl(hwnd, kUnpackButton, false);
  ShowControl(hwnd, kPauseButton, false);
  ShowControl(hwnd, kStopButton, activity);
  ShowControl(g_progress, activity);
  ShowControl(g_statusLabel, activity);
  ShowControl(g_logEdit, activity);

  EnableWindow(g_previousButton, page != WizardPage::Welcome && !running);
  EnableWindow(g_nextButton, page != WizardPage::Activity && !running);

  EnableWindow(GetDlgItem(hwnd, kDownloadBrowse), false);
  EnableWindow(g_unpackDriveCombo, folders && !running);
  EnableWindow(GetDlgItem(hwnd, kInstallBrowse), folders && !running);
  EnableWindow(GetDlgItem(hwnd, kValidateButton), activity && !running);
  EnableWindow(GetDlgItem(hwnd, kStartButton), activity && !running);
  EnableWindow(GetDlgItem(hwnd, kUnpackButton), false);
  EnableWindow(GetDlgItem(hwnd, kPauseButton), false);
  EnableWindow(GetDlgItem(hwnd, kStopButton), activity && running);
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
    out << L" | ETA " << FormatEta(etaSeconds);
  }
  if (elapsedSeconds >= 0) {
    out << L" | Elapsed " << FormatEta(elapsedSeconds);
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
  PostLog(hwnd, L"Extracting: " + PathToDisplay(extraction.archiveFirstPart));
  PostProgress(hwnd, progressBase);
  PostStatus(hwnd, FormatExtractionStatus(statusLabel, 0, 0, -1, 0));
  int lastPercent = -1;
  const auto startedAt = std::chrono::steady_clock::now();
  const auto result = extractor.Extract(extraction, [hwnd, statusLabel, progressBase, progressSpan, estimatedBytes, startedAt, &lastPercent](int percent) {
    if (percent == lastPercent) {
      return;
    }
    lastPercent = percent;
    const int mapped = progressBase + (percent * progressSpan) / 100;
    uintmax_t speed = 0;
    int eta = -1;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    if (estimatedBytes > 0 && elapsed > 0 && percent > 0 && percent < 100) {
      const uintmax_t processed = (estimatedBytes * static_cast<uintmax_t>(percent)) / 100;
      speed = processed / static_cast<uintmax_t>(elapsed);
      if (speed > 0 && estimatedBytes > processed) {
        eta = static_cast<int>((estimatedBytes - processed) / speed);
      }
    }
    PostProgress(hwnd, mapped);
    PostStatus(hwnd, FormatExtractionStatus(statusLabel, percent, speed, eta, static_cast<int>(elapsed)));
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
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt).count();
    PostStatus(hwnd, FormatExtractionStatus(statusLabel, 100, 0, -1, static_cast<int>(elapsed)));
  }
  return result.ok;
}

bool ExtractArchiveChain(HWND hwnd,
                         modlist::SevenZipExtractor& extractor,
                         const std::filesystem::path& sevenZipExe,
                         std::filesystem::path archiveFirstPart,
                         std::filesystem::path installFolder,
                         int progressBase = 0,
                         int progressSpan = 100) {
  modlist::ExtractionConfig extraction;
  extraction.sevenZipExe = sevenZipExe;
  extraction.archiveFirstPart = std::move(archiveFirstPart);
  extraction.installFolder = installFolder;
  extraction.useSameDiskTemp = true;

  const bool splitArchive = IsFirstSplitArchivePart(extraction.archiveFirstPart);
  const int firstSpan = splitArchive ? progressSpan / 2 : progressSpan;
  std::error_code sizeEc;
  uintmax_t firstBytes = 0;
  if (splitArchive) {
    firstBytes = EstimateNearbyArchiveBytes(extraction.archiveFirstPart.parent_path());
  } else {
    firstBytes = std::filesystem::file_size(extraction.archiveFirstPart, sizeEc);
    if (sizeEc) {
      firstBytes = 0;
    }
  }
  if (!RunExtractionStep(hwnd, extractor, extraction, L"Unpacking", progressBase, firstSpan, firstBytes)) {
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
  const int secondSpan = progressSpan - firstSpan;
  std::error_code ec;
  const uintmax_t secondBytes = std::filesystem::file_size(extraction.archiveFirstPart, ec);
  return RunExtractionStep(hwnd, extractor, extraction, L"Unpacking inner archive", progressBase + firstSpan, secondSpan, ec ? 0 : secondBytes);
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

uintmax_t EstimateInstallBytes(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) || ec) {
    return 0;
  }
  if (std::filesystem::is_regular_file(path, ec) && !ec) {
    return std::filesystem::file_size(path, ec);
  }
  if (!std::filesystem::is_directory(path, ec) || ec) {
    return 0;
  }

  uintmax_t total = 0;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec)) {
    if (ec) {
      break;
    }
    if (entry.is_regular_file(ec) && !ec) {
      total += entry.file_size(ec);
      if (ec) {
        ec.clear();
      }
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
  status << L"Installing " << percent << L"%";
  if (progress.totalBytes > 0) {
    status << L" | " << FormatBytes(progress.doneBytes) << L" / " << FormatBytes(progress.totalBytes);
  }
  if (speed > 0) {
    status << L" | " << FormatBytesPerSecond(speed);
  }
  if (eta >= 0) {
    status << L" | ETA " << FormatEta(eta);
  }
  status << L" | Elapsed " << FormatEta(static_cast<int>(elapsed));

  PostProgress(progress.hwnd, 95 + (percent * 5) / 100);
  PostStatus(progress.hwnd, status.str());
}

bool MoveWholeEntry(const std::filesystem::path& source,
                    const std::filesystem::path& target,
                    std::wstring& error) {
  std::error_code ec;
  std::filesystem::create_directories(target.parent_path(), ec);
  if (ec) {
    error = L"Unable to create install folder: " + Widen(ec.message());
    return false;
  }

  DWORD flags = MOVEFILE_WRITE_THROUGH;
  if (!std::filesystem::is_directory(source, ec)) {
    flags |= MOVEFILE_REPLACE_EXISTING;
  }
  if (!MoveFileExW(source.wstring().c_str(), target.wstring().c_str(), flags)) {
    error = L"Unable to move " + PathToDisplay(source) + L" to " + PathToDisplay(target) +
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
    error = L"Unable to create install folder: " + Widen(ec.message());
    return false;
  }

  std::ifstream input(source, std::ios::binary);
  if (!input) {
    error = L"Unable to read unpacked file: " + PathToDisplay(source);
    return false;
  }
  std::ofstream output(target, std::ios::binary | std::ios::trunc);
  if (!output) {
    error = L"Unable to write install file: " + PathToDisplay(target);
    return false;
  }

  std::vector<char> buffer(kBufferSize);
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto read = input.gcount();
    if (read <= 0) {
      break;
    }
    output.write(buffer.data(), read);
    if (!output) {
      error = L"Unable to write install file: " + PathToDisplay(target);
      return false;
    }
    UpdateInstallProgress(progress, static_cast<uintmax_t>(read));
  }
  if (!input.eof()) {
    error = L"Unable to copy unpacked file: " + PathToDisplay(source);
    return false;
  }
  output.close();
  if (!output) {
    error = L"Unable to finish writing install file: " + PathToDisplay(target);
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
  std::error_code ec;
  if (sameDrive && !std::filesystem::exists(target, ec)) {
    const uintmax_t movedBytes = EstimateInstallBytes(source);
    if (!MoveWholeEntry(source, target, error)) {
      return false;
    }
    UpdateInstallProgress(progress, movedBytes, true);
    return true;
  }

  if (std::filesystem::is_directory(source, ec) && !ec) {
    std::filesystem::create_directories(target, ec);
    if (ec) {
      error = L"Unable to create install folder: " + Widen(ec.message());
      return false;
    }
    for (const auto& entry : std::filesystem::directory_iterator(source, ec)) {
      if (ec) {
        error = L"Unable to read unpacked folder: " + Widen(ec.message());
        return false;
      }
      if (!InstallEntry(hwnd, entry.path(), target / entry.path().filename(), sameDrive, progress, error)) {
        return false;
      }
    }
    std::filesystem::remove(source, ec);
    if (ec) {
      error = L"Unable to remove installed source folder: " + Widen(ec.message());
      return false;
    }
    return true;
  }

  if (sameDrive) {
    const uintmax_t movedBytes = EstimateInstallBytes(source);
    if (!MoveWholeEntry(source, target, error)) {
      return false;
    }
    UpdateInstallProgress(progress, movedBytes, true);
    return true;
  }

  if (!CopyFileWithProgress(source, target, progress, error)) {
    return false;
  }
  std::filesystem::remove(source, ec);
  if (ec) {
    error = L"Unable to remove copied unpacked file: " + Widen(ec.message());
    return false;
  }
  return true;
}

bool InstallExtractedFiles(HWND hwnd, const std::filesystem::path& unpackFolder, const std::filesystem::path& installFolder) {
  PostProgress(hwnd, 95);
  PostStatus(hwnd, L"Installing 0%");
  if (IsSameFolder(unpackFolder, installFolder)) {
    PostLog(hwnd, L"Install folder is the unpack folder; no move is needed.");
    PostProgress(hwnd, 100);
    PostStatus(hwnd, L"Installing 100%");
    return true;
  }
  if (IsChildFolder(installFolder, unpackFolder)) {
    PostLog(hwnd, L"Install folder cannot be inside the unpack folder.");
    return false;
  }

  std::error_code ec;
  std::filesystem::create_directories(installFolder, ec);
  if (ec) {
    PostLog(hwnd, L"Unable to create install folder: " + Widen(ec.message()));
    return false;
  }

  modlist::PathValidator validator;
  const bool sameDrive = validator.IsSameDrive(unpackFolder, installFolder);
  PostLog(hwnd, sameDrive ? L"Installing with same-drive cut/move; files will not be copied."
                          : L"Installing across drives; Windows must copy files, then remove unpacked originals.");

  InstallProgress installProgress;
  installProgress.hwnd = hwnd;
  for (const auto& entry : std::filesystem::directory_iterator(unpackFolder, ec)) {
    if (ec) {
      PostLog(hwnd, L"Unable to read unpack folder: " + Widen(ec.message()));
      return false;
    }
    if (entry.path().filename() != ".install_temp") {
      installProgress.totalBytes += EstimateInstallBytes(entry.path());
    }
  }
  PostLog(hwnd, L"Install payload size: " + FormatBytes(installProgress.totalBytes));
  UpdateInstallProgress(installProgress, 0, true);

  std::wstring error;
  for (const auto& entry : std::filesystem::directory_iterator(unpackFolder, ec)) {
    if (ec) {
      PostLog(hwnd, L"Unable to read unpack folder: " + Widen(ec.message()));
      return false;
    }
    if (entry.path().filename() == ".install_temp") {
      std::filesystem::remove_all(entry.path(), ec);
      if (ec) {
        PostLog(hwnd, L"Unable to remove extraction temp folder: " + Widen(ec.message()));
        return false;
      }
      continue;
    }
    const auto target = installFolder / entry.path().filename();
    if (!InstallEntry(hwnd, entry.path(), target, sameDrive, installProgress, error)) {
      PostLog(hwnd, error);
      return false;
    }
  }

  PostProgress(hwnd, 100);
  PostStatus(hwnd, L"Installing 100%");
  PostLog(hwnd, L"Install step completed.");
  return true;
}

void RunInstallWorker(HWND hwnd,
                      modlist::PackageDiscovery package,
                      std::filesystem::path unpackFolder,
                      std::filesystem::path installFolder,
                      std::shared_ptr<modlist::LibtorrentDownloader> downloader) {
  g_workerRunning = true;
  PostProgress(hwnd, 0);
  PostLog(hwnd, L"Starting local package validation...");
  PostLog(hwnd, L"The torrent will validate files already beside the installer. Missing files stop the install.");

  modlist::DownloadConfig config;
  config.torrent.type = modlist::TorrentSourceType::TorrentFile;
  config.torrent.source = package.torrentFile.string();
  config.downloadFolder = ArchiveFolder();
  config.features.enableDht = false;
  config.features.enablePex = false;
  config.features.enableLsd = false;

  downloader->StartLocalValidation(config);
  modlist::DownloadStage lastStage = modlist::DownloadStage::Idle;
  const auto validationStartedAt = std::chrono::steady_clock::now();
  while (true) {
    auto status = downloader->GetStatus();
    if (status.stage != lastStage) {
      lastStage = status.stage;
      PostLog(hwnd, L"Validation stage: " + StageToText(status.stage));
    }
    int elapsedSeconds = 0;
    if (status.totalBytes > 0) {
      status.downloadedBytes = static_cast<int64_t>(status.totalBytes * status.progress);
      const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now() - validationStartedAt).count();
      elapsedSeconds = static_cast<int>(elapsed);
      if (elapsed > 0 && status.downloadedBytes > 0 && status.totalBytes > status.downloadedBytes) {
        const auto speed = static_cast<uintmax_t>(status.downloadedBytes / elapsed);
        if (speed > 0) {
          status.downloadRateBytesPerSecond = speed > static_cast<uintmax_t>(std::numeric_limits<int>::max())
                                                  ? std::numeric_limits<int>::max()
                                                  : static_cast<int>(speed);
          status.etaSeconds = static_cast<int>((status.totalBytes - status.downloadedBytes) / static_cast<int64_t>(speed));
        }
      }
    }
    PostProgress(hwnd, static_cast<int>(status.progress * 35.0f));
    PostStatus(hwnd, FormatDownloadStatus(status, elapsedSeconds));

    if (status.stage == modlist::DownloadStage::Completed) {
      PostLog(hwnd, L"Local package validation completed.");
      break;
    }
    if (status.stage == modlist::DownloadStage::Failed) {
      PostLog(hwnd, L"Validation error: " + Widen(status.error));
      PostValidationFailed(hwnd);
      FinishWorker(hwnd, downloader);
      return;
    }
    if (status.stage == modlist::DownloadStage::Cancelled) {
      PostLog(hwnd, L"Validation cancelled.");
      FinishWorker(hwnd, downloader);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  PostLog(hwnd, L"Releasing validated files before unpacking...");
  {
    std::lock_guard<std::mutex> lock(g_downloaderMutex);
    if (g_activeDownloader == downloader) {
      g_activeDownloader.reset();
    }
  }
  downloader->ReleaseFiles();

  std::optional<modlist::Manifest> manifest;
  {
    std::wstring manifestMessage;
    manifest = LoadPackageManifest(manifestMessage);
    PostLog(hwnd, manifestMessage);
  }
  if (!manifest.has_value() && PackageManifestFileExists()) {
    PostValidationFailed(hwnd);
    FinishWorker(hwnd, downloader);
    return;
  }
  if (manifest.has_value() && !VerifyPackageManifest(hwnd, *manifest)) {
    FinishWorker(hwnd, downloader);
    return;
  }

  PostLog(hwnd, L"Looking for archive file to unpack...");
  auto firstArchivePart = package.firstArchivePart;
  if (manifest.has_value()) {
    firstArchivePart = ArchivePartFromManifest(*manifest);
  }
  if (!firstArchivePart.has_value()) {
    firstArchivePart = FindFirstArchivePart(ArchiveFolder());
  }
  if (!firstArchivePart.has_value()) {
    PostLog(hwnd, L"No archive file found beside the installer. Cannot extract automatically.");
    FinishWorker(hwnd, downloader);
    return;
  }

  PostLog(hwnd, L"Selected install folder: " + PathToDisplay(installFolder));

  const uintmax_t archiveBytes = EstimateNearbyArchiveBytes(firstArchivePart->parent_path());
  if (archiveBytes > 0) {
    const uintmax_t unpackFree = FreeBytes(unpackFolder);
    PostLog(hwnd, L"Unpack free space: " + FormatBytes(unpackFree) + L"; archive size minimum: " + FormatBytes(archiveBytes));
    if (unpackFree < archiveBytes) {
      PostLog(hwnd, L"Not enough free space in the unpack folder for extraction.");
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

  const bool extracted = ExtractArchiveChain(hwnd, extractor, sevenZip.value(), *firstArchivePart, unpackFolder, 35, 60);
  if (!extracted) {
    PostProgress(hwnd, 0);
    FinishWorker(hwnd, downloader);
    return;
  }

  const bool installed = InstallExtractedFiles(hwnd, unpackFolder, installFolder);
  PostProgress(hwnd, installed ? 100 : 0);
  FinishWorker(hwnd, downloader);
}

bool ValidateFolders(uintmax_t knownRequiredBytes = 0) {
  modlist::PathValidator validator;
  bool ok = true;

  const auto installText = GetText(g_installEdit);
  const auto unpackFolder = SelectedUnpackFolder();
  if (!unpackFolder.empty()) {
    const auto result = validator.ValidateInstallFolder(unpackFolder, knownRequiredBytes);
    AppendLog(L"Unpack folder: " + PathToDisplay(unpackFolder) + L" - " + Widen(result.message));
    if (result.warning) {
      AppendLog(L"Warning: " + Widen(result.message));
    }
    ok = ok && result.ok;
  } else {
    AppendLog(L"Unpack drive: not selected yet.");
    ok = false;
  }

  if (!installText.empty()) {
    const auto result = validator.ValidateInstallFolder(std::filesystem::path(installText), knownRequiredBytes);
    AppendLog(L"Install folder: " + Widen(result.message));
    if (result.warning) {
      AppendLog(L"Warning: " + Widen(result.message));
    }
    ok = ok && result.ok;
  } else {
    AppendLog(L"Install folder: not selected yet.");
    ok = false;
  }

  if (!unpackFolder.empty() && !HasEnoughSpace(unpackFolder, knownRequiredBytes, L"Unpack folder")) {
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
    AppendLog(L"No archive file found beside the installer.");
  }

  std::wstring manifestMessage;
  auto manifest = LoadPackageManifest(manifestMessage);
  AppendLog(manifestMessage);
  if (manifest.has_value()) {
    AppendLog(L"Manifest files: " + std::to_wstring(manifest->files.size()));
    AppendLog(L"Manifest archive entry: " + PathToDisplay(manifest->extract.firstArchivePart));
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
  (void)running;
  SetWindowTextW(GetDlgItem(hwnd, kPauseButton), L"Pause");
  ShowWizardPage(hwnd, g_page);
}

std::shared_ptr<modlist::LibtorrentDownloader> ActiveDownloader() {
  std::lock_guard<std::mutex> lock(g_downloaderMutex);
  return g_activeDownloader;
}

void TogglePause(HWND hwnd) {
  auto downloader = ActiveDownloader();
  if (!downloader) {
    AppendLog(L"No active validation to pause.");
    return;
  }

  const auto status = downloader->GetStatus();
  if (status.stage == modlist::DownloadStage::Paused) {
    downloader->Resume();
    SetWindowTextW(GetDlgItem(hwnd, kPauseButton), L"Pause");
    AppendLog(L"Validation resumed.");
  } else {
    downloader->Pause();
    SetWindowTextW(GetDlgItem(hwnd, kPauseButton), L"Resume");
    AppendLog(L"Validation paused.");
  }
}

void StopInstall() {
  auto downloader = ActiveDownloader();
  if (!downloader) {
    AppendLog(L"No active validation to stop.");
    return;
  }
  downloader->Cancel();
  AppendLog(L"Stopping validation. Existing package files are left untouched.");
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

  const auto unpackFolder = SelectedUnpackFolder();
  const auto installFolder = std::filesystem::path(GetText(g_installEdit));
  auto downloader = std::make_shared<modlist::LibtorrentDownloader>();
  {
    std::lock_guard<std::mutex> lock(g_downloaderMutex);
    g_activeDownloader = downloader;
  }
  g_workerRunning = true;
  SetControlsRunning(hwnd, true);
  g_closeAfterWorker = false;
  std::thread(RunInstallWorker, hwnd, std::move(package.value()), unpackFolder, installFolder, downloader).detach();
}

void RunUnpackWorker(HWND hwnd, std::filesystem::path archiveFirstPart, std::filesystem::path unpackFolder) {
  g_workerRunning = true;
  PostProgress(hwnd, 0);
  PostLog(hwnd, L"Unpacking without torrent validation: " + PathToDisplay(archiveFirstPart));

  const uintmax_t archiveBytes = EstimateNearbyArchiveBytes(archiveFirstPart.parent_path());
  if (archiveBytes > 0) {
    const uintmax_t unpackFree = FreeBytes(unpackFolder);
    PostLog(hwnd, L"Unpack free space: " + FormatBytes(unpackFree) + L"; archive size minimum: " + FormatBytes(archiveBytes));
    if (unpackFree < archiveBytes) {
      PostLog(hwnd, L"Not enough free space in the unpack folder for extraction.");
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

  const bool extracted = ExtractArchiveChain(hwnd, extractor, sevenZip.value(), std::move(archiveFirstPart), std::move(unpackFolder));
  PostProgress(hwnd, extracted ? 100 : 0);
  g_workerRunning = false;
  PostMessageW(hwnd, kWorkerFinishedMessage, 0, 0);
}

void UnpackOnly(HWND hwnd) {
  if (g_workerRunning) {
    AppendLog(L"Installer is already running.");
    return;
  }

  const auto unpackFolder = SelectedUnpackFolder();
  const auto installFolder = std::filesystem::path(GetText(g_installEdit));
  if (unpackFolder.empty() || installFolder.empty()) {
    AppendLog(L"Select unpack drive and install folder before unpacking.");
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
    AppendLog(L"No archive file found beside the installer.");
    return;
  }

  g_workerRunning = true;
  SetControlsRunning(hwnd, true);
  g_closeAfterWorker = false;
  std::thread(RunUnpackWorker, hwnd, *archive, unpackFolder).detach();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_CREATE: {
      INITCOMMONCONTROLSEX controls{};
      controls.dwSize = sizeof(controls);
      controls.dwICC = ICC_PROGRESS_CLASS;
      InitCommonControlsEx(&controls);
      std::filesystem::create_directories(ExeFolder() / "logs");
      std::filesystem::create_directories(PackageFolder());
      std::filesystem::create_directories(ExeFolder() / "tools" / "7zip");
      ApplyWindowFrameTheme(hwnd);
      g_contentBrush = CreateSolidBrush(kContentColor);
      g_headerBrush = CreateSolidBrush(kHeaderColor);
      g_panelBrush = CreateSolidBrush(kPanelColor);
      g_footerBrush = CreateSolidBrush(kFooterColor);
      g_editBrush = CreateSolidBrush(kEditColor);
      g_stepFont = CreateUiFont(10, FW_SEMIBOLD);
      g_titleFont = CreateUiFont(22, FW_NORMAL, L"Georgia");
      g_bodyFont = CreateUiFont(10);
      g_labelFont = CreateUiFont(9, FW_SEMIBOLD);

      g_stepLabel = CreateLabel(hwnd, L"", 16, 18, 720, 24);
      g_welcomeTitle = CreateLabel(hwnd, L"Modlist Installer", 16, 92, 720, 38);
      g_welcomeBody = CreateLabel(hwnd, L"Welcome text will be added later.", 16, 146, 720, 90);
      g_downloadLabel = CreateLabel(hwnd, L"Package", 16, 112, 100, 20);
      g_unpackDriveLabel = CreateLabel(hwnd, L"Unpack drive", 16, 136, 100, 20);
      g_unpackTargetLabel = CreateLabel(hwnd, L"Target: choose a drive", 160, 136, 420, 20);
      g_installLabel = CreateLabel(hwnd, L"Install", 16, 178, 100, 20);
      g_downloadEdit = CreateEdit(hwnd, kDownloadEdit, 120, 22, 420, 25);
      g_unpackDriveCombo = CreateCombo(hwnd, kUnpackDriveCombo, 120, 132, 120, 180);
      g_installEdit = CreateEdit(hwnd, kInstallEdit, 120, 57, 420, 25);
      CreateButton(hwnd, kDownloadBrowse, L"Browse", 550, 22, 88, 25);
      CreateButton(hwnd, kInstallBrowse, L"Browse", 550, 57, 88, 25);
      CreateButton(hwnd, kValidateButton, L"Validate", 120, 96, 120, 30);
      CreateButton(hwnd, kStartButton, L"Install", 252, 96, 120, 30);
      CreateButton(hwnd, kUnpackButton, L"Unpack", 384, 96, 120, 30);
      CreateButton(hwnd, kPauseButton, L"Pause", 516, 96, 120, 30);
      CreateButton(hwnd, kStopButton, L"Stop", 648, 96, 92, 30);
      g_previousButton = CreateButton(hwnd, kPreviousButton, L"Previous", 632, 450, 100, 30);
      g_nextButton = CreateButton(hwnd, kNextButton, L"Next", 744, 450, 100, 30);
      g_progress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE,
                                   16, 142, 622, 20, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(kProgress)), g_instance, nullptr);
      g_statusLabel = CreateWindowExW(0, L"STATIC", L"Idle | Ready for local validation",
                                      WS_CHILD | WS_VISIBLE,
                                      16, 170, 622, 22, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStatusLabel)), g_instance, nullptr);
      g_logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                  16, 177, 622, 258, hwnd,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(kLogEdit)), g_instance, nullptr);
      SetControlFont(g_stepLabel, g_stepFont);
      SetControlFont(g_welcomeTitle, g_titleFont);
      SetControlFont(g_welcomeBody, g_bodyFont);
      SetControlFont(g_downloadLabel, g_labelFont);
      SetControlFont(g_unpackDriveLabel, g_labelFont);
      SetControlFont(g_unpackTargetLabel, g_bodyFont);
      SetControlFont(g_installLabel, g_labelFont);
      SetControlFont(g_downloadEdit, g_bodyFont);
      SetControlFont(g_unpackDriveCombo, g_bodyFont);
      SetControlFont(g_installEdit, g_bodyFont);
      SetControlFont(g_statusLabel, g_bodyFont);
      SetControlFont(g_logEdit, g_bodyFont);
      SetControlFont(GetDlgItem(hwnd, kDownloadBrowse), g_bodyFont);
      SetControlFont(GetDlgItem(hwnd, kInstallBrowse), g_bodyFont);
      SetControlFont(GetDlgItem(hwnd, kValidateButton), g_bodyFont);
      SetControlFont(GetDlgItem(hwnd, kStartButton), g_bodyFont);
      SetControlFont(GetDlgItem(hwnd, kUnpackButton), g_bodyFont);
      SetControlFont(GetDlgItem(hwnd, kPauseButton), g_bodyFont);
      SetControlFont(GetDlgItem(hwnd, kStopButton), g_bodyFont);
      SetControlFont(g_previousButton, g_bodyFont);
      SetControlFont(g_nextButton, g_bodyFont);
      SendMessageW(g_progress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
      SendMessageW(g_progress, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(RGB(159, 196, 216)));
      SendMessageW(g_progress, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(kContentColor));
      SetText(g_downloadEdit, PackageFolder().wstring());
      PopulateDriveCombo();
      SetText(g_installEdit, L"");
      AppendLog(L"App log: " + PathToDisplay(AppLogPath()));
      AppendLog(L"Place exactly one .torrent file in: " + PathToDisplay(PackageFolder()));
      AppendLog(L"Place all archive parts beside this exe: " + PathToDisplay(ArchiveFolder()));
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
      AppendLog(L"Choose an unpack drive; the unpack folder will be created as <drive>:\\Sky.");
      Layout(hwnd);
      ShowWizardPage(hwnd, WizardPage::Welcome);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(hwnd, &paint);
      PaintInstallerChrome(hwnd, dc);
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      HWND control = reinterpret_cast<HWND>(lParam);
      SetBkMode(dc, TRANSPARENT);
      if (control == g_logEdit) {
        SetBkMode(dc, OPAQUE);
        SetTextColor(dc, kMutedTextColor);
        SetBkColor(dc, kEditColor);
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
      SetTextColor(dc, kPrimaryTextColor);
      SetBkColor(dc, kEditColor);
      return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_CTLCOLORLISTBOX: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      SetTextColor(dc, kPrimaryTextColor);
      SetBkColor(dc, kEditColor);
      return reinterpret_cast<LRESULT>(g_editBrush);
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
      if (item != nullptr && item->CtlType == ODT_BUTTON) {
        DrawNsisButton(*item);
        return TRUE;
      }
      return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    case WM_SIZE:
      Layout(hwnd);
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      if (id == kInstallBrowse) {
        if (auto path = PickFolder(hwnd)) {
          SetText(g_installEdit, path->wstring());
        }
      } else if (id == kUnpackDriveCombo && HIWORD(wParam) == CBN_SELCHANGE) {
        UpdateUnpackTargetLabel();
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
    case kValidationFailedMessage:
      MessageBoxW(hwnd, L"Rehash torrent", L"Validation failed", MB_OK | MB_ICONERROR);
      return 0;
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
        AppendLog(L"Waiting for validation to stop before closing.");
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
  windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_MODLIST_INSTALLER));
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
