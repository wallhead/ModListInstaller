#include "tracker/TrackerProvider.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace modlist {

namespace {

std::string Trim(std::string value) {
  auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) { return !isSpace(static_cast<unsigned char>(c)); }).base(), value.end());
  return value;
}

bool StartsWith(const std::string& value, const char* prefix) {
  const std::string text(prefix);
  return value.size() >= text.size() && std::equal(text.begin(), text.end(), value.begin());
}

}  // namespace

TrackerProvider::TrackerProvider(const ITextFetcher& fetcher) : fetcher_(fetcher) {}

Result<std::vector<std::string>> TrackerProvider::LoadTrackers(const std::string& url) const {
  auto response = fetcher_.FetchText(url);
  if (!response.ok()) {
    return Result<std::vector<std::string>>::Error(response.error());
  }
  return Result<std::vector<std::string>>::Ok(ParseTrackers(response.value()));
}

std::vector<std::string> TrackerProvider::ParseTrackers(const std::string& text) {
  std::vector<std::string> trackers;
  std::set<std::string> seen;
  std::istringstream input(text);
  std::string line;
  while (std::getline(input, line)) {
    line = Trim(line);
    if (line.empty() || !IsValidTrackerUrl(line) || seen.count(line) > 0) {
      continue;
    }
    seen.insert(line);
    trackers.push_back(line);
  }
  return trackers;
}

bool TrackerProvider::IsValidTrackerUrl(const std::string& url) {
  return StartsWith(url, "udp://") || StartsWith(url, "http://") ||
         StartsWith(url, "https://") || StartsWith(url, "wss://");
}

}  // namespace modlist
