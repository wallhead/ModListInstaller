#include "logging/Logger.h"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace modlist {

namespace {

const char* ToString(LogLevel level) {
  switch (level) {
    case LogLevel::Info:
      return "INFO";
    case LogLevel::Warning:
      return "WARN";
    case LogLevel::Error:
      return "ERROR";
  }
  return "INFO";
}

std::string Timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  localtime_s(&tm, &time);
#else
  localtime_r(&time, &tm);
#endif
  std::ostringstream out;
  out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return out.str();
}

}  // namespace

Logger::Logger(std::filesystem::path logPath) : logPath_(std::move(logPath)) {
  if (logPath_.has_parent_path()) {
    std::filesystem::create_directories(logPath_.parent_path());
  }
  stream_.open(logPath_, std::ios::app);
}

void Logger::Info(const std::string& message) {
  Write(LogLevel::Info, message);
}

void Logger::Warning(const std::string& message) {
  Write(LogLevel::Warning, message);
}

void Logger::Error(const std::string& message) {
  Write(LogLevel::Error, message);
}

void Logger::Write(LogLevel level, const std::string& message) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!stream_) {
    return;
  }
  stream_ << Timestamp() << " [" << ToString(level) << "] " << message << '\n';
  stream_.flush();
}

}  // namespace modlist
