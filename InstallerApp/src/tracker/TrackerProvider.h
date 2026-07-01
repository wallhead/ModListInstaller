#pragma once

#include "common/Result.h"

#include <string>
#include <vector>

namespace modlist {

class ITextFetcher {
public:
  virtual ~ITextFetcher() = default;
  virtual Result<std::string> FetchText(const std::string& url) const = 0;
};

class ITrackerProvider {
public:
  virtual ~ITrackerProvider() = default;
  virtual Result<std::vector<std::string>> LoadTrackers(const std::string& url) const = 0;
};

class TrackerProvider : public ITrackerProvider {
public:
  explicit TrackerProvider(const ITextFetcher& fetcher);

  Result<std::vector<std::string>> LoadTrackers(const std::string& url) const override;
  static std::vector<std::string> ParseTrackers(const std::string& text);
  static bool IsValidTrackerUrl(const std::string& url);

private:
  const ITextFetcher& fetcher_;
};

}  // namespace modlist
