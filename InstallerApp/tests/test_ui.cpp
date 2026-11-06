#include "../src/ui/WinInstallerApp.cpp"

#include <iostream>
#include <stdexcept>

namespace {

HWND testOwner = nullptr;
bool quitTimerFired = false;
struct CompletionObservation {
  bool seen = false;
  int progress = -1;
  std::wstring status;
  std::wstring button;
  RECT dialogRect{};
  bool visible = false;
  bool ownerMinimized = false;
  bool escape = false;
  int dismissalAttempts = 0;
  bool defaultButtonFocused = false;
} completionObservation;

void ExpectUi(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

BOOL CALLBACK ObserveCompletionDialog(HWND window, LPARAM) {
  wchar_t className[80]{};
  GetClassNameW(window, className, static_cast<int>(std::size(className)));
  if (GetWindow(window, GW_OWNER) != testOwner ||
      std::wstring_view(className) != L"ModlistInstallerThemedDialog") return TRUE;
  completionObservation.seen = true;
  completionObservation.progress = g_progressPercent;
  completionObservation.status = g_statusText;
  completionObservation.button = GetText(GetDlgItem(testOwner, kStartButton));
  GetWindowRect(window, &completionObservation.dialogRect);
  completionObservation.visible = IsWindowVisible(window) != FALSE;
  completionObservation.ownerMinimized = IsIconic(testOwner) != FALSE;
  const HWND defaultButton = GetDlgItem(window, IDNO) != nullptr
      ? GetDlgItem(window, IDNO) : GetDlgItem(window, IDOK);
  completionObservation.defaultButtonFocused = GetFocus() == defaultButton;
  if (completionObservation.escape && completionObservation.dismissalAttempts == 0) {
    PostMessageW(defaultButton, WM_KEYDOWN, VK_ESCAPE, 0);
  } else {
    PostMessageW(window, WM_COMMAND, IDOK, 0);
  }
  ++completionObservation.dismissalAttempts;
  return FALSE;
}

void CALLBACK CompletionTimer(HWND, UINT, UINT_PTR id, DWORD) {
  EnumThreadWindows(GetCurrentThreadId(), ObserveCompletionDialog, 0);
  if (completionObservation.seen) KillTimer(nullptr, id);
}

void CALLBACK DismissalTimer(HWND, UINT, UINT_PTR, DWORD) {
  EnumThreadWindows(GetCurrentThreadId(), ObserveCompletionDialog, 0);
}

void CALLBACK QuitModalTimer(HWND, UINT, UINT_PTR id, DWORD) {
  KillTimer(nullptr, id);
  quitTimerFired = true;
  PostQuitMessage(42);
}

void CALLBACK QuitWatchdogTimer(HWND, UINT, UINT_PTR id, DWORD) {
  KillTimer(nullptr, id);
  PostQuitMessage(43);
}

BOOL CALLBACK FindThemedDialog(HWND window, LPARAM result) {
  wchar_t className[80]{};
  GetClassNameW(window, className, static_cast<int>(std::size(className)));
  if (GetWindow(window, GW_OWNER) == testOwner &&
      std::wstring_view(className) == L"ModlistInstallerThemedDialog") {
    *reinterpret_cast<HWND*>(result) = window;
    return FALSE;
  }
  return TRUE;
}

void TestCompletionSetsFinalStateBeforeDialog() {
  g_installCompleted = false;
  g_workerRunning = true;
  SendUiProgress(67, L"Validation 71% | Extraction 79% | ETA stale");
  completionObservation = {};
  const auto timer = SetTimer(nullptr, 0, 10, CompletionTimer);
  ExpectUi(timer != 0, "Unable to schedule completion-dialog observer");
  SendMessageW(testOwner, kWorkerFinishedMessage, 1, 0);
  KillTimer(nullptr, timer);
  ExpectUi(completionObservation.seen, "Successful completion did not open its dialog");
  ExpectUi(completionObservation.progress == 100,
           "Completion popup opened while progress still displayed an earlier percentage");
  ExpectUi(completionObservation.status.find(L"100%") != std::wstring::npos &&
               completionObservation.status.find(L"ETA stale") == std::wstring::npos,
           "Completion popup opened with stale extraction/ETA text");
  ExpectUi(completionObservation.button == UiText("button_close", L"Close"),
           "Completion did not change the install button to Close");
}

void TestCompletedStateRejectsLateProgress() {
  g_installCompleted = true;
  SendUiProgress(100, L"Installed 100%");
  SendMessageW(testOwner, kProgressMessage, 71, 0);
  SendMessageW(testOwner, kStatusMessage, 0,
               reinterpret_cast<LPARAM>(new std::wstring(L"Extraction 79% | ETA stale")));
  ExpectUi(g_progressPercent == 100 && g_statusText == L"Installed 100%",
           "A delayed progress/status message overwrote successful completion");
}

void ExpectDialogInsideWorkArea() {
  MONITORINFO monitor{sizeof(monitor)};
  const RECT rect = completionObservation.dialogRect;
  ExpectUi(completionObservation.seen && completionObservation.visible,
           "Completion popup was not visible");
  ExpectUi(GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONULL), &monitor) != FALSE &&
               rect.left >= monitor.rcWork.left && rect.top >= monitor.rcWork.top &&
               rect.right <= monitor.rcWork.right && rect.bottom <= monitor.rcWork.bottom,
           "Completion popup opened outside the monitor work area, leaving its owner disabled");
  ExpectUi(IsWindowEnabled(testOwner) != FALSE,
           "Closing the completion popup left the installer disabled");
}

void TestCompletionFromMinimizedWindow() {
  g_installCompleted = false;
  g_workerRunning = true;
  ShowWindow(testOwner, SW_SHOWMINNOACTIVE);
  ExpectUi(IsIconic(testOwner) != FALSE, "Test owner was not minimized");
  completionObservation = {};
  const auto timer = SetTimer(nullptr, 0, 10, CompletionTimer);
  ExpectUi(timer != 0, "Unable to observe minimized completion");
  SendMessageW(testOwner, kWorkerFinishedMessage, 1, 0);
  KillTimer(nullptr, timer);
  ShowWindow(testOwner, SW_HIDE);
  ShowWindow(testOwner, SW_RESTORE);
  ShowWindow(testOwner, SW_HIDE);
  ExpectUi(!completionObservation.ownerMinimized,
           "Completion left its owner minimized, hiding the modal popup");
  ExpectDialogInsideWorkArea();
}

void TestCompletionFromOffscreenWindow() {
  SetWindowPos(testOwner, nullptr, -16000, -16000, 0, 0,
               SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
  TestCompletionSetsFinalStateBeforeDialog();
  ExpectDialogInsideWorkArea();
}

void TestPopupFocusAndEscape() {
  completionObservation = {};
  completionObservation.escape = true;
  const auto timer = SetTimer(nullptr, 0, 10, DismissalTimer);
  ExpectUi(timer != 0, "Unable to observe Escape dismissal");
  const int result = ShowThemedMessage(testOwner, L"Confirmation", L"Do not accept on Escape", true);
  KillTimer(nullptr, timer);
  ExpectUi(completionObservation.dismissalAttempts == 1 && result == IDNO,
           "Escape did not dismiss the modal confirmation safely");
  ExpectUi(completionObservation.defaultButtonFocused,
           "The modal popup did not focus its safe default button");
}

void TestPopupPreservesDisabledOwner() {
  EnableWindow(testOwner, FALSE);
  completionObservation = {};
  const auto timer = SetTimer(nullptr, 0, 10, CompletionTimer);
  ExpectUi(timer != 0, "Unable to observe disabled-owner popup");
  ShowThemedMessage(testOwner, L"Nested message", L"Owner was already disabled");
  KillTimer(nullptr, timer);
  const bool stillDisabled = IsWindowEnabled(testOwner) == FALSE;
  EnableWindow(testOwner, TRUE);
  ExpectUi(stillDisabled, "Closing a popup re-enabled an owner that was already disabled");
}

void TestQuitCleansModalWindow() {
  quitTimerFired = false;
  const auto timer = SetTimer(nullptr, 0, 10, QuitModalTimer);
  ExpectUi(timer != 0, "Unable to schedule modal quit");
  ShowThemedMessage(testOwner, L"Quit", L"No orphan popup after shutdown");
  KillTimer(nullptr, timer);
  MSG quit{};
  const auto watchdog = SetTimer(nullptr, 0, 1000, QuitWatchdogTimer);
  ExpectUi(watchdog != 0, "Unable to bound outer message-loop check");
  BOOL messageResult = 1;
  while ((messageResult = GetMessageW(&quit, nullptr, 0, 0)) > 0) {
    TranslateMessage(&quit);
    DispatchMessageW(&quit);
  }
  KillTimer(nullptr, watchdog);
  const bool preservedQuit = quitTimerFired && messageResult == 0 && quit.wParam == 42;
  HWND leftover = nullptr;
  EnumThreadWindows(GetCurrentThreadId(), FindThemedDialog, reinterpret_cast<LPARAM>(&leftover));
  if (leftover != nullptr) {
    SetWindowLongPtrW(leftover, GWLP_USERDATA, 0);
    DestroyWindow(leftover);
  }
  ExpectUi(leftover == nullptr, "Leaving the modal loop orphaned a window with expired dialog state");
  ExpectUi(preservedQuit, "Leaving the modal loop lost WM_QUIT");
  ExpectUi(IsWindowEnabled(testOwner) != FALSE,
           "Leaving the modal loop left the installer disabled");
}

struct MessageObservation {
  std::wstring expected;
  bool found = false;
  bool complete = false;
  bool linesFit = false;
  bool lastInitiallyVisible = false;
  bool lastReachable = false;
  bool buttonsClear = false;
  bool scrollable = false;
} messageObservation;

BOOL CALLBACK FindMessageControl(HWND child, LPARAM result) {
  wchar_t className[40]{};
  GetClassNameW(child, className, static_cast<int>(std::size(className)));
  if (_wcsicmp(className, L"Edit") != 0) return TRUE;
  *reinterpret_cast<HWND*>(result) = child;
  return FALSE;
}

BOOL CALLBACK ObserveDialogMessage(HWND window, LPARAM) {
  HWND dialog = nullptr;
  FindThemedDialog(window, reinterpret_cast<LPARAM>(&dialog));
  if (dialog == nullptr) return TRUE;
  HWND control = nullptr;
  EnumChildWindows(dialog, FindMessageControl, reinterpret_cast<LPARAM>(&control));
  if (control != nullptr) {
    messageObservation.found = true;
    auto text = GetText(control);
    text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
    messageObservation.complete = text == messageObservation.expected;
    RECT area{};
    SendMessageW(control, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&area));
    HDC dc = GetDC(control);
    const auto oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    messageObservation.linesFit = true;
    const auto lines = SendMessageW(control, EM_GETLINECOUNT, 0, 0);
    std::vector<wchar_t> line(32768);
    for (LRESULT i = 0; i < lines; ++i) {
      *reinterpret_cast<WORD*>(line.data()) = static_cast<WORD>(line.size());
      const auto length = SendMessageW(control, EM_GETLINE, i, reinterpret_cast<LPARAM>(line.data()));
      SIZE extent{};
      GetTextExtentPoint32W(dc, line.data(), static_cast<int>(length), &extent);
      if (extent.cx > area.right - area.left) messageObservation.linesFit = false;
    }
    SelectObject(dc, oldFont);
    ReleaseDC(control, dc);
    const auto last = static_cast<WPARAM>(GetWindowTextLengthW(control) - 1);
    const auto lastVisible = [&] {
      const LRESULT point = SendMessageW(control, EM_POSFROMCHAR, last, 0);
      const int x = static_cast<short>(LOWORD(point));
      const int y = static_cast<short>(HIWORD(point));
      return point != -1 && x >= area.left && x < area.right &&
             y >= area.top && y + metrics.tmHeight <= area.bottom;
    };
    messageObservation.lastInitiallyVisible = lastVisible();
    SendMessageW(control, EM_SETSEL, last, last);
    SendMessageW(control, EM_SCROLLCARET, 0, 0);
    messageObservation.lastReachable = lastVisible();
    SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE};
    messageObservation.scrollable = GetScrollInfo(control, SB_VERT, &scroll) &&
        scroll.nMax - scroll.nMin + 1 > static_cast<int>(scroll.nPage);
    RECT messageRect{}, buttonRect{};
    GetWindowRect(control, &messageRect);
    GetWindowRect(GetDlgItem(dialog, IDNO), &buttonRect);
    messageObservation.buttonsClear = messageRect.bottom < buttonRect.top;
  }
  PostMessageW(dialog, WM_COMMAND, IDNO, 0);
  return FALSE;
}

void CALLBACK MessageObservationTimer(HWND, UINT, UINT_PTR, DWORD) {
  EnumThreadWindows(GetCurrentThreadId(), ObserveDialogMessage, 0);
}

void TestCleanupLongPathIsNotClipped() {
  messageObservation = {};
  messageObservation.expected = L"Files remain from a previous installation.\n\nFolder:\nE:\\Test builds\\" +
      std::wstring(180, L'a') + L"\\SmokePack\n\nClean this folder and continue? END_OF_MESSAGE";
  const auto timer = SetTimer(nullptr, 0, 10, MessageObservationTimer);
  ExpectUi(timer != 0, "Unable to observe long-path cleanup message");
  const int result = ShowThemedMessage(testOwner, L"Cleanup", messageObservation.expected, true);
  KillTimer(nullptr, timer);
  ExpectUi(messageObservation.found && messageObservation.complete,
           "Long cleanup message has no complete readable text control");
  ExpectUi(messageObservation.linesFit && messageObservation.lastInitiallyVisible,
           "Cleanup path or final question is clipped instead of wrapping and fitting");
  ExpectUi(messageObservation.buttonsClear && result == IDNO && IsWindowEnabled(testOwner),
           "Cleanup text overlaps buttons or prevents safe cancellation");
}

void TestOversizedCleanupMessageCanScroll() {
  messageObservation = {};
  messageObservation.expected = L"Folder:\nE:\\" + std::wstring(20000, L'b') +
      L"\n\nKeep the full path and the final question reachable. END_OF_MESSAGE";
  const auto timer = SetTimer(nullptr, 0, 10, MessageObservationTimer);
  ExpectUi(timer != 0, "Unable to observe oversized cleanup message");
  ShowThemedMessage(testOwner, L"Cleanup", messageObservation.expected, true);
  KillTimer(nullptr, timer);
  ExpectUi(messageObservation.complete && messageObservation.linesFit && messageObservation.scrollable &&
               messageObservation.lastReachable && messageObservation.buttonsClear,
           "An oversized cleanup message cannot scroll to the complete final question");
}

void TestFailedWorkerDoesNotReportSuccess() {
  g_installCompleted = false;
  g_workerRunning = true;
  SendUiProgress(42, L"Stopped");
  SendMessageW(testOwner, kWorkerFinishedMessage, 0, 0);
  ExpectUi(!g_installCompleted && !g_workerRunning && g_progressPercent == 42,
           "A failed or cancelled worker was displayed as a successful installation");
}

void TestDialogPaintsWithoutGraphicsDevice() {
  RECT client{};
  GetClientRect(testOwner, &client);
  HDC dc = CreateCompatibleDC(nullptr);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = client.right;
  info.bmiHeader.biHeight = -client.bottom;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* pixels = nullptr;
  HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
  ExpectUi(dc != nullptr && bitmap != nullptr, "Unable to allocate dialog test surface");
  const auto previous = SelectObject(dc, bitmap);
  FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
  modlist::NativeInstallerView renderer;
  // No Direct2D/DirectWrite initialization: the modal body must not need a GPU target.
  renderer.PaintDialog(testOwner, dc, L"Completion title");
  const auto& theme = renderer.Theme();
  const auto color = [](uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); };
  const auto scale = [=](int dip) { return MulDiv(dip, static_cast<int>(GetDpiForWindow(testOwner)), 96); };
  const bool background = GetPixel(dc, 4, 4) == color(theme.panel);
  const bool footer = GetPixel(dc, 4, client.bottom - 4) == color(theme.panelAlt);
  auto hasText = [&](int top, int bottom) {
    int count = 0;
    for (int y = top; y < bottom && count < 30; ++y) {
      for (int x = scale(28); x < client.right - scale(28) && count < 30; ++x) {
        if (GetPixel(dc, x, y) != color(theme.panel)) ++count;
      }
    }
    return count >= 30;
  };
  const bool title = hasText(scale(20), scale(54));
  SelectObject(dc, previous);
  DeleteObject(bitmap);
  DeleteDC(dc);
  ExpectUi(background && footer, "GPU-independent dialog painted a blank or unthemed body");
  ExpectUi(title, "GPU-independent dialog omitted its title");
}

}  // namespace

int wmain() {
  int failures = 0;
  try {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = g_instance;
    windowClass.lpszClassName = L"ModlistInstallerUiRegressionOwner";
    ExpectUi(RegisterClassW(&windowClass) != 0, "Unable to register test owner");
    testOwner = CreateWindowExW(0, windowClass.lpszClassName, L"UI regression owner",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, -16000, -16000, 960, 760,
        nullptr, nullptr, g_instance, nullptr);
    ExpectUi(testOwner != nullptr, "Unable to create test owner");
    const std::pair<const char*, void (*)()> tests[] = {
        {"final state before modal popup", TestCompletionSetsFinalStateBeforeDialog},
        {"late progress cannot overwrite completion", TestCompletedStateRejectsLateProgress},
        {"failed worker is not success", TestFailedWorkerDoesNotReportSuccess},
        {"completion from a minimized window", TestCompletionFromMinimizedWindow},
        {"completion from an offscreen window", TestCompletionFromOffscreenWindow},
        {"popup focus and Escape dismissal", TestPopupFocusAndEscape},
        {"popup preserves a disabled owner", TestPopupPreservesDisabledOwner},
        {"quit cleans the modal window", TestQuitCleansModalWindow},
        {"cleanup long path is not clipped", TestCleanupLongPathIsNotClipped},
        {"oversized cleanup message can scroll", TestOversizedCleanupMessageCanScroll},
        {"dialog pixels without a graphics device", TestDialogPaintsWithoutGraphicsDevice}};
    for (const auto& [name, test] : tests) {
      try { test(); std::cout << "PASS: " << name << '\n'; }
      catch (const std::exception& error) { ++failures; std::cerr << "FAIL: " << error.what() << '\n'; }
    }
    DestroyWindow(testOwner);
    testOwner = nullptr;
  } catch (const std::exception& error) {
    ++failures;
    std::cerr << "FAIL: " << error.what() << '\n';
    if (testOwner != nullptr) DestroyWindow(testOwner);
  }
  return failures == 0 ? 0 : 1;
}
