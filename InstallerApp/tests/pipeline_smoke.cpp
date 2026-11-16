#include "extractor/PipelinedSevenZipExtractor.h"
#include "manifest/Manifest.h"
#include "verifier/Sha256.h"
#include "common/PathText.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <vector>

void WriteJsonString(std::ostream& out, const std::string& value) {
  out << '"';
  for (const unsigned char ch : value) {
    if (ch == '"' || ch == '\\') out << '\\' << ch;
    else if (ch < 0x20) {
      out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << static_cast<unsigned>(ch) << std::dec;
    } else out << ch;
  }
  out << '"';
}

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc < 3 || argc > 4) {
      throw std::runtime_error("Usage: pipeline_smoke <manifest.json> <new-output-folder> [cancel-percent], or --describe-package <manifest.json>");
    }
    const bool describe = argc == 3 && std::wstring_view(argv[1]) == L"--describe-package";
    const std::filesystem::path manifestPath(argv[describe ? 2 : 1]);
    if (!manifestPath.is_absolute()) {
      throw std::runtime_error("Use an absolute manifest path");
    }
    modlist::ManifestLoader loader;
    auto manifest = loader.LoadFromFile(manifestPath);
    if (!manifest.ok()) {
      throw std::runtime_error(manifest.error());
    }
    modlist::PipelinedSevenZipExtractor extractor;
    if (describe) {
      std::cout.imbue(std::locale::classic());
      std::cout << "{\"first_archive_part\":";
      WriteJsonString(std::cout, modlist::GenericPathToUtf8(manifest.value().extract.firstArchivePart));
      std::cout << ",\"pipeline_supported\":" << (extractor.CanUse(manifest.value()) ? "true" : "false")
                << ",\"payload_bytes\":" << manifest.value().unpackedSize << "}\n";
      return 0;
    }
    const std::filesystem::path output(argv[2]);
    if (!output.is_absolute() || std::filesystem::exists(output)) {
      throw std::runtime_error("Use absolute paths and an output folder that does not exist");
    }
    const int cancelPercent = argc == 4 ? std::stoi(argv[3]) : -1;
    if (argc == 4 && (cancelPercent < 1 || cancelPercent > 99)) {
      throw std::runtime_error("Cancellation percentage must be between 1 and 99");
    }
    if (!extractor.CanUse(manifest.value())) {
      throw std::runtime_error("Manifest requires full chunked validation");
    }
    std::vector<wchar_t> module(32768);
    const DWORD moduleLength = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (moduleLength == 0 || moduleLength >= module.size()) {
      throw std::runtime_error("Unable to locate the smoke runner");
    }
    auto library = extractor.LocateLibrary(std::filesystem::path(module.data()).parent_path());
    if (!library.ok()) {
      throw std::runtime_error(library.error());
    }
    auto dllHash = modlist::Sha256::FileHexDigest(library.value());
    if (!dllHash.ok()) {
      throw std::runtime_error(dllHash.error());
    }
    std::atomic_bool cancel{false};
    modlist::PipelineExtractionDiagnostics metrics;
    modlist::PipelinedExtractionConfig config;
    config.sevenZipLibrary = library.value();
    config.archiveFolder = manifestPath.parent_path();
    config.installFolder = output;
    config.manifest = &manifest.value();
    config.cancelRequested = &cancel;
    config.decoderThreads = 4;
    config.diagnostics = &metrics;
    modlist::PipelineExtractionProgress lastProgress;
    const auto start = std::chrono::steady_clock::now();
    const auto result = extractor.Extract(config, [&](const auto& progress) {
      lastProgress = progress;
      if (cancelPercent > 0 && progress.extractionPercent >= cancelPercent) {
        cancel.store(true);
      }
    });
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout.imbue(std::locale::classic());
    std::cout << std::setprecision(12)
              << "ok=" << result.ok << '\n'
              << "cancelled=" << cancel.load() << '\n'
              << "seconds=" << seconds << '\n'
              << "dll_sha256=" << dllHash.value() << '\n'
              << "validated_bytes=" << lastProgress.validatedBytes << '\n'
              << "validation_total_bytes=" << lastProgress.validationTotalBytes << '\n'
              << "input_read_bytes=" << metrics.inputDiskBytes.load() << '\n'
              << "block_loads=" << metrics.blockLoads.load() << '\n'
              << "block_reloads=" << metrics.blockReloads.load() << '\n'
              << "cache_hits=" << metrics.cacheHits.load() << '\n'
              << "cache_misses=" << metrics.cacheMisses.load() << '\n'
              << "demand_wait_ns=" << metrics.blockWaitNanoseconds.load() << '\n'
              << "read_wait_ns=" << metrics.diskWaitNanoseconds.load() << '\n'
              << "hash_ns=" << metrics.hashNanoseconds.load() << '\n'
              << "output_files=" << metrics.outputFilesCreated.load() << '\n'
              << "output_bytes=" << metrics.outputBytesWritten.load() << '\n'
              << "directory_ns=" << metrics.outputDirectoryNanoseconds.load() << '\n'
              << "open_ns=" << metrics.outputCreateNanoseconds.load() << '\n'
              << "write_ns=" << metrics.outputWriteNanoseconds.load() << '\n'
              << "close_ns=" << metrics.outputCloseNanoseconds.load() << '\n';
    std::cerr << result.message << '\n';
    if (result.ok) {
      if (cancelPercent > 0 || lastProgress.validatedBytes != lastProgress.validationTotalBytes ||
          metrics.outputBytesWritten.load() != manifest.value().unpackedSize) {
        throw std::runtime_error("Successful extraction reported incomplete validation/output or ignored cancellation");
      }
      return 0;
    }
    return cancel.load() ? 2 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Smoke failure: " << error.what() << '\n';
    return 1;
  }
}
