#include "../src/main.cpp"

#include <cwchar>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string_view>

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
  Require(static_cast<bool>(output), "Unable to write fixture: " + Narrow(path.wstring()));
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  Require(static_cast<bool>(input), "Missing file: " + Narrow(path.wstring()));
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string Utf8Bytes(std::u8string_view text) {
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

class ScopedLocalAppData {
 public:
  explicit ScopedLocalAppData(const std::filesystem::path& folder) {
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (length > 0) {
      previous_.resize(length);
      const DWORD copied = GetEnvironmentVariableW(L"LOCALAPPDATA", previous_.data(), length);
      Require(copied > 0 && copied < length, "Unable to save LOCALAPPDATA");
      previous_.resize(copied);
      hadPrevious_ = true;
    }
    Require(SetEnvironmentVariableW(L"LOCALAPPDATA", folder.c_str()) != FALSE,
            "Unable to isolate the embedded tool cache");
  }

  ~ScopedLocalAppData() {
    SetEnvironmentVariableW(L"LOCALAPPDATA", hadPrevious_ ? previous_.c_str() : nullptr);
  }

 private:
  std::wstring previous_;
  bool hadPrevious_{false};
};

class OwnedFixtureTree {
 public:
  explicit OwnedFixtureTree(const std::filesystem::path& parent, bool keep = false) : keep_(keep) {
    parent_ = std::filesystem::absolute(parent).lexically_normal();
    std::filesystem::create_directories(parent_);
    Require(std::filesystem::canonical(parent_) == parent_, "Fixture parent must not redirect outside its known path");
    static unsigned int sequence = 0;
    root_ = parent_ / ("flat-layout-" + std::to_string(GetCurrentProcessId()) + "-" +
                       std::to_string(GetTickCount64()) + "-" + std::to_string(sequence++));
    Require(std::filesystem::create_directory(root_), "Fixture directory already exists");
    WriteFile(root_ / ".packer-tests-owned", root_.filename().string());
    owned_ = true;
  }

  OwnedFixtureTree(const OwnedFixtureTree&) = delete;
  OwnedFixtureTree& operator=(const OwnedFixtureTree&) = delete;

  ~OwnedFixtureTree() {
    if (owned_ && !keep_) {
      try {
        Cleanup();
      } catch (const std::exception& error) {
        std::cerr << "Fixture cleanup refused: " << Narrow(root_.wstring()) << ": " << error.what() << '\n';
      }
    }
  }

  const std::filesystem::path& Root() const { return root_; }

  void Cleanup() {
    if (!owned_) return;
    // Never sweep test-fixtures or older runs, and never traverse a redirected directory.
    Require(root_.parent_path() == parent_ && std::filesystem::canonical(root_) == root_ &&
                std::filesystem::canonical(parent_) == parent_, "Fixture cleanup escaped its generated root");
    Require(ReadFile(root_ / ".packer-tests-owned") == root_.filename().string(),
            "Fixture ownership marker is missing or changed");
    auto checkEntry = [](const std::filesystem::path& path) {
      const DWORD attributes = GetFileAttributesW(path.c_str());
      Require(attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0,
              "Fixture cleanup must not traverse reparse points");
    };
    checkEntry(root_);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root_)) checkEntry(entry.path());
    std::filesystem::remove_all(root_);
    owned_ = false;
  }

 private:
  std::filesystem::path parent_;
  std::filesystem::path root_;
  bool keep_{false};
  bool owned_{false};
};

std::string CaptureProcess(const std::wstring& command, const std::filesystem::path& outputPath) {
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE output = CreateFileW(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  Require(output != INVALID_HANDLE_VALUE, "Unable to create process output fixture");
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  auto mutableCommand = command;
  const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(output);
  Require(created != FALSE, "Unable to start captured process");
  const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
  if (wait != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 255);
    WaitForSingleObject(process.hProcess, INFINITE);
  }
  DWORD exitCode = 255;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  const auto result = ReadFile(outputPath);
  Require(wait == WAIT_OBJECT_0 && exitCode == 0, "Captured process failed: " + result);
  return result;
}

std::vector<std::filesystem::path> ListedArchivePaths(const PackerConfig& config,
                                                     const std::filesystem::path& outputPath) {
  const auto listing = CaptureProcess(Quote(config.sevenZipExe) + L" l -slt -ba -sccUTF-8 " +
                                         Quote(ArchivePath(config)), outputPath);
  std::vector<std::filesystem::path> paths;
  std::istringstream lines(listing);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("Path = ")) paths.emplace_back(Widen(line.substr(7)));
  }
  Require(!paths.empty(), "7-Zip metadata listing has no entries: " + listing);
  return paths;
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
  config.pathMode = L"Relative paths";
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

void FixtureCleanupIsOwnedAndRunScoped() {
  const auto parent = g_fixtureRoot / "fixture-cleanup-scope";
  WriteFile(parent / "notes.txt", "unrelated parent file");
  WriteFile(parent / "older-run" / "keep.txt", "unrelated older fixture");
  std::filesystem::path generated;
  {
    OwnedFixtureTree fixture(parent);
    generated = fixture.Root();
    WriteFile(generated / "nested" / "generated.txt", "owned fixture");
  }
  Require(!std::filesystem::exists(generated), "Default cleanup retained its own generated fixture tree");
  Require(ReadFile(parent / "notes.txt") == "unrelated parent file" &&
              ReadFile(parent / "older-run" / "keep.txt") == "unrelated older fixture",
          "Fixture cleanup removed unrelated parent files or older runs");
  OwnedFixtureTree unmarked(parent);
  const auto marker = unmarked.Root() / ".packer-tests-owned";
  WriteFile(marker, "not this run");
  WriteFile(unmarked.Root() / "keep.txt", "do not delete an unowned tree");
  bool rejected = false;
  try { unmarked.Cleanup(); } catch (const std::exception&) { rejected = true; }
  Require(rejected && ReadFile(unmarked.Root() / "keep.txt") == "do not delete an unowned tree",
          "Fixture cleanup removed a tree without its ownership marker");
  WriteFile(marker, unmarked.Root().filename().string());
  unmarked.Cleanup();
  {
    OwnedFixtureTree retained(parent, true);
    generated = retained.Root();
    WriteFile(generated / "generated.txt", "retained fixture");
  }
  Require(ReadFile(generated / "generated.txt") == "retained fixture", "Opt-in fixture retention was ignored");
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

void CyrillicManifestUsesUtf8AndKnownHashes() {
  auto config = Config("cyrillic-manifest");
  config.archiveFirst = false;
  config.archiveName = L"\u0421\u0431\u043e\u0440\u043a\u0430";
  config.chunkSize = 3;
  const auto package = PackageFolder(config);
  WriteFile(package / (config.archiveName + L".7z.001"), "abcdef");
  Require(WriteManifest(g_testWindow, config, 6), "Cyrillic manifest generation failed");
  const auto manifest = ReadFile(package / "manifest.json");
  Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, manifest.data(),
                              static_cast<int>(manifest.size()), nullptr, 0) > 0,
          "Manifest contains invalid UTF-8 bytes");
  const auto filename = Utf8Bytes(u8"\u0421\u0431\u043e\u0440\u043a\u0430.7z.001");
  Require(manifest.find("\"path\": \"" + filename + "\"") != std::string::npos,
          "Cyrillic files[].path is not the expected UTF-8 filename");
  Require(ReadFile(package / Widen(filename)) == "abcdef", "UTF-8 manifest filename did not roundtrip");
  for (const auto* hash : {
           "bef57ec7f53a6d40beb640a780a639c83bc29ac8a9816f1fc6c5c6dcd93c4721",
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "cb8379ac2098aa165029e3938a51da0bcecfc008fd6795f401178647f96c5b34"}) {
    Require(manifest.find(hash) != std::string::npos, "Cyrillic manifest lost known full/chunk SHA-256");
  }
}

void EmbeddedCacheRepairsSameSizeTampering() {
  std::wstring error;
  const auto cache = ExtractEmbeddedSevenZip(error);
  Require(cache.has_value(), "Embedded extraction failed: " + Narrow(error));
  Require(IsSameOrChildPath(*cache, g_fixtureRoot), "Test must not tamper with the user's real tool cache");
  const HMODULE module = GetModuleHandleW(nullptr);
  const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_7ZIP_EXE), MAKEINTRESOURCEW(10));
  const HGLOBAL loaded = LoadResource(module, resource);
  const auto* data = static_cast<const char*>(LockResource(loaded));
  const DWORD size = SizeofResource(module, resource);
  Require(data != nullptr && size > 1, "Unable to inspect embedded reference bytes");
  const std::string expected(data, size);
  Require(ReadFile(*cache) == expected, "First extraction differs from embedded bytes");
  auto tampered = expected;
  tampered[tampered.size() / 2] ^= 0x01;
  WriteFile(*cache, tampered);
  Require(std::filesystem::file_size(*cache) == expected.size(), "Tamper fixture changed executable size");
  const auto repaired = ExtractEmbeddedSevenZip(error);
  Require(repaired.has_value() && *repaired == *cache, "Tampered cache was not regenerated");
  const auto repairedBytes = ReadFile(*cache);
  // Restore even on a failed assertion so later integration tests never execute tampered bytes.
  if (repairedBytes != expected) WriteFile(*cache, expected);
  Require(repairedBytes == expected, "Same-size tampered cache was trusted instead of restored");
  const auto timestamp = std::filesystem::last_write_time(*cache) - std::chrono::hours(1);
  std::filesystem::last_write_time(*cache, timestamp);
  Require(ExtractEmbeddedSevenZip(error).has_value(), "Valid cache could not be reused");
  Require(std::filesystem::last_write_time(*cache) == timestamp, "Valid cache was needlessly rewritten");
}

void RealSevenZipFullPathMetadata() {
  auto config = Config("full-path-metadata");
  config.volumeSize = L"none";
  Require(ValidateConfig(g_testWindow, config), "Relative-path fixture was rejected");
  Require(ResolveSevenZip(g_testWindow, config), "Unable to resolve 7-Zip for metadata reproduction");
  Require(RunProcess(g_testWindow, BuildSevenZipCommand(config), L"Check relative paths", 0) == 0,
          "Actual relative-path packing failed");
  const auto relativePaths = ListedArchivePaths(config, config.sourceFolder.parent_path() / "relative-path-listing.txt");
  Require(std::find(relativePaths.begin(), relativePaths.end(), std::filesystem::path(L"nested/readme.txt")) != relativePaths.end() &&
              std::none_of(relativePaths.begin(), relativePaths.end(), [](const auto& path) {
                return path.has_root_path();
              }), "Relative-path archive metadata contains a rooted path or lost its nested filename");
  config.archiveName = L"FullPaths";
  Require(ValidateConfig(g_testWindow, config), "Unable to prepare full-path reproduction output");
  // Deliberately bypass config validation to characterize the unsafe 7-Zip switch itself.
  Require(RunProcess(g_testWindow, BuildSevenZipCommand(config) + L" -spf", L"Reproduce -spf", 0) == 0,
          "Actual 7-Zip -spf packing failed");
  const auto paths = ListedArchivePaths(config, config.sourceFolder.parent_path() / "full-path-listing.txt");
  const auto expected = config.sourceFolder / "nested" / "readme.txt";
  Require(std::find(paths.begin(), paths.end(), expected) != paths.end(),
          "-spf reproduction did not retain the absolute source filename");
  Require(std::all_of(paths.begin(), paths.end(), [](const auto& path) { return path.is_absolute(); }),
          "-spf reproduction did not produce absolute archive entries");
  std::cout << "REPRO: -spf archive entry: " << Narrow(expected.wstring()) << '\n';
}

void UnsafePathModesAreRejectedBeforeWriting() {
  for (const auto* mode : {L"Full paths", L"unknown mode"}) {
    auto config = Config("rejected-path-mode-" + std::to_string(std::wcslen(mode)));
    config.pathMode = mode;
    Require(!ValidateConfig(g_testWindow, config), "Unsafe path mode accepted: " + Narrow(mode));
    Require(!std::filesystem::exists(config.releaseFolder), "Unsafe path mode created release output");
  }
  size_t index = 0;
  for (const auto* parameters : {L"-spf", L"-spf2", L"-mx=3 \"-spf\"", L"-SPF", L"/spf"}) {
    auto config = Config("rejected-spf-" + std::to_string(index++));
    config.parameters = parameters;
    Require(!ValidateConfig(g_testWindow, config), "Full paths accepted via parameters: " + Narrow(parameters));
    Require(!std::filesystem::exists(config.releaseFolder), "Unsafe parameters created release output");
  }
}

void NullTargetsDoNotQueueOwnedStrings() {
  PostLog(nullptr, L"unowned log");
  PostStatus(nullptr, L"unowned status");
  MSG message{};
  bool queued = false;
  while (PeekMessageW(&message, reinterpret_cast<HWND>(static_cast<INT_PTR>(-1)),
                       kLogMessage, kStatusMessage, PM_REMOVE)) {
    std::unique_ptr<std::wstring> payload(reinterpret_cast<std::wstring*>(message.lParam));
    queued = true;
  }
  Require(!queued, "Null PostMessage target queued strings with no owning window handler");
  PostLog(g_testWindow, L"owned log");
  PostStatus(g_testWindow, L"owned status");
  DrainMessages();
  Require(std::find(g_messages.begin(), g_messages.end(), L"owned log") != g_messages.end() &&
              std::find(g_messages.begin(), g_messages.end(), L"owned status") != g_messages.end(),
          "Valid log/status messages no longer transfer to the receiving window");
}

LRESULT CALLBACK HiddenPackerWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_DESTROY) return 0;
  return WindowProc(hwnd, message, wParam, lParam);
}

void PackerLogRetainsLongOutputAndDefaults() {
  g_instance = GetModuleHandleW(nullptr);
  WNDCLASSW windowClass{};
  windowClass.lpfnWndProc = HiddenPackerWindowProc;
  windowClass.hInstance = g_instance;
  windowClass.lpszClassName = L"HiddenPackerLogTest";
  Require(RegisterClassW(&windowClass) != 0, "Unable to register hidden packer window");
  const HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"", WS_OVERLAPPED,
                                      0, 0, 735, 710, nullptr, nullptr, g_instance, nullptr);
  Require(window != nullptr, "Unable to create hidden packer controls");
  try {
    const auto defaults = ReadConfig(true);
    Require(defaults.level == L"5" && defaults.method == L"LZMA2" && defaults.dictionary == L"32m" &&
                defaults.wordSize == L"32" && defaults.solid == L"8g" && defaults.threads == L"20" &&
                SevenZipVolumeSize(defaults.volumeSize) == L"4092m" && defaults.memoryLimitPercent == 80 &&
                defaults.chunkSize == 64ull * 1024ull * 1024ull,
            "Review fixes changed the compression/hash/memory defaults");
    const auto limit = SendMessageW(g_logEdit, EM_GETLIMITTEXT, 0, 0);
    std::cout << "REPRO: packer log EM_GETLIMITTEXT = " << limit << '\n';
    const std::wstring line(100, L'x');
    for (int i = 0; i < 700; ++i) AppendLog(line);
    AppendLog(L"final-log-marker");
    const auto log = GetText(g_logEdit);
    Require(log.size() > 70000 && log.ends_with(L"final-log-marker\r\n"),
            "Packer log truncated output beyond the edit-control default limit");
    Require(defaults.pathMode == L"Relative paths" &&
                SendMessageW(g_pathModeCombo, CB_GETCOUNT, 0, 0) == 1,
            "Packer UI still offers the incompatible Full paths mode");
  } catch (...) {
    DestroyWindow(window);
    throw;
  }
  DestroyWindow(window);
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

void RealCyrillicPackingAndManifestRoundTrip() {
  auto config = Config("cyrillic-compression");
  const auto caseRoot = config.sourceFolder.parent_path();
  config.sourceFolder = caseRoot / L"\u0418\u0441\u0442\u043e\u0447\u043d\u0438\u043a";
  config.releaseFolder = caseRoot / L"\u0420\u0435\u043b\u0438\u0437\u044b";
  config.archiveName = L"\u0421\u0431\u043e\u0440\u043a\u0430";
  config.chunkSize = 3;
  const std::filesystem::path payloadName = L"\u041c\u043e\u0434\u044b/\u0442\u0435\u0441\u0442.txt";
  WriteFile(config.sourceFolder / payloadName, "abcdef");
  if (!g_installerSource.empty()) {
    auto setupConfig = config;
    setupConfig.copyInstaller = true;
    Require(PrepareInstallerSetup(g_testWindow, setupConfig, g_installerSource),
            "Unable to prepare the Cyrillic E2E fixture installer");
  }
  Worker(g_testWindow, config);
  const auto package = PackageFolder(config);
  if (!g_installerSource.empty()) {
    Require(ReadFile(package / "modlist-installer.exe") == ReadFile(g_installerSource / "modlist-installer.exe"),
            "Cyrillic E2E fixture did not preserve the supplied installer");
  }
  const auto manifest = ReadFile(package / "manifest.json");
  Require(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, manifest.data(),
                              static_cast<int>(manifest.size()), nullptr, 0) > 0,
          "Packed Cyrillic manifest is not valid UTF-8");
  const auto archiveName = Utf8Bytes(u8"\u0421\u0431\u043e\u0440\u043a\u0430.7z.001");
  Require(manifest.find("\"path\": \"" + archiveName + "\"") != std::string::npos,
          "Real packing did not write the literal Cyrillic UTF-8 archive filename");
  // Use a real JSON parser and independent hashes, then pass the parsed UTF-8 path back to 7-Zip.
  const auto script = caseRoot / "roundtrip.ps1";
  WriteFile(script, R"ps(param([string]$ManifestPath, [string]$ExpectedName, [string]$SevenZip, [string]$Extracted)
$ErrorActionPreference = 'Stop'
$manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
if ($manifest.archive_name -cne $ExpectedName -or @($manifest.files).Count -ne 1) { throw 'Wrong archive name/file count' }
$file = $manifest.files[0]
if ($file.path -cne ($ExpectedName + '.7z.001')) { throw 'JSON UTF-8 path roundtrip failed' }
$archive = Join-Path (Split-Path -Parent $ManifestPath) $file.path
if ((Get-Item -LiteralPath $archive).Length -ne $file.size) { throw 'Manifest size mismatch' }
$bytes = [IO.File]::ReadAllBytes($archive)
$sha = [Security.Cryptography.SHA256]::Create()
try {
  $fullHash = [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
  if ($fullHash -cne $file.sha256) { throw 'Manifest full hash mismatch' }
  $index = 0
  for ($offset = 0; $offset -lt $bytes.Length; $offset += $manifest.hash.chunk_size) {
    $count = [int][Math]::Min($manifest.hash.chunk_size, $bytes.Length - $offset)
    $hash = [BitConverter]::ToString($sha.ComputeHash($bytes, $offset, $count)).Replace('-', '').ToLowerInvariant()
    if ($hash -cne $file.chunks[$index]) { throw 'Manifest chunk hash mismatch' }
    $index++
  }
  if ($index -ne @($file.chunks).Count) { throw 'Wrong manifest chunk count' }
} finally { $sha.Dispose() }
& $SevenZip x $archive ('-o' + $Extracted) -y | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Parsed manifest archive could not be extracted' }
Write-Output ('JSON roundtrip and independent full/chunk SHA-256 verified: ' + $file.sha256)
)ps");
  Require(ResolveSevenZip(g_testWindow, config), "Unable to resolve 7-Zip for Cyrillic extraction");
  const auto extracted = caseRoot / L"\u0420\u0430\u0441\u043f\u0430\u043a\u043e\u0432\u0430\u043d\u043e";
  const auto output = CaptureProcess(L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                                        Quote(script) + L" -ManifestPath " + Quote(package / "manifest.json") +
                                        L" -ExpectedName " + Quote(config.archiveName) + L" -SevenZip " +
                                        Quote(config.sevenZipExe) + L" -Extracted " + Quote(extracted),
                                    caseRoot / "roundtrip-output.txt");
  Require(output.find("JSON roundtrip and independent full/chunk SHA-256 verified") != std::string::npos,
          "Independent manifest roundtrip did not finish");
  Require(ReadFile(extracted / payloadName) == "abcdef", "Cyrillic source filename/payload did not roundtrip");
  Require(ReadFile(extracted / payloadName) == ReadFile(config.sourceFolder / payloadName),
          "Extracted Cyrillic payload differs from the source");
  std::atomic_uint64_t done{0};
  FileManifest payload;
  Require(HashArchiveFile(g_testWindow, extracted, extracted / payloadName, 3, payload, done, 6,
                          std::chrono::steady_clock::now()), "Unable to hash the extracted Cyrillic payload");
  Require(payload.path.generic_wstring() == L"\u041c\u043e\u0434\u044b/\u0442\u0435\u0441\u0442.txt" &&
              payload.sha256 == L"bef57ec7f53a6d40beb640a780a639c83bc29ac8a9816f1fc6c5c6dcd93c4721" &&
              payload.chunks == std::vector<std::wstring>{
                  L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                  L"cb8379ac2098aa165029e3938a51da0bcecfc008fd6795f401178647f96c5b34"},
          "Cyrillic payload lost its known filename or full/chunk SHA-256 hashes");
  std::cout << "REPRO: Cyrillic UTF-8 path: " << archiveName << "; payload SHA-256: " << Narrow(payload.sha256) << '\n';
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
  std::unique_ptr<OwnedFixtureTree> fixtureTree;
  try {
    bool keepFixtures = false;
    const std::string usage = "Usage: packer_tests [--keep-fixtures] [--installer-source <folder containing modlist-installer.exe>]";
    for (int i = 1; i < argc; ++i) {
      const std::wstring option = argv[i];
      if (option == L"--keep-fixtures" && !keepFixtures) {
        keepFixtures = true;
      } else if (option == L"--installer-source" && g_installerSource.empty() && i + 1 < argc) {
        g_installerSource = std::filesystem::absolute(argv[++i]);
        Require(std::filesystem::is_regular_file(g_installerSource / "modlist-installer.exe"),
                "Installer source folder must contain modlist-installer.exe");
      } else {
        throw std::runtime_error(usage);
      }
    }
    fixtureTree = std::make_unique<OwnedFixtureTree>(ModuleFolder() / "test-fixtures", keepFixtures);
    g_fixtureRoot = fixtureTree->Root();
    ScopedLocalAppData isolatedCache(g_fixtureRoot / "local-appdata");
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = TestWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"PackerConsoleTestMessages";
    Require(RegisterClassW(&windowClass) != 0, "Unable to register test message window");
    g_testWindow = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
                                  HWND_MESSAGE, nullptr, windowClass.hInstance, nullptr);
    Require(g_testWindow != nullptr, "Unable to create test message window");
    std::cout << "Fixture root: " << Narrow(g_fixtureRoot.wstring()) << std::endl;
    const std::vector<std::pair<const char*, std::function<void()>>> tests{
        {"fixture cleanup owns only this run and supports retention", FixtureCleanupIsOwnedAndRunScoped},
        {"flat preparation", FlatPreparation},
        {"portable installer preparation and refresh", PortableInstallerPreparation},
        {"missing installer rejection", MissingInstallerDoesNotFallBackToRuntimeFiles},
        {"Manifest Only root paths, filtering and known SHA-256 hashes", ManifestOnlyUsesFlatRootAndRealArchiveName},
        {"Manifest Only archive selection and ambiguity", ManifestOnlySelectionAndAmbiguity},
        {"Cyrillic manifest UTF-8 bytes and known hashes", CyrillicManifestUsesUtf8AndKnownHashes},
        {"name-scoped cleanup preserves unrelated outputs", CleanupIsNameScopedAndPreservesDirectories},
        {"missing package cleanup", MissingPackageCleanupIsHarmless},
        {"cleanup preserves manifest directory", CleanupDoesNotDeleteManifestDirectory},
        {"source/release overlap rejection", OverlappingFoldersAreRejectedBeforeWriting},
        {"unsafe archive name rejection", UnsafeArchiveNamesAreRejectedBeforeWriting},
        {"real -spf absolute archive metadata reproduction", RealSevenZipFullPathMetadata},
        {"unsafe path mode and parameter rejection", UnsafePathModesAreRejectedBeforeWriting},
        {"owned strings are not queued without a window", NullTargetsDoNotQueueOwnedStrings},
        {"same-size embedded cache tamper regeneration", EmbeddedCacheRepairsSameSizeTampering},
        {"packer log retains long output and compression defaults", PackerLogRetainsLongOutputAndDefaults},
        {"real compression, integrity test, extraction and manifest", RealCompressionTestAndManifest},
        {"real unsplit ZIP compression and manifest", RealSingleZipCompression},
        {"real Cyrillic packing and parsed manifest roundtrip", RealCyrillicPackingAndManifestRoundTrip}};
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
    g_testWindow = nullptr;
    std::cout << tests.size() - failures << '/' << tests.size() << " tests passed\n";
    if (keepFixtures) {
      std::cout << "Fixtures retained: " << Narrow(g_fixtureRoot.wstring()) << '\n';
    } else {
      fixtureTree->Cleanup();
      std::cout << "Fixtures removed: " << Narrow(g_fixtureRoot.wstring()) << '\n';
    }
    return failures == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    if (g_testWindow != nullptr) {
      DrainMessages();
      DestroyWindow(g_testWindow);
      g_testWindow = nullptr;
    }
    std::cerr << "Test harness failed: " << error.what() << '\n';
    return 1;
  }
}
