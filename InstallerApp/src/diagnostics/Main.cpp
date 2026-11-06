#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>

#include "Diagnostics.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
using modlist::diagnostics::Config;
using modlist::diagnostics::Outcome;
using modlist::diagnostics::Progress;

enum class LaunchMode { Gui, Run, Invalid };
struct Arguments {
  LaunchMode mode{LaunchMode::Invalid};
  Config config;
};

bool ParsePositiveNumber(std::wstring_view text, uint32_t& value) {
  if (text.empty()) return false;
  uint32_t number = 0;
  for (const wchar_t c : text) {
    if (c < L'0' || c > L'9') return false;
    const auto digit = static_cast<uint32_t>(c - L'0');
    if (number > (std::numeric_limits<uint32_t>::max() - digit) / 10) return false;
    number = number * 10 + digit;
  }
  value = number;
  return number != 0;
}

Arguments ParseArguments(int argc, wchar_t** argv) {
  Arguments result;
  if (argc == 1) {
    result.mode = LaunchMode::Gui;
    return result;
  }
  enum : unsigned { Run = 1, Source = 2, Destination = 4, GiB = 8, Threads = 16,
                    SevenZip = 32, Reverse = 64, Smoke = 128 };
  unsigned seen = 0;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view flag(argv[i]);
    unsigned bit = 0;
    bool hasValue = false;
    if (flag == L"--run") bit = Run;
    else if (flag == L"--source") { bit = Source; hasValue = true; }
    else if (flag == L"--destination") { bit = Destination; hasValue = true; }
    else if (flag == L"--gib") { bit = GiB; hasValue = true; }
    else if (flag == L"--threads") { bit = Threads; hasValue = true; }
    else if (flag == L"--sevenzip-first") bit = SevenZip;
    else if (flag == L"--reverse") bit = Reverse;
    else if (flag == L"--smoke") bit = Smoke;
    else return result;
    if ((seen & bit) != 0) return result;
    seen |= bit;
    if (hasValue) {
      if (++i == argc) return result;
      const std::wstring_view value(argv[i]);
      if (value.empty() || value.starts_with(L"--")) return result;
      if (bit == Source) result.config.sourceParent = value;
      else if (bit == Destination) result.config.destinationParent = value;
      else if (bit == GiB) {
        if (value != L"10" && value != L"20" && value != L"30") return result;
        if (!ParsePositiveNumber(value, result.config.sizeGiB)) return result;
      } else if (!ParsePositiveNumber(value, result.config.threads)) return result;
    }
  }
  constexpr unsigned required = Run | Source | Destination | GiB | Threads;
  if ((seen & required) != required) return result;
  result.config.sevenZipFirst = (seen & SevenZip) != 0;
  result.config.reversePass = (seen & Reverse) != 0;
  result.config.smoke = (seen & Smoke) != 0;
  result.mode = LaunchMode::Run;
  return result;
}

int ExitCode(const Outcome& outcome) {
  return outcome.cancelled ? 2 : outcome.ok ? 0 : 1;
}

std::atomic_bool cliCancel{false};
BOOL WINAPI ConsoleControl(DWORD event) {
  if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT &&
      event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT) return FALSE;
  cliCancel.store(true, std::memory_order_relaxed);
  return TRUE;
}

int RunCli(const Config& config) {
  cliCancel.store(false, std::memory_order_relaxed);
  const bool handlerInstalled = SetConsoleCtrlHandler(ConsoleControl, TRUE) != FALSE;
  int code = 1;
  try {
    code = ExitCode(modlist::diagnostics::RunBenchmark(config, cliCancel));
  } catch (...) {
    code = cliCancel.load(std::memory_order_relaxed) ? 2 : 1;
  }
  if (handlerInstalled) SetConsoleCtrlHandler(ConsoleControl, FALSE);
  return code;
}

std::wstring Wide(std::string_view text) {
  if (text.empty()) return {};
  const int length = static_cast<int>(std::min<size_t>(text.size(), INT_MAX));
  const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
  if (count == 0) return L"[Unreadable UTF-8 message]";
  std::wstring result(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), length, result.data(), count);
  return result;
}

std::filesystem::path ExecutableFolder() {
  std::wstring buffer(512, L'\0');
  for (;;) {
    const DWORD count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (count == 0) throw std::runtime_error("Cannot locate diagnostics executable.");
    if (count < buffer.size()) {
      buffer.resize(count);
      return std::filesystem::path(buffer).parent_path();
    }
    if (buffer.size() >= 32768) throw std::runtime_error("Executable path is too long.");
    buffer.resize(std::min<size_t>(buffer.size() * 2, 32768));
  }
}

void StartupError(const std::string& error) noexcept {
  try {
    std::ofstream log(ExecutableFolder() / "diagnostics-startup.log", std::ios::app | std::ios::binary);
    log << error << "\r\n";
  } catch (...) {}
}

std::wstring Duration(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
  const auto value = static_cast<uint64_t>(std::min(seconds, 315360000.0));
  std::wostringstream text;
  text << std::setfill(L'0') << std::setw(2) << value / 3600 << L':'
       << std::setw(2) << value / 60 % 60 << L':' << std::setw(2) << value % 60;
  return text.str();
}

constexpr UINT ProgressMessageId = WM_APP + 1;
constexpr UINT DoneMessageId = WM_APP + 2;
constexpr UINT_PTR CompletionTimer = 1;
constexpr wchar_t WindowClass[] = L"ModlistPortableDiagnostics";

struct ProgressChannel {
  explicit ProgressChannel(HWND target) : window(target) {}
  const HWND window;
  std::atomic_uint pending{0};
};

struct ProgressMessage {
  ProgressMessage(const Progress& value, const std::shared_ptr<ProgressChannel>& owner)
      : progress(value), channel(owner) {}
  ProgressMessage(const ProgressMessage&) = delete;
  ProgressMessage& operator=(const ProgressMessage&) = delete;
  const Progress progress;
  const std::shared_ptr<ProgressChannel> channel;
  ~ProgressMessage() { channel->pending.fetch_sub(1, std::memory_order_relaxed); }
};

void PostProgress(const std::shared_ptr<ProgressChannel>& channel, const Progress& progress) noexcept {
  // Bound queued snapshots when producers outpace the UI; never block a backend callback.
  if (channel->pending.fetch_add(1, std::memory_order_relaxed) >= 128) {
    channel->pending.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
  std::unique_ptr<ProgressMessage> message;
  try {
    message = std::make_unique<ProgressMessage>(progress, channel);
  } catch (...) {
    channel->pending.fetch_sub(1, std::memory_order_relaxed);
    return;
  }
  if (PostMessageW(channel->window, ProgressMessageId, 0, reinterpret_cast<LPARAM>(message.get())))
    message.release();
}

enum Control : size_t {
  SourceLabel, SourceDisk, DestinationLabel, DestinationDisk,
  SizeLabel, SizeCombo, ThreadsLabel, ThreadsCombo, OrderLabel, OrderCombo, ReverseCheck,
  StageText, MetricsText, ProgressBar, LogEdit, StartButton, StopButton, LogsButton, ControlCount
};
constexpr int ControlBase = 100;

class Application {
 public:
  HWND window{};
  UINT dpi{96};

  ~Application() {
    cancel_.store(true, std::memory_order_relaxed);
    if (worker_.joinable()) worker_.join();
    DiscardProgress();
    if (font_) DeleteObject(font_);
  }

  int Result() const { return result_; }

  void RequestCancel() {
    if (!running_) return;
    cancel_.store(true, std::memory_order_relaxed);
    EnableWindow(controls_[StopButton], FALSE);
    SetWindowTextW(controls_[StageText], closing_ ? L"Stopping before closing..." : L"Stopping...");
  }

  static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
      app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      app->window = window;
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
      return app->HandleMessage(message, wparam, lparam);
    } catch (const std::exception& error) {
      StartupError(error.what());
      if (message == WM_CREATE) return -1;
      app->RequestCancel();
      SetWindowTextW(app->controls_[StageText], L"Frontend error. Benchmark cancellation requested.");
      return 0;
    } catch (...) {
      StartupError("Unexpected frontend exception");
      return message == WM_CREATE ? -1 : 0;
    }
  }

 private:
  std::array<HWND, ControlCount> controls_{};
  struct Disk { std::filesystem::path root; uint64_t freeBytes; };
  std::vector<Disk> disks_;
  HFONT font_{};
  std::thread worker_;
  std::atomic_bool cancel_{false};
  std::atomic_bool done_{false};
  std::shared_ptr<ProgressChannel> channel_;
  Outcome outcome_;
  Progress latest_;
  bool running_{false};
  bool closing_{false};
  ULONGLONG started_{};
  int result_{0};
  std::string lastStage_;
  std::string lastMessage_;

  int Px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }

  void Add(Control id, const wchar_t* type, const wchar_t* text, DWORD style, DWORD extra = 0) {
    controls_[id] = CreateWindowExW(extra, type, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 0, 0, window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(ControlBase + id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (!controls_[id]) throw std::runtime_error("Cannot create diagnostics control " +
        std::to_string(id) + " (Windows error " + std::to_string(GetLastError()) + ")");
  }

  void CreateControls() {
    Add(SourceLabel, L"STATIC", L"Source disk", 0);
    Add(SourceDisk, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    Add(DestinationLabel, L"STATIC", L"Destination disk", 0);
    Add(DestinationDisk, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    Add(SizeLabel, L"STATIC", L"Size", 0);
    Add(SizeCombo, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    Add(ThreadsLabel, L"STATIC", L"Threads", 0);
    Add(ThreadsCombo, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    Add(OrderLabel, L"STATIC", L"Order", 0);
    Add(OrderCombo, L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL);
    Add(ReverseCheck, L"BUTTON", L"Reverse-order repeat", WS_TABSTOP | BS_AUTOCHECKBOX);
    Add(StageText, L"STATIC", L"Ready", SS_ENDELLIPSIS);
    Add(MetricsText, L"STATIC", L"", 0);
    Add(ProgressBar, PROGRESS_CLASSW, L"", 0);
    Add(LogEdit, L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
        ES_AUTOHSCROLL | WS_VSCROLL | WS_HSCROLL, WS_EX_CLIENTEDGE);
    Add(StartButton, L"BUTTON", L"Start", WS_TABSTOP | BS_DEFPUSHBUTTON);
    Add(StopButton, L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON);
    Add(LogsButton, L"BUTTON", L"Open Logs", WS_TABSTOP | BS_PUSHBUTTON);
    for (const auto* text : {L"10 GiB", L"20 GiB", L"30 GiB"})
      SendMessageW(controls_[SizeCombo], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    for (const auto* text : {L"1", L"2", L"4", L"8", L"16", L"32"})
      SendMessageW(controls_[ThreadsCombo], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    for (const auto* text : {L"Pipeline first", L"7-Zip first"})
      SendMessageW(controls_[OrderCombo], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text));
    SendMessageW(controls_[SizeCombo], CB_SETCURSEL, 0, 0);
    SendMessageW(controls_[ThreadsCombo], CB_SETCURSEL, 2, 0);
    SendMessageW(controls_[OrderCombo], CB_SETCURSEL, 0, 0);
    SendMessageW(controls_[LogEdit], EM_SETLIMITTEXT, 1024 * 1024, 0);
    SendMessageW(controls_[ProgressBar], PBM_SETRANGE32, 0, 100);
    RefreshDisks();
    UpdateFont();
    EnableControls();
    Layout();
  }

  void UpdateFont() {
    HFONT replacement = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!replacement) return;
    for (const HWND control : controls_)
      if (control) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
    if (font_) DeleteObject(font_);
    font_ = replacement;
  }

  void Layout() {
    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int margin = Px(16);
    const auto move = [&](Control id, int x, int y, int w, int h) {
      if (controls_[id]) MoveWindow(controls_[id], x, y, std::max(w, 0), std::max(h, 0), TRUE);
    };
    move(SourceLabel, margin, Px(16), width - 2 * margin, Px(20));
    move(SourceDisk, margin, Px(40), width - 2 * margin, Px(180));
    move(DestinationLabel, margin, Px(80), width - 2 * margin, Px(20));
    move(DestinationDisk, margin, Px(104), width - 2 * margin, Px(180));
    move(SizeLabel, margin, Px(148), Px(104), Px(20));
    move(SizeCombo, margin, Px(170), Px(104), Px(180));
    move(ThreadsLabel, Px(136), Px(148), Px(96), Px(20));
    move(ThreadsCombo, Px(136), Px(170), Px(96), Px(230));
    move(OrderLabel, Px(248), Px(148), width - margin - Px(248), Px(20));
    move(OrderCombo, Px(248), Px(170), width - margin - Px(248), Px(130));
    move(ReverseCheck, margin, Px(210), width - 2 * margin, Px(26));
    move(StageText, margin, Px(248), width - 2 * margin, Px(22));
    move(MetricsText, margin, Px(276), width - 2 * margin, Px(42));
    move(ProgressBar, margin, Px(326), width - 2 * margin, Px(18));
    move(LogEdit, margin, Px(356), width - 2 * margin, height - Px(356 + 58));
    move(StartButton, margin, height - Px(42), Px(88), Px(26));
    move(StopButton, Px(112), height - Px(42), Px(88), Px(26));
    move(LogsButton, width - margin - Px(112), height - Px(42), Px(112), Px(26));
  }

  void EnableControls() {
    for (const auto id : {SourceDisk, DestinationDisk,
                         SizeCombo, ThreadsCombo, OrderCombo, ReverseCheck, StartButton})
      EnableWindow(controls_[id], !running_);
    EnableWindow(controls_[StartButton], !running_ && disks_.size()>=2);
    EnableWindow(controls_[StopButton], running_ && !cancel_.load(std::memory_order_relaxed));
    EnableWindow(controls_[LogsButton], !running_ && !outcome_.logFolder.empty());
  }

  void AppendLog(const std::wstring& line) {
    const int length = GetWindowTextLengthW(controls_[LogEdit]);
    if (length > 256 * 1024) {
      SendMessageW(controls_[LogEdit], EM_SETSEL, 0, length - 192 * 1024);
      SendMessageW(controls_[LogEdit], EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }
    const auto text = line + L"\r\n";
    SendMessageW(controls_[LogEdit], EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SendMessageW(controls_[LogEdit], EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));
    SendMessageW(controls_[LogEdit], EM_SCROLLCARET, 0, 0);
  }

  std::filesystem::path SelectedDisk(Control control) const {
    const auto index=SendMessageW(controls_[control],CB_GETCURSEL,0,0);
    return index>=0 && static_cast<size_t>(index)<disks_.size()?disks_[static_cast<size_t>(index)].root:std::filesystem::path{};
  }

  void RefreshDisks() {
    if(running_)return;
    auto source=SelectedDisk(SourceDisk);
    const auto destination=SelectedDisk(DestinationDisk);
    if(source.empty())source=ExecutableFolder().root_path();
    disks_.clear();
    for(const auto id:{SourceDisk,DestinationDisk})SendMessageW(controls_[id],CB_RESETCONTENT,0,0);
    const DWORD mask=GetLogicalDrives();
    for(unsigned letter=0;letter<26;++letter) {
      if(!(mask&(1u<<letter)))continue;
      const std::wstring root=std::wstring(1,static_cast<wchar_t>(L'A'+letter))+L":\\";
      const auto type=GetDriveTypeW(root.c_str());
      if(type!=DRIVE_FIXED && type!=DRIVE_REMOVABLE)continue;
      ULARGE_INTEGER free{};
      if(!GetDiskFreeSpaceExW(root.c_str(),&free,nullptr,nullptr))continue;
      std::wostringstream label;label << root << L"    " << std::fixed << std::setprecision(1) << free.QuadPart/(1024.0*1024*1024) << L" GiB free";
      const auto text=label.str();
      for(const auto id:{SourceDisk,DestinationDisk})
        if(SendMessageW(controls_[id],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()))<0)
          throw std::runtime_error("Unable to populate disk selector");
      disks_.push_back({root,free.QuadPart});
    }
    const auto find=[&](const std::filesystem::path& root)->int {
      for(size_t i=0;i<disks_.size();++i)
        if(CompareStringOrdinal(root.c_str(),-1,disks_[i].root.c_str(),-1,TRUE)==CSTR_EQUAL)return static_cast<int>(i);
      return -1;
    };
    int sourceIndex=find(source),destinationIndex=find(destination);
    if(sourceIndex<0 && !disks_.empty())sourceIndex=0;
    if(destinationIndex<0) {
      for(size_t i=0;i<disks_.size();++i)if(static_cast<int>(i)!=sourceIndex &&
          (destinationIndex<0 || disks_[i].freeBytes>disks_[static_cast<size_t>(destinationIndex)].freeBytes))
        destinationIndex=static_cast<int>(i);
    }
    SendMessageW(controls_[SourceDisk],CB_SETCURSEL,static_cast<WPARAM>(sourceIndex),0);
    SendMessageW(controls_[DestinationDisk],CB_SETCURSEL,static_cast<WPARAM>(destinationIndex),0);
    if(disks_.size()<2)SetWindowTextW(controls_[StageText],L"Two available local disks are required.");
    EnableControls();
  }

  void Start() {
    if (running_) return;
    Config config;
    const auto source=SelectedDisk(SourceDisk),destination=SelectedDisk(DestinationDisk);
    if (source.empty() || destination.empty() || source==destination) {
      SetWindowTextW(controls_[StageText], L"Choose different source and destination disks.");
      SetFocus(controls_[DestinationDisk]);
      return;
    }
    config.sourceParent=modlist::diagnostics::TestParentForDrive(source);
    config.destinationParent=modlist::diagnostics::TestParentForDrive(destination);
    constexpr std::array<uint32_t, 3> sizes{10, 20, 30};
    constexpr std::array<uint32_t, 6> threads{1, 2, 4, 8, 16, 32};
    const auto sizeIndex = SendMessageW(controls_[SizeCombo], CB_GETCURSEL, 0, 0);
    const auto threadsIndex = SendMessageW(controls_[ThreadsCombo], CB_GETCURSEL, 0, 0);
    if (sizeIndex < 0 || sizeIndex >= static_cast<LRESULT>(sizes.size()) ||
        threadsIndex < 0 || threadsIndex >= static_cast<LRESULT>(threads.size())) return;
    config.sizeGiB = sizes[static_cast<size_t>(sizeIndex)];
    config.threads = threads[static_cast<size_t>(threadsIndex)];
    config.sevenZipFirst = SendMessageW(controls_[OrderCombo], CB_GETCURSEL, 0, 0) == 1;
    config.reversePass = SendMessageW(controls_[ReverseCheck], BM_GETCHECK, 0, 0) == BST_CHECKED;
    auto channel = std::make_shared<ProgressChannel>(window);
    if (!SetTimer(window, CompletionTimer, 200, nullptr)) {
      SetWindowTextW(controls_[StageText], L"Cannot create completion timer.");
      return;
    }
    channel_ = channel;
    outcome_ = {};
    latest_ = {};
    lastStage_.clear();
    lastMessage_.clear();
    cancel_.store(false, std::memory_order_relaxed);
    done_.store(false, std::memory_order_relaxed);
    started_ = GetTickCount64();
    running_ = true;
    EnableControls();
    SetFocus(controls_[StopButton]);
    SetWindowTextW(controls_[LogEdit], L"");
    SetWindowTextW(controls_[StageText], L"Starting...");
    SendMessageW(controls_[ProgressBar], PBM_SETPOS, 0, 0);
    UpdateMetrics();
    try {
      worker_ = std::thread([this, config, channel] {
        try {
          outcome_ = modlist::diagnostics::RunBenchmark(config, cancel_,
              [channel](const Progress& progress) { PostProgress(channel, progress); });
        } catch (const std::exception& error) {
          outcome_.error = error.what();
          outcome_.cancelled = cancel_.load(std::memory_order_relaxed);
        } catch (...) {
          outcome_.error = "Unexpected backend exception.";
          outcome_.cancelled = cancel_.load(std::memory_order_relaxed);
        }
        // Outcome is read only after this release/acquire pair and joining the worker.
        done_.store(true, std::memory_order_release);
        PostMessageW(channel->window, DoneMessageId, 0, 0);
      });
    } catch (const std::exception& error) {
      KillTimer(window, CompletionTimer);
      running_ = false;
      result_ = 1;
      EnableControls();
      SetWindowTextW(controls_[StageText], L"Cannot start benchmark worker.");
      AppendLog(Wide(error.what()));
      SetFocus(controls_[StartButton]);
    }
  }

  void UpdateMetrics() {
    const double elapsed = static_cast<double>(GetTickCount64() - started_) / 1000.0;
    const double speed = std::isfinite(latest_.currentMiBps) ? std::max(0.0, latest_.currentMiBps) : 0;
    const bool child=latest_.basis!=modlist::diagnostics::ProgressBasis::LogicalBytes;
    const wchar_t* basis=latest_.basis==modlist::diagnostics::ProgressBasis::ChildInput?L"Input ":
                         child?L"Output ":L"";
    std::wostringstream text;
    text << L"Run " << Duration(elapsed) << L"    Stage " << Duration(latest_.elapsedSeconds)
         << L"    " << basis << std::fixed << std::setprecision(1) << speed << L" MiB/s"
         << L"    CPU " << latest_.cpuPercent << L"%    RAM " << latest_.workingSetMiB << L" MiB\r\n"
         << static_cast<double>(latest_.bytes) / (1024.0 * 1024.0) << L" / "
         << static_cast<double>(latest_.totalBytes) / (1024.0 * 1024.0) << L" MiB    "
         << (child?L"Read ":L"Files ");
    if(child)text << latest_.processReadMiBps << L" MiB/s    Write " << latest_.processWriteMiBps << L" MiB/s";
    else text << latest_.files;
    SetWindowTextW(controls_[MetricsText], text.str().c_str());
  }

  void ReceiveProgress(std::unique_ptr<ProgressMessage> message) {
    if (!message || !running_ || message->channel != channel_) return;
    latest_ = message->progress;
    const auto stage = Wide(latest_.stage);
    const auto detail = Wide(latest_.message);
    if (!cancel_.load(std::memory_order_relaxed)) {
      const auto status = stage + (detail.empty() ? L"" : L": " + detail);
      SetWindowTextW(controls_[StageText], status.c_str());
    }
    SendMessageW(controls_[ProgressBar], PBM_SETPOS, std::clamp(latest_.percent, 0, 100), 0);
    UpdateMetrics();
    if (latest_.stage != lastStage_ || latest_.message != lastMessage_) {
      AppendLog(L"[" + Duration(static_cast<double>(GetTickCount64() - started_) / 1000.0) + L"] " +
                stage + (detail.empty() ? L"" : L": " + detail));
      lastStage_ = latest_.stage;
      lastMessage_ = latest_.message;
    }
  }

  void DiscardProgress() {
    if (!window) return;
    MSG message{};
    while (PeekMessageW(&message, window, ProgressMessageId, ProgressMessageId, PM_REMOVE)) {
      std::unique_ptr<ProgressMessage> owned(reinterpret_cast<ProgressMessage*>(message.lParam));
    }
  }

  void CheckCompletion() {
    if (!running_ || !done_.load(std::memory_order_acquire)) return;
    if (worker_.joinable()) worker_.join();
    MSG message{};
    while (PeekMessageW(&message, window, ProgressMessageId, ProgressMessageId, PM_REMOVE))
      ReceiveProgress(std::unique_ptr<ProgressMessage>(reinterpret_cast<ProgressMessage*>(message.lParam)));
    UpdateMetrics();
    KillTimer(window, CompletionTimer);
    running_ = false;
    result_ = ExitCode(outcome_);
    const wchar_t* status = outcome_.cancelled ? L"Cancelled" : outcome_.ok ? L"Completed" : L"Failed";
    SetWindowTextW(controls_[StageText], status);
    AppendLog(status);
    if (!outcome_.error.empty()) AppendLog(Wide(outcome_.error));
    if (!outcome_.logFolder.empty()) AppendLog(L"Logs: " + outcome_.logFolder.wstring());
    if (closing_) DestroyWindow(window);
    else { RefreshDisks(); SetFocus(controls_[StartButton]); }
  }

  void OpenLogs() {
    if (running_ || outcome_.logFolder.empty()) return;
    const auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open",
        outcome_.logFolder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32) AppendLog(L"Cannot open log folder (Shell error " + std::to_wstring(result) + L").");
  }

  LRESULT HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE: {
        const auto getDpi = reinterpret_cast<UINT(WINAPI*)(HWND)>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (getDpi) dpi = getDpi(window);
        CreateControls();
        return 0;
      }
      case WM_SIZE: Layout(); return 0;
      case WM_DEVICECHANGE:
        if(wparam==DBT_DEVICEARRIVAL || wparam==DBT_DEVICEREMOVECOMPLETE || wparam==DBT_DEVNODES_CHANGED)RefreshDisks();
        return DefWindowProcW(window,message,wparam,lparam);
      case WM_DPICHANGED: {
        dpi = HIWORD(wparam);
        UpdateFont();
        const auto* rect = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left,
                     rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
      }
      case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {Px(680), Px(580)};
        return 0;
      }
      case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == IDCANCEL) { RequestCancel(); return 0; }
        if (id == IDOK) { Start(); return 0; }
        if (HIWORD(wparam) != BN_CLICKED) return 0;
        if (id == ControlBase + StopButton) RequestCancel();
        else if (!running_) {
          if (id == ControlBase + StartButton) Start();
          else if (id == ControlBase + LogsButton) OpenLogs();
        }
        return 0;
      }
      case ProgressMessageId:
        ReceiveProgress(std::unique_ptr<ProgressMessage>(reinterpret_cast<ProgressMessage*>(lparam)));
        return 0;
      case DoneMessageId: CheckCompletion(); return 0;
      case WM_TIMER:
        if (wparam == CompletionTimer) {
          CheckCompletion();
          if (running_) UpdateMetrics();
        }
        return 0;
      case WM_CLOSE:
        if (running_) { closing_ = true; RequestCancel(); }
        else DestroyWindow(window);
        return 0;
      case WM_DESTROY:
        DiscardProgress();
        PostQuitMessage(result_);
        return 0;
      case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
      default: return DefWindowProcW(window, message, wparam, lparam);
    }
  }
};

void EnableDpiAwareness() {
  const auto setContext = reinterpret_cast<BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT)>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
  if (!setContext || !setContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
}

int RunGui(HINSTANCE instance, int show) {
  EnableDpiAwareness();
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  if (FAILED(com)) { StartupError("COM initialization: " + std::to_string(com)); return 1; }
  struct ComScope { ~ComScope() { CoUninitialize(); } } comScope;
  INITCOMMONCONTROLSEX common{sizeof(common), ICC_PROGRESS_CLASS};
  if (!InitCommonControlsEx(&common)) { StartupError("Common controls: " + std::to_string(GetLastError())); return 1; }
  WNDCLASSEXW type{sizeof(type)};
  type.lpfnWndProc = Application::WindowProcedure;
  type.hInstance = instance;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  type.hIconSm = type.hIcon;
  type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
  type.lpszClassName = WindowClass;
  if (!RegisterClassExW(&type)) { StartupError("Register window: " + std::to_string(GetLastError())); return 1; }
  Application app;
  HDC screen = GetDC(nullptr);
  if (screen) {
    app.dpi = static_cast<UINT>(GetDeviceCaps(screen, LOGPIXELSX));
    ReleaseDC(nullptr, screen);
  }
  const HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, WindowClass, L"Installer Diagnostics",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      MulDiv(820, static_cast<int>(app.dpi), 96), MulDiv(680, static_cast<int>(app.dpi), 96),
      nullptr, nullptr, instance, &app);
  if (!window) { StartupError("Create window: " + std::to_string(GetLastError())); return 1; }
  ShowWindow(window, show);
  UpdateWindow(window);
  MSG message{};
  int status = 0;
  while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0) {
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
      app.RequestCancel();
      continue;
    }
    if (!IsDialogMessageW(window, &message)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }
  return status == -1 ? 1 : app.Result();
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  try {
    int argc = 0;
    auto* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    struct FreeArguments {
      wchar_t** value;
      ~FreeArguments() { LocalFree(value); }
    } arguments{argv};
    const auto parsed = ParseArguments(argc, argv);
    if (parsed.mode == LaunchMode::Invalid) return 1;
    if (parsed.mode == LaunchMode::Run) return RunCli(parsed.config);
    return RunGui(instance, show);
  } catch (...) {
    return 1;
  }
}
