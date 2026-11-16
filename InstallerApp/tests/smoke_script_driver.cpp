#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

std::wstring Environment(const wchar_t* name) {
  wchar_t value[32768]{};
  const DWORD length = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
  return length > 0 && length < std::size(value) ? std::wstring(value, length) : std::wstring{};
}

std::wstring Quote(const std::wstring& value) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (const auto ch : value) {
    if (ch == L'\\') { ++slashes; continue; }
    result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
    slashes = 0;
    if (ch == L'"') result += L'\\';
    result += ch;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}

void PublishMarker(const std::filesystem::path& marker) {
  auto pending = marker;
  pending += L".tmp";
  {
    std::ofstream file(pending);
    file << GetCurrentProcessId() << '\n';
    file.close();
    if (!file) throw std::runtime_error("Cannot write smoke-test marker");
  }
  std::filesystem::rename(pending, marker);
}

int wmain(int argc, wchar_t** argv) {
  const auto real = Environment(L"MODLIST_SCRIPT_TEST_RUNNER");
  if (real.empty() || argc < 2) return 9;
  if (argc >= 3 && std::filesystem::path(argv[2]).filename() == L"pipeline-corrupt") {
    const auto control = Environment(L"MODLIST_SCRIPT_TEST_CONTROL");
    if (!control.empty()) {
      std::ofstream manifest(std::filesystem::path(control) / "corrupt-manifest.txt", std::ios::binary);
      const auto utf8 = std::filesystem::path(argv[1]).u8string();
      manifest.write(reinterpret_cast<const char*>(utf8.data()), static_cast<std::streamsize>(utf8.size()));
      manifest.close();
      if (!manifest) return 9;
      PublishMarker(std::filesystem::path(control) / "corrupt-entered.txt");
    }
    const auto mode = Environment(L"MODLIST_SCRIPT_TEST_MODE");
    if (mode == L"hold") Sleep(INFINITE);
    if (mode == L"fail") {
      std::cerr << "Injected failure after corruption fixture preparation\n";
      return 1;
    }
  }
  std::wstring command = Quote(real);
  for (int index = 1; index < argc; ++index) {
    command += L" " + Quote(argv[index]);
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
  startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(real.c_str(), command.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 9;
  WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = 9;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exitCode == 0 && argc >= 3 &&
      std::filesystem::path(argv[2]).filename() == L"pipeline-after-cancel" &&
      Environment(L"MODLIST_SCRIPT_TEST_MODE") == L"block-copy") {
    PublishMarker(std::filesystem::path(Environment(L"MODLIST_SCRIPT_TEST_CONTROL")) /
                  "after-cancel-finished.txt");
    const auto gate = OpenEventW(SYNCHRONIZE, FALSE,
                                Environment(L"MODLIST_SCRIPT_TEST_GATE").c_str());
    if (!gate) return 9;
    const auto waited = WaitForSingleObject(gate, 60000);
    CloseHandle(gate);
    if (waited != WAIT_OBJECT_0) return 9;
  }
  return static_cast<int>(exitCode);
}
