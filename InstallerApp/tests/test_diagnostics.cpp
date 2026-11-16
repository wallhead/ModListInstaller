#include "diagnostics/Diagnostics.h"
#include "diagnostics/PipelineProgressSnapshot.h"
#include "common/PathText.h"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace modlist::diagnostics;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void TestProgressPublishDoesNotWaitForLogSink() {
  using namespace std::chrono_literals;
  std::mutex logMutex;
  std::promise<void> sinkBlocked, releaseSink, callbackStarted, callbackFinished;
  auto release = releaseSink.get_future().share();
  PipelineProgressSnapshot progressLog([&](const std::string&) {
    std::lock_guard lock(logMutex);
  });
  std::thread sampler([&] {
    std::lock_guard lock(logMutex);
    sinkBlocked.set_value();
    release.wait();
  });
  sinkBlocked.get_future().wait();
  auto finished = callbackFinished.get_future();
  std::thread extractor([&] {
    modlist::PipelineExtractionProgress progress;
    progress.validationArchive = "Benchmark.7z.001";
    progress.validatedBytes = 4096;
    progress.extractionPercent = 37;
    callbackStarted.set_value();
    progressLog.Publish(progress);
    callbackFinished.set_value();
  });
  callbackStarted.get_future().wait();
  const bool published = finished.wait_for(2s) == std::future_status::ready;
  releaseSink.set_value();
  sampler.join();
  extractor.join();
  Check(published, "Extraction callback waited for the blocked diagnostic log sink");
}

void TestProgressPublishWhileSamplerSinkIsBlocked() {
  using namespace std::chrono_literals;
  std::promise<void> sinkEntered, releaseSink, callbackFinished;
  auto release = releaseSink.get_future().share();
  std::vector<std::string> lines;
  PipelineProgressSnapshot progressLog([&](const std::string& line) {
    if (lines.empty()) {
      sinkEntered.set_value();
      release.wait();
    }
    lines.push_back(line);
  });
  modlist::PipelineExtractionProgress progress;
  progress.validationArchive = "first.7z.001";
  progress.validatedBytes = 7;
  progress.extractionPercent = 12;
  progressLog.Publish(progress);
  std::thread sampler([&] { progressLog.Sample(); });
  const bool sampled = sinkEntered.get_future().wait_for(2s) == std::future_status::ready;
  auto finished = callbackFinished.get_future();
  std::atomic_bool cancel{false};
  std::thread extractor([&] {
    progress.validationArchive = "last.7z.002";
    progress.validatedBytes = 9001;
    progress.extractionPercent = 43;
    progressLog.Publish(progress);
    cancel = true;
    callbackFinished.set_value();
  });
  const bool published = finished.wait_for(2s) == std::future_status::ready;
  const bool cancelledBeforeDrain = cancel.load();
  releaseSink.set_value();
  sampler.join();
  extractor.join();
  Check(sampled, "Sampler did not deliver published progress to its log sink");
  Check(published, "Sampler held the progress snapshot lock while its log sink was blocked");
  Check(cancelledBeforeDrain, "Blocked progress logging delayed cancellation after publish");
  progressLog.Sample(true);
  Check(lines.size() == 2, "Final partial progress was lost after cancellation");
  Check(lines[0].find("first.7z.001; validation_bytes=7; extraction_percent=12") != std::string::npos,
        "Sampler did not retain a coherent snapshot while new progress arrived");
  Check(lines[1].find("last.7z.002; validation_bytes=9001; extraction_percent=43") != std::string::npos,
        "Cancelled extraction final progress was fabricated or lost");
}

void TestProgressSamplingCoalescesAndRetainsFinalState() {
  std::vector<std::string> lines;
  PipelineProgressSnapshot progressLog([&](const std::string& line) { lines.push_back(line); });
  progressLog.Sample(true);
  Check(lines.empty(), "Sampling without an extraction callback fabricated progress");
  modlist::PipelineExtractionProgress progress;
  progress.validationArchive = "Benchmark.7z.001";
  for (int percent = 0; percent <= 100; ++percent) {
    progress.validatedBytes = 12345;
    progress.extractionPercent = percent;
    progressLog.Publish(progress);
  }
  Check(lines.empty(), "Publish performed log I/O instead of coalescing progress in memory");
  progressLog.Sample();
  Check(lines.size() == 1 && lines.back().find("validation_bytes=12345; extraction_percent=100") != std::string::npos,
        "Sampling lost the final validation bytes or extraction percentage");
  progressLog.Sample();
  Check(lines.size() == 1, "Unchanged progress was logged again on a periodic sample");
  progress.validationArchive = "Benchmark.7z.002";
  progressLog.Publish(progress);
  progressLog.Sample();
  Check(lines.size() == 2 && lines.back().find("Benchmark.7z.002") != std::string::npos,
        "Archive transition with unchanged counters was lost");
  progressLog.Sample(true);
  Check(lines.size() == 3 && lines.back() == lines[1], "Final progress was not emitted after the last periodic sample");
}

int main() {
  try {
    TestProgressPublishDoesNotWaitForLogSink();
    TestProgressPublishWhileSamplerSinkIsBlocked();
    TestProgressSamplingCoalescesAndRetainsFinalState();
    constexpr uint64_t gib = 1024ull * 1024 * 1024;
    for (const uint64_t size : {10ull * gib, 20ull * gib, 30ull * gib}) {
      const auto files = BuildWorkload(size);
      uint64_t total = 0, smallBytes = 0, smallFiles = 0, directories = 0;
      std::set<std::filesystem::path> names;
      for (const auto& file : files) {
        Check(!file.relative.empty() && !file.relative.is_absolute(), "Unsafe workload path");
        for (const auto& part : file.relative) Check(part != ".." && part != ".", "Traversal workload path");
        Check(names.insert(file.relative).second, "Duplicate workload entry");
        Check(file.directory || file.size <= 2 * gib, "A file exceeds FAT32-safe fixture size");
        total += file.size;
        if (file.directory) ++directories;
        if (file.smallFile && !file.directory && file.size > 0) {smallBytes += file.size; ++smallFiles;}
      }
      Check(total == size, "Workload does not generate the requested real byte count");
      Check(smallBytes == size / 5 && smallFiles == 200000, "Small-file profile is wrong");
      Check(directories >= 2, "Missing empty-directory fixtures");
    }
    const auto smoke = BuildWorkload(64 * 1024 * 1024, true);
    uint64_t total = 0;
    for (const auto& file : smoke) total += file.size;
    Check(total == 64 * 1024 * 1024 && smoke.size() < 1000, "Smoke profile is not bounded");
    Check(SelectProgressBytes(ProgressBasis::ChildInput,0,60,0,100)==60,"Compression appears stalled while input reads continue without output writes");
    Check(SelectProgressBytes(ProgressBasis::ChildOutput,0,60,7,100)==7,"Extraction progress must follow unpacked output, not compressed input");
    Check(SelectProgressBytes(ProgressBasis::ChildInput,0,120,0,100)==100,"Process read progress exceeds the payload size");
    Check(SelectProgressBytes(ProgressBasis::LogicalBytes,17,60,7,100)==17,"Ordinary copy/generation counters were replaced by process I/O");
    Check(CounterDelta(70,50)==20 && CounterDelta(0,50)==0,"Failed/reset process counters wrap into huge speeds");
    Config config;
    Check(config.sizeGiB==10,"The default dataset must be 10 GiB");
    Check(TestParentForDrive(L"D:\\")==std::filesystem::path(L"D:\\ModlistDiagnostics"),"Drive selection does not use its own test folder");
    Check(TestParentForDrive(L"e:/")==std::filesystem::path(L"e:\\ModlistDiagnostics"),"Drive-root separators are not normalized");
    for(const auto* invalid:{L"",L"D:",L"D:\\Games",L"\\\\server\\share\\"}) {
      bool rejected=false;
      try{(void)TestParentForDrive(invalid);}catch(const std::invalid_argument&){rejected=true;}
      Check(rejected,"Non-disk input was accepted as a test disk");
    }
    Check(!ValidateConfig(config).empty(), "Empty paths were accepted");
    config.sourceParent = L"C:\\bench source";
    config.destinationParent = L"E:\\bench destination";
    Check(ValidateConfig(config).empty(), "Valid configuration rejected");
    config.sizeGiB = 11;
    Check(!ValidateConfig(config).empty(), "Unsupported dataset size accepted");
    config.sizeGiB = 20;
    config.threads = 0;
    Check(!ValidateConfig(config).empty(), "Zero decoder threads accepted");
    config.threads = 4;
    config.destinationParent = L"c:\\BENCH SOURCE\\";
    Check(!ValidateConfig(config).empty(), "Aliased same directory accepted");
    config.destinationParent = L"E:relative";
    Check(!ValidateConfig(config).empty(), "Drive-relative destination accepted");
    std::cout << "Diagnostics progress/workload/config tests passed\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "FAIL: " << ex.what() << '\n';
    return 1;
  }
}
