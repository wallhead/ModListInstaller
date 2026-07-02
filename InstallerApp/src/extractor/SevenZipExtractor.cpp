#include "extractor/SevenZipExtractor.h"

#include "paths/PathValidator.h"
#include "resource.h"

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
#include <vector>

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

std::filesystem::path LocalToolCacheFolder(const std::filesystem::path& appRoot) {
  return appRoot / "data" / "tools" / "7zip";
}

Result<std::filesystem::path> ExtractEmbeddedSevenZip(const std::filesystem::path& appRoot) {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_7ZIP_EXE), MAKEINTRESOURCEW(10));
  if (resource == nullptr) {
    return Result<std::filesystem::path>::Error("Embedded 7-Zip resource was not found.");
  }

  HGLOBAL loaded = LoadResource(module, resource);
  const DWORD size = SizeofResource(module, resource);
  const void* data = LockResource(loaded);
  if (loaded == nullptr || data == nullptr || size == 0) {
    return Result<std::filesystem::path>::Error("Embedded 7-Zip resource could not be loaded.");
  }

  auto outputFolder = LocalToolCacheFolder(appRoot);
  std::error_code ec;
  std::filesystem::create_directories(outputFolder, ec);
  if (ec) {
    return Result<std::filesystem::path>::Error("Unable to create 7-Zip cache folder: " + ec.message());
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
      return Result<std::filesystem::path>::Error("Unable to write embedded 7-Zip executable.");
    }
    output.write(static_cast<const char*>(data), size);
    if (!output) {
      return Result<std::filesystem::path>::Error("Unable to finish writing embedded 7-Zip executable.");
    }
  }

  return Result<std::filesystem::path>::Ok(outputPath);
}

std::filesystem::path MakeOutputCapturePath(const std::filesystem::path& sevenZipExe) {
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  (void)sevenZipExe;
  auto logFolder = ModuleFolder() / "data" / "logs";
  std::error_code ec;
  std::filesystem::create_directories(logFolder, ec);
  if (ec) {
    logFolder = std::filesystem::temp_directory_path();
  }
  return logFolder / ("7z-extract-" + std::to_string(stamp) + ".log");
}

void AppendBoundedTail(std::string& tail, const char* data, size_t size, size_t maxSize) {
  if (size >= maxSize) {
    tail.assign(data + size - maxSize, maxSize);
    return;
  }
  tail.append(data, size);
  if (tail.size() > maxSize) {
    tail.erase(0, tail.size() - maxSize);
  }
}

std::string BytesToDisplay(DWORDLONG bytes) {
  std::ostringstream out;
  constexpr DWORDLONG kGiB = 1024ull * 1024ull * 1024ull;
  constexpr DWORDLONG kMiB = 1024ull * 1024ull;
  if (bytes >= kGiB) {
    out << (bytes / kGiB) << " GiB";
  } else {
    out << (bytes / kMiB) << " MiB";
  }
  return out.str();
}

SIZE_T SevenZipMemoryLimitBytes() {
  constexpr DWORDLONG kGiB = 1024ull * 1024ull * 1024ull;
  constexpr DWORDLONG kMinLimit = 1ull * kGiB;
  constexpr DWORDLONG kPreferredFloor = 4ull * kGiB;
  constexpr DWORDLONG kMaxLimit = 16ull * kGiB;

  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (!GlobalMemoryStatusEx(&memory) || memory.ullTotalPhys == 0) {
    return static_cast<SIZE_T>(kPreferredFloor);
  }

  const DWORDLONG total = memory.ullTotalPhys;
  DWORDLONG limit = (total >= 8ull * kGiB) ? (total * 3 / 4) : (total / 2);
  if (total >= 8ull * kGiB && limit < kPreferredFloor) {
    limit = kPreferredFloor;
  }
  if (limit < kMinLimit) {
    limit = kMinLimit;
  }
  if (limit > kMaxLimit) {
    limit = kMaxLimit;
  }
  if (limit >= total) {
    limit = total > kGiB ? total - kGiB : total / 2;
  }
  return static_cast<SIZE_T>(limit);
}

HANDLE CreateMemoryLimitedJob(SIZE_T memoryLimitBytes, std::ofstream& log) {
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    if (log) {
      log << "Warning: unable to create 7-Zip memory limit job: " << GetLastError() << "\n";
    }
    return nullptr;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  limits.ProcessMemoryLimit = memoryLimitBytes;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
    if (log) {
      log << "Warning: unable to apply 7-Zip memory limit: " << GetLastError() << "\n";
    }
    CloseHandle(job);
    return nullptr;
  }
  return job;
}

std::string SevenZipExitMessage(int code) {
  constexpr int kStatusNoMemory = static_cast<int>(0xC0000017u);
  constexpr int kStatusCommitmentLimit = static_cast<int>(0xC000012Du);
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
      return "7-Zip ran out of memory or hit the installer memory limit (exit code 8).";
    case 255:
      return "7-Zip was stopped by the user (exit code 255).";
    case kStatusNoMemory:
    case kStatusCommitmentLimit:
      return "7-Zip was stopped because it exceeded available or allowed memory.";
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
                         const std::filesystem::path& outputPath,
                         std::string& outputTail,
                         const SevenZipExtractor::ProgressCallback& progressCallback) {
  constexpr size_t kOutputTailLimit = 64 * 1024;

  std::ofstream log(outputPath, std::ios::binary);
  const SIZE_T memoryLimitBytes = SevenZipMemoryLimitBytes();
  if (log) {
    log << "Command:\n" << command << "\n\n";
    log << "7-Zip process memory limit:\n" << BytesToDisplay(memoryLimitBytes)
        << " (" << memoryLimitBytes << " bytes)\n\n";
    log << "7-Zip output:\n";
  }

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
  HANDLE job = CreateMemoryLimitedJob(memoryLimitBytes, log);
  const BOOL created = CreateProcessA(nullptr,
                                      mutableCommand.data(),
                                      nullptr,
                                      nullptr,
                                      TRUE,
                                      CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                      nullptr,
                                      nullptr,
                                      &startup,
                                      &process);
  CloseHandle(writePipe);
  if (!created) {
    CloseHandle(readPipe);
    if (job != nullptr) {
      CloseHandle(job);
    }
    if (log) {
      log << "\n\nCreateProcess failed: " << GetLastError() << "\n";
    }
    return static_cast<int>(GetLastError());
  }
  if (job != nullptr && !AssignProcessToJobObject(job, process.hProcess) && log) {
    log << "Warning: unable to assign 7-Zip to memory limit job: " << GetLastError() << "\n";
  }
  if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
    const DWORD resumeError = GetLastError();
    if (log) {
      log << "Unable to resume 7-Zip process: " << resumeError << "\n";
    }
    TerminateProcess(process.hProcess, resumeError);
  }

  std::string parseTail;
  char buffer[4096];
  DWORD bytesRead = 0;
  int lastPercent = -1;
  while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
    if (log) {
      log.write(buffer, bytesRead);
    }
    AppendBoundedTail(outputTail, buffer, bytesRead, kOutputTailLimit);
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
  if (job != nullptr) {
    CloseHandle(job);
  }
  CloseHandle(readPipe);
  if (log) {
    log << "\n\nExit code:\n" << exitCode << "\n";
  }
  return static_cast<int>(exitCode);
}

}  // namespace

Result<std::filesystem::path> SevenZipExtractor::LocateExecutable(const std::filesystem::path& appRoot) const {
  auto embedded = ExtractEmbeddedSevenZip(appRoot);
  if (embedded.ok()) {
    return embedded;
  }

  const std::array<std::filesystem::path, 12> candidates = {
      appRoot / "data" / "tools" / "7zip" / "7z.exe",
      appRoot / "data" / "tools" / "7zip" / "7za.exe",
      appRoot / "data" / "tools" / "7zip" / "7zz.exe",
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
  return Result<std::filesystem::path>::Error("7-Zip executable not found. Embedded copy could not be extracted: " + embedded.error());
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
  std::string outputTail;
  result.exitCode = RunProcessAndCapture(result.command, outputPath, outputTail, progressCallback);
  result.output = "Full 7-Zip output is streamed to:\n" + outputPath.string() + "\n\nLast captured output:\n" + outputTail;
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
