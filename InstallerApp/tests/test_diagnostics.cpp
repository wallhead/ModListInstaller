#include "diagnostics/Diagnostics.h"
#include "common/PathText.h"

#include <iostream>
#include <set>
#include <stdexcept>

using namespace modlist::diagnostics;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

int main() {
  try {
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
    std::cout << "Diagnostics workload/config tests passed\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "FAIL: " << ex.what() << '\n';
    return 1;
  }
}
