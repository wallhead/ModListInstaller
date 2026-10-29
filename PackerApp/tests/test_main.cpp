#include "../src/main.cpp"

#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

namespace {

std::filesystem::path g_fixtureRoot;
std::filesystem::path g_installerSource;
HWND g_testWindow = nullptr;
std::vector<std::wstring> g_messages;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void WriteFile(const std::filesystem::path& path, const std::string& contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  Require(static_cast<bool>(output), "Unable to write fixture: " + path.string());
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "Missing file: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::set<std::wstring> EntryNames(const std::filesystem::path& folder) {
  std::set<std::wstring> names;
  for (const auto& entry : std::filesystem::directory_iterator(folder)) {
    names.insert(entry.path().filename().wstring());
  }
  return names;
}

PackerConfig Config(const std::string& testName) {
  PackerConfig config;
  config.sourceFolder = g_fixtureRoot / testName / "source";
  config.releaseFolder = g_fixtureRoot / testName / "releases";
  config.archiveName = L"My Pack";
  config.format = L"7z";
  config.level = L"5";
  config.method = L"LZMA2";
  config.dictionary = L"1m";
  config.wordSize = L"32";
  config.solid = L"8g";
  config.volumeSize = L"8k";
  config.threads = L"2";
  config.chunkSize = 1024;
  config.copyInstaller = false;
  WriteFile(config.sourceFolder / "nested" / "readme.txt", "Small modlist payload\r\n");
  return config;
}

LRESULT CALLBACK TestWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == kLogMessage || message == kStatusMessage) {
    std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
    g_messages.push_back(*text);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

void DrainMessages() {
  MSG message{};
  while (PeekMessageW(&message, g_testWindow, 0, 0, PM_REMOVE)) {
    DispatchMessageW(&message);
  }
}

void FlatPreparation() {
  const auto config = Config("flat-preparation");
  WriteFile(config.releaseFolder / "other-release" / "keep.txt", "keep parent outputs");
  Require(ValidateConfig(g_testWindow, config), "Valid sibling source/release folders rejected");
  Require(PrepareInstallerSetup(g_testWindow, config), "Setup preparation failed with installer copying disabled");
  Require(EntryNames(config.releaseFolder) == std::set<std::wstring>{L"My Pack", L"other-release"},
          "Selected release parent must contain the archive-named child, without data/runtime folders");
  Require(EntryNames(config.releaseFolder / "My Pack").empty(),
          "Preparing without installer copying must not create runtime directories");
  Require(ReadFile(config.releaseFolder / "other-release" / "keep.txt") == "keep parent outputs",
          "Preparation changed another release");
}

void PortableInstallerPreparation() {
  auto config = Config("portable-installer");
  config.copyInstaller = true;
  const auto portable = g_fixtureRoot / "portable-installer" / "portable";
  WriteFile(portable / "modlist-installer.exe", "self-contained installer fixture");
  Require(ValidateConfig(g_testWindow, config), "Portable setup validation failed");
  Require(PrepareInstallerSetup(g_testWindow, config, portable),
          "Preparation must need only a sibling installer executable, without UI/tools files");
  const auto package = config.releaseFolder / "My Pack";
  Require(EntryNames(config.releaseFolder) == std::set<std::wstring>{L"My Pack"}, "Installer was copied into selected parent instead of child");
  Require(EntryNames(package) == std::set<std::wstring>{L"modlist-installer.exe"}, "Installer preparation created runtime files/directories");
  Require(ReadFile(package / "modlist-installer.exe") == "self-contained installer fixture", "Wrong installer bytes copied");
  WriteFile(portable / "data" / "ui" / "style.css", "old theme");
  WriteFile(portable / "data" / "tools" / "7zip" / "7z.exe", "old tool");
  WriteFile(portable / "modlist-installer.exe", "updated installer");
  Require(PrepareInstallerSetup(g_testWindow, config, portable), "Refreshing installer failed");
  Require(EntryNames(package) == std::set<std::wstring>{L"modlist-installer.exe"}, "Legacy portable data was copied into release");
  Require(ReadFile(package / "modlist-installer.exe") == "updated installer", "Installer refresh did not replace executable");
  Require(ReadFile(portable / "data" / "ui" / "style.css") == "old theme", "Preparation deleted old portable data");
  Require(PrepareInstallerSetup(g_testWindow, config, package), "Copying installer onto itself should be harmless");
}

void MissingInstallerDoesNotFallBackToRuntimeFiles() {
  auto config = Config("missing-installer");
  config.copyInstaller = true;
  const auto portable = g_fixtureRoot / "missing-installer" / "portable";
  WriteFile(portable / "data" / "ui" / "style.css", "not an installer");
  Require(!PrepareInstallerSetup(g_testWindow, config, portable), "Missing sibling installer must fail preparation");
  const auto package = config.releaseFolder / "My Pack";
  Require(!std::filesystem::exists(package) || EntryNames(package).empty(), "Missing installer left runtime directories");
}

void ManifestOnlyUsesFlatRootAndRealArchiveName() {
  auto config = Config("manifest-only");
  config.archiveFirst = false;
  config.chunkSize = 3;
  const auto package = config.releaseFolder / "My Pack";
  WriteFile(package / "Actual Name.zip.001", "abcdef");
  WriteFile(package / "Actual Name.zip.002", "");
  WriteFile(package / "Actual Name.zip.003.tmp", "not an archive part");
  WriteFile(package / "packing.log", "keep log");
  Require(ValidateConfig(g_testWindow, config), "Manifest Only rejected existing flat package");
  Require(PrepareInstallerSetup(g_testWindow, config), "Manifest Only preparation failed");
  Require(WriteManifest(g_testWindow, config, 42), "Manifest Only did not find archive parts in archive-named child");
  const auto manifest = ReadFile(package / "manifest.json");
  for (const auto* expected : {
           "\"archive_name\": \"Actual Name\"", "\"unpacked_size\": 42", "\"chunk_size\": 3",
           "\"path\": \"Actual Name.zip.001\"", "\"path\": \"Actual Name.zip.002\"",
           "bef57ec7f53a6d40beb640a780a639c83bc29ac8a9816f1fc6c5c6dcd93c4721",
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "cb8379ac2098aa165029e3938a51da0bcecfc008fd6795f401178647f96c5b34",
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"}) {
    Require(manifest.find(expected) != std::string::npos, "Manifest missing expected field/hash: " + std::string(expected));
  }
  Require(manifest.find(".tmp") == std::string::npos && manifest.find("packing.log") == std::string::npos,
          "Manifest included a temporary file or log");
  Require(ReadFile(package / "packing.log") == "keep log", "Manifest Only removed unrelated files");
  Require(EntryNames(config.releaseFolder) == std::set<std::wstring>{L"My Pack"},
          "Manifest Only created files outside archive-named child");
}

void ManifestOnlySelectionAndAmbiguity() {
  auto config = Config("manifest-selection");
  config.archiveFirst = false;
  const auto package = config.releaseFolder / "My Pack";
  WriteFile(package / "My Pack.7z", "selected");
  WriteFile(package / "Other.zip", "other");
  auto selected = SelectArchiveSet(g_testWindow, config);
  Require(selected.has_value() && selected->baseName == L"My Pack.7z", "Named flat archive was not selected");
  config.archiveName = L"Ambiguous";
  const auto ambiguous = config.releaseFolder / "Ambiguous";
  WriteFile(ambiguous / "First.7z", "first");
  WriteFile(ambiguous / "Second.zip", "second");
  Require(!WriteManifest(g_testWindow, config, 0), "Ambiguous archive set should be rejected");
  Require(!std::filesystem::exists(ambiguous / "manifest.json"), "Ambiguous set wrote a manifest");
}

void CleanupIsNameScopedAndPreservesDirectories() {
  const auto config = Config("cleanup");
  const auto package = config.releaseFolder / "My Pack";
  for (const auto* name : {"My Pack.7z", "My Pack.7z.001", "My Pack.zip.001", "My Pack.7z.tmp",
                           "My Pack.7z.002.tmp", "manifest.json"}) {
    WriteFile(package / name, "stale generated output");
  }
  for (const auto* name : {"Other.7z.001", "Other.7z.tmp", "My Pack.7z.log", "My Pack.7z.notes.tmp",
                           "My Pack.7z.001.bak", "notes.txt"}) {
    WriteFile(package / name, "keep");
  }
  WriteFile(package / "data" / "ui" / "keep.txt", "legacy data");
  WriteFile(package / "My Pack.zip.002.tmp" / "keep.txt", "directory is not an archive part");
  WriteFile(config.releaseFolder / "My Pack.7z.001", "keep parent archive");
  WriteFile(config.releaseFolder / "Other" / "manifest.json", "keep sibling manifest");
  Require(RemovePreviousPackageOutputs(g_testWindow, config), "Scoped cleanup failed");
  Require(EntryNames(package) == std::set<std::wstring>{L"Other.7z.001", L"Other.7z.tmp", L"My Pack.7z.log",
              L"My Pack.7z.notes.tmp", L"My Pack.7z.001.bak", L"notes.txt", L"data", L"My Pack.zip.002.tmp"},
          "Cleanup must remove only selected archive parts/temps and manifest, preserving unrelated output");
  Require(ReadFile(package / "data" / "ui" / "keep.txt") == "legacy data", "Cleanup removed legacy data");
  Require(ReadFile(config.releaseFolder / "My Pack.7z.001") == "keep parent archive", "Cleanup touched parent archive");
  Require(ReadFile(config.releaseFolder / "Other" / "manifest.json") == "keep sibling manifest", "Cleanup touched sibling release");
}

void MissingPackageCleanupIsHarmless() {
  const auto config = Config("missing-cleanup");
  Require(RemovePreviousPackageOutputs(g_testWindow, config), "Missing output should not prevent first build");
  Require(!std::filesystem::exists(config.releaseFolder), "Cleanup created output directories");
}

void CleanupDoesNotDeleteManifestDirectory() {
  const auto config = Config("manifest-directory");
  const auto manifest = config.releaseFolder / "My Pack" / "manifest.json";
  std::filesystem::create_directories(manifest);
  Require(!RemovePreviousPackageOutputs(g_testWindow, config), "A manifest directory must be rejected, not removed");
  Require(std::filesystem::is_directory(manifest), "Cleanup deleted a directory named manifest.json");
}

void OverlappingFoldersAreRejectedBeforeWriting() {
  for (int mode = 0; mode < 3; ++mode) {
    auto config = Config("overlap-" + std::to_string(mode));
    const auto source = config.sourceFolder;
    if (mode == 0) config.releaseFolder = source;
    if (mode == 1) config.releaseFolder = source / "releases";
    if (mode == 2) config.releaseFolder = source.parent_path();
    Require(!ValidateConfig(g_testWindow, config), "Same/nested source and release folders must be rejected");
    Require(ReadFile(source / "nested" / "readme.txt") == "Small modlist payload\r\n", "Validation changed source files");
    Require(!std::filesystem::exists(config.releaseFolder / "My Pack"), "Overlap validation created package output");
  }
  Require(!PathsOverlap(g_fixtureRoot / "source", g_fixtureRoot / "source-other"), "Sibling path prefix is not overlap");
}

void UnsafeArchiveNamesAreRejectedBeforeWriting() {
  const auto config = Config("unsafe-names");
  for (const auto* name : {L"", L".", L"..", L"../escape", L"path/name", L"path\\name", L"C:drive",
                           L"trailing.", L"trailing ", L"NUL", L"CON.txt", L"com1", L"bad\tname"}) {
    auto invalid = config;
    invalid.archiveName = name;
    Require(!ValidateConfig(g_testWindow, invalid), "Unsafe archive name accepted: " + Narrow(name));
    Require(!std::filesystem::exists(config.releaseFolder), "Invalid name validation created release output");
  }
}

void RealCompressionTestAndManifest() {
  auto config = Config("compression");
  std::string payload(16 * 1024, '\0');
  uint32_t state = 0x12345678;
  for (auto& byte : payload) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    byte = static_cast<char>(state & 0xff);
  }
  WriteFile(config.sourceFolder / "nested" / "payload.bin", payload);
  auto portable = g_installerSource;
  if (portable.empty()) {
    portable = g_fixtureRoot / "compression" / "portable";
    WriteFile(portable / "modlist-installer.exe", "self-contained installer fixture");
  }
  const auto installerBytes = ReadFile(portable / "modlist-installer.exe");
  auto setupConfig = config;
  setupConfig.copyInstaller = true;
  Require(PrepareInstallerSetup(g_testWindow, setupConfig, portable), "Unable to prepare actual compression fixture installer");
  Worker(g_testWindow, config);
  const auto package = config.releaseFolder / "My Pack";
  Require(std::filesystem::exists(package / "manifest.json"), "Real worker did not write flat manifest");
  const auto names = EntryNames(package);
  Require(names.contains(L"My Pack.7z.001") && names.contains(L"My Pack.7z.002"), "Real 7-Zip did not create split volumes");
  for (const auto& entry : std::filesystem::directory_iterator(package)) {
    Require(entry.is_regular_file(), "Real package contains an unexpected directory");
    Require(entry.path().filename() == "manifest.json" || entry.path().filename() == "modlist-installer.exe" ||
                (entry.path().filename().wstring().starts_with(L"My Pack.7z.") &&
                 IsNumericVolumeSuffix(entry.path().extension().wstring().substr(1))),
            "Real package contains an unexpected file");
  }
  Require(ReadFile(package / "modlist-installer.exe") == installerBytes, "Worker changed the prepared installer");
  Require(ResolveSevenZip(g_testWindow, config), "Embedded 7-Zip resource not available in test executable");
  Require(RunProcess(g_testWindow, BuildSevenZipTestCommand(config), L"Test fixture archive", 0) == 0,
          "Actual generated split archive failed 7-Zip integrity test");
  const auto extracted = g_fixtureRoot / "compression" / "extracted";
  const auto command = Quote(config.sevenZipExe) + L" x " + Quote(package / "My Pack.7z.001") +
                       L" -o" + Quote(extracted) + L" -y";
  Require(RunProcess(g_testWindow, command, L"Extract fixture archive", 0) == 0, "Generated archive could not be extracted");
  Require(ReadFile(extracted / "nested" / "payload.bin") == payload, "Compression changed payload bytes or relative paths");
  Require(ReadFile(extracted / "nested" / "readme.txt") == "Small modlist payload\r\n", "Archive lost a nested source file");
  const auto manifest = ReadFile(package / "manifest.json");
  Require(manifest.find("\"path\": \"My Pack.7z.001\"") != std::string::npos, "Manifest archive paths must be root-relative filenames");
  Require(manifest.find("\"unpacked_size\": 16407") != std::string::npos, "Worker did not retain scanned unpacked size");
  DrainMessages();
  Require(std::any_of(g_messages.begin(), g_messages.end(), [](const auto& text) { return text.starts_with(L"7-Zip RAM limit: 80%"); }),
          "Worker stopped applying the configured RAM limit");
  Require(std::find(g_messages.begin(), g_messages.end(), L"Archive test completed.") != g_messages.end(),
          "Worker skipped the requested integrity test");
}

void RealSingleZipCompression() {
  auto config = Config("zip-compression");
  config.format = L"zip";
  config.volumeSize = L"none";
  Worker(g_testWindow, config);
  const auto package = config.releaseFolder / "My Pack";
  Require(EntryNames(package) == std::set<std::wstring>{L"My Pack.zip", L"manifest.json"},
          "Unsplit ZIP worker must write only archive and manifest to package root when copying is disabled");
  Require(ResolveSevenZip(g_testWindow, config), "Unable to resolve embedded 7-Zip for ZIP verification");
  Require(RunProcess(g_testWindow, BuildSevenZipTestCommand(config), L"Test unsplit ZIP", 0) == 0,
          "Unsplit ZIP integrity test used the wrong archive path or failed");
  Require(ReadFile(package / "manifest.json").find("\"path\": \"My Pack.zip\"") != std::string::npos,
          "ZIP manifest must use root-relative archive path");
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  try {
    if (argc != 1) {
      Require(argc == 3 && std::wstring(argv[1]) == L"--installer-source",
              "Usage: packer_tests [--installer-source <folder containing modlist-installer.exe>]");
      g_installerSource = std::filesystem::absolute(argv[2]);
      Require(std::filesystem::is_regular_file(g_installerSource / "modlist-installer.exe"),
              "Installer source folder must contain modlist-installer.exe");
    }
    g_fixtureRoot = ModuleFolder() / "test-fixtures" /
        ("flat-layout-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    Require(std::filesystem::create_directories(g_fixtureRoot), "Fixture directory already exists");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = TestWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"PackerConsoleTestMessages";
    Require(RegisterClassW(&windowClass) != 0, "Unable to register test message window");
    g_testWindow = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    Require(g_testWindow != nullptr, "Unable to create test message window");
    std::cout << "Fixture root: " << g_fixtureRoot.string() << std::endl;
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"flat preparation", FlatPreparation},
        {"portable installer preparation and refresh", PortableInstallerPreparation},
        {"missing installer rejection", MissingInstallerDoesNotFallBackToRuntimeFiles},
        {"Manifest Only root paths, filtering and known SHA-256 hashes", ManifestOnlyUsesFlatRootAndRealArchiveName},
        {"Manifest Only archive selection and ambiguity", ManifestOnlySelectionAndAmbiguity},
        {"name-scoped cleanup preserves unrelated outputs", CleanupIsNameScopedAndPreservesDirectories},
        {"missing package cleanup", MissingPackageCleanupIsHarmless},
        {"cleanup preserves manifest directory", CleanupDoesNotDeleteManifestDirectory},
        {"source/release overlap rejection", OverlappingFoldersAreRejectedBeforeWriting},
        {"unsafe archive name rejection", UnsafeArchiveNamesAreRejectedBeforeWriting},
        {"real compression, integrity test, extraction and manifest", RealCompressionTestAndManifest},
        {"real unsplit ZIP compression and manifest", RealSingleZipCompression}};
    int failures = 0;
    for (const auto& [name, test] : tests) {
      g_cancelRequested = false;
      g_messages.clear();
      try {
        test();
        std::cout << "PASS: " << name << '\n';
      } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << error.what() << '\n';
        DrainMessages();
        for (const auto& message : g_messages) {
          std::cerr << "  " << Narrow(message) << '\n';
        }
      }
      DrainMessages();
    }
    DestroyWindow(g_testWindow);
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "Test harness failed: " << error.what() << '\n';
    return 1;
  }
}
