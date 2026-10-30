#include "../src/ui/WinInstallerApp.cpp"

#include <iostream>
#include <stdexcept>

namespace {

HWND testOwner = nullptr;
struct CompletionObservation {
  bool seen = false;
  int progress = -1;
  std::wstring status;
  std::wstring button;
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
  PostMessageW(window, WM_COMMAND, IDOK, 0);
  return FALSE;
}

void CALLBACK CompletionTimer(HWND, UINT, UINT_PTR id, DWORD) {
  EnumThreadWindows(GetCurrentThreadId(), ObserveCompletionDialog, 0);
  if (completionObservation.seen) KillTimer(nullptr, id);
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
  renderer.PaintDialog(testOwner, dc, L"Completion title", L"Installation completed\nReady to close");
  const auto& theme = renderer.Theme();
  const auto color = [](uint32_t rgb) { return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255); };
  const auto scale = [=](int dip) { return MulDiv(dip, static_cast<int>(GetDpiForWindow(testOwner)), 96); };
  const int footerTop = client.bottom - scale(static_cast<int>(theme.footerHeight));
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
  const bool message = hasText(scale(67), footerTop - scale(14));
  SelectObject(dc, previous);
  DeleteObject(bitmap);
  DeleteDC(dc);
  ExpectUi(background && footer, "GPU-independent dialog painted a blank or unthemed body");
  ExpectUi(title && message, "GPU-independent dialog omitted its title or message");
}

}  // namespace

int wmain() {
  int failures = 0;
  try {
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
