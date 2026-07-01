#include "extractor/SevenZipExtractor.h"

#include "paths/PathValidator.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>

namespace modlist {

namespace {

std::string Quote(const std::filesystem::path& path) {
  std::string text = path.string();
  std::string escaped;
  escaped.reserve(text.size());
  for (char c : text) {
    if (c == '"') {
      escaped += "\\\"";
    } else {
      escaped += c;
    }
  }
  return "\"" + escaped + "\"";
}

std::filesystem::path MakeOutputCapturePath(const std::filesystem::path& sevenZipExe) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto logFolder = sevenZipExe.parent_path() / "logs";
  if (sevenZipExe.parent_path().filename() == "7zip" &&
      sevenZipExe.parent_path().parent_path().filename() == "tools") {
    logFolder = sevenZipExe.parent_path().parent_path().parent_path() / "logs";
  }
  std::error_code ec;
  std::filesystem::create_directories(logFolder, ec);
  if (ec) {
    logFolder = std::filesystem::temp_directory_path();
  }
  return logFolder / ("7z-extract-" + std::to_string(stamp) + ".log");
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

void WriteTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream output(path, std::ios::binary);
  if (output) {
    output << text;
  }
}

std::string SevenZipExitMessage(int code) {
  switch (code) {
    case 0:
      return "Extraction completed.";
    case 1:
      return "7-Zip completed with warnings (exit code 1). Check the details below.";
    case 2:
      return "7-Zip fatal error (exit code 2).";
    case 7:
      return "7-Zip command line error (exit code 7).";
    case 8:
      return "7-Zip ran out of memory (exit code 8).";
    case 255:
      return "7-Zip was stopped by the user (exit code 255).";
    default:
      return "7-Zip extraction failed with exit code " + std::to_string(code) + ".";
  }
}

std::optional<int> LastPercentIn(const std::string& text) {
  std::optional<int> percent;
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%') {
      continue;
    }
    size_t begin = i;
    while (begin > 0 && std::isdigit(static_cast<unsigned char>(text[begin - 1]))) {
      --begin;
    }
    if (begin == i) {
      continue;
    }
    const int value = std::stoi(text.substr(begin, i - begin));
    if (value >= 0 && value <= 100) {
      percent = value;
    }
  }
  return percent;
}

int RunProcessAndCapture(const std::string& command,
                         std::string& output,
                         const SevenZipExtractor::ProgressCallback& progressCallback) {
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;

  HANDLE readPipe = nullptr;
  HANDLE writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &security, 0)) {
    return -1;
  }
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = writePipe;
  startup.hStdError = writePipe;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

  PROCESS_INFORMATION process{};
  std::string mutableCommand = command;
  const BOOL created = CreateProcessA(nullptr,
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

  std::string parseTail;
  char buffer[4096];
  DWORD bytesRead = 0;
  int lastPercent = -1;
  while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
    output.append(buffer, bytesRead);
    parseTail.append(buffer, bytesRead);
    if (parseTail.size() > 256) {
      parseTail.erase(0, parseTail.size() - 256);
    }
    if (progressCallback) {
      const auto percent = LastPercentIn(parseTail);
      if (percent.has_value() && *percent != lastPercent) {
        lastPercent = *percent;
        progressCallback(*percent);
      }
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

}  // namespace

Result<std::filesystem::path> SevenZipExtractor::LocateExecutable(const std::filesystem::path& appRoot) const {
  const std::array<std::filesystem::path, 9> candidates = {
      appRoot / "tools" / "7zip" / "7z.exe",
      appRoot / "tools" / "7zip" / "7za.exe",
      appRoot / "tools" / "7zip" / "7zz.exe",
      appRoot / "tools" / "7z.exe",
      appRoot / "tools" / "7za.exe",
      appRoot / "tools" / "7zz.exe",
      appRoot / "third_party" / "7zip" / "7z.exe",
      appRoot / "third_party" / "7zip" / "7za.exe",
      appRoot / "third_party" / "7zip" / "7zz.exe",
  };

  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate, ec) && std::filesystem::is_regular_file(candidate, ec)) {
      return Result<std::filesystem::path>::Ok(candidate);
    }
  }
  return Result<std::filesystem::path>::Error("7-Zip executable not found. Bundle it under tools\\7zip.");
}

ExtractionResult SevenZipExtractor::Extract(const ExtractionConfig& config, ProgressCallback progressCallback) const {
  ExtractionResult result;
  result.tempFolder = SameDiskTempPath(config.installFolder);

  std::error_code ec;
  if (!std::filesystem::exists(config.sevenZipExe, ec)) {
    result.message = "7-Zip executable is missing.";
    return result;
  }
  if (!std::filesystem::exists(config.archiveFirstPart, ec)) {
    result.message = "Archive first part is missing: " + config.archiveFirstPart.string();
    return result;
  }
  std::filesystem::create_directories(config.installFolder, ec);
  if (ec) {
    result.message = "Unable to create install folder: " + ec.message();
    return result;
  }
  if (config.useSameDiskTemp) {
    std::filesystem::create_directories(result.tempFolder, ec);
    if (ec) {
      result.message = "Unable to create same-disk temp folder: " + ec.message();
      return result;
    }
  }

  const auto outputPath = MakeOutputCapturePath(config.sevenZipExe);
  result.outputLogPath = outputPath;
  result.command = BuildCommand(config);
  result.exitCode = RunProcessAndCapture(result.command, result.output, progressCallback);
  const std::string logText = "Command:\n" + result.command + "\n\nExit code:\n" +
                              std::to_string(result.exitCode) + "\n\n7-Zip output:\n" + result.output;
  WriteTextFile(outputPath, logText);
  result.output = logText;
  result.ok = result.exitCode == 0 || result.exitCode == 1;
  result.message = SevenZipExitMessage(result.exitCode);
  return result;
}

std::string SevenZipExtractor::BuildCommand(const ExtractionConfig& config) {
  std::string command = Quote(config.sevenZipExe) + " x " + Quote(config.archiveFirstPart) +
                        " -o" + Quote(config.installFolder) + " -y -bsp1";
  if (config.useSameDiskTemp) {
    command += " -w" + Quote(SameDiskTempPath(config.installFolder));
  }
  return command;
}

}  // namespace modlist
