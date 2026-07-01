#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace modlist {

enum class LogLevel {
  Info,
  Warning,
  Error,
};

class Logger {
public:
  explicit Logger(std::filesystem::path logPath);

  void Info(const std::string& message);
  void Warning(const std::string& message);
  void Error(const std::string& message);
  const std::filesystem::path& path() const { return logPath_; }

private:
  void Write(LogLevel level, const std::string& message);

  std::filesystem::path logPath_;
  std::ofstream stream_;
  std::mutex mutex_;
};

}  // namespace modlist
