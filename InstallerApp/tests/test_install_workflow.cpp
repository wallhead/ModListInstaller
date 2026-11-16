#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace workflow_barrier {
BOOL WINAPI Post(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
}
#define PostMessageW workflow_barrier::Post
#include "../src/ui/WinInstallerApp.cpp"
#undef PostMessageW

#include <iostream>
#include <stdexcept>

namespace {
bool completed = false;
bool installed = false;
bool validationFailed = false;
int lastProgress = -1;
bool reportedHundred = false;
std::filesystem::path stagingToLock;
std::vector<HANDLE> stagingLocks;
bool stagingRetainedWarning = false;
bool stagingFileError = false;
bool addLeftover = false;
bool addedLeftover = false;

void ReleaseStagingLocks() {
  for (const HANDLE lock : stagingLocks) CloseHandle(lock);
  stagingLocks.clear();
}

void RequireWorkflow(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

LRESULT CALLBACK CaptureWorkerMessages(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == kLogMessage || message == kStatusMessage) {
    std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
    if (text && message == kLogMessage) {
      std::cout << Narrow(*text) << '\n';
      stagingRetainedWarning = stagingRetainedWarning ||
          text->starts_with(L"Staging retained:");
      stagingFileError = stagingFileError ||
          text->find(L"Staging still contains files; ownership retained for recovery") != std::wstring::npos;
    }
    return 0;
  }
  if (message == kWorkerFinishedMessage) {
    completed = true;
    installed = wParam != 0;
    return 0;
  }
  if (message == kProgressMessage) {
    lastProgress = static_cast<int>(wParam);
    reportedHundred = reportedHundred || lastProgress == 100;
    return 0;
  }
  if (message == kValidationFailedMessage) {
    validationFailed = true;
    return 0;
  }
  if (message == kInstallFilesFailedMessage) {
    delete reinterpret_cast<ManualInstallRequiredData*>(lParam);
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

void DrainWorkerMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}
}  // namespace

BOOL WINAPI workflow_barrier::Post(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (stagingToLock.empty() || message != kLogMessage || !lParam) {
    return ::PostMessageW(window, message, wParam, lParam);
  }
  const auto& text = *reinterpret_cast<const std::wstring*>(lParam);
  if (stagingLocks.empty() && text == L"Verified asynchronous extraction completed") {
    // Lock existing fixture directories after extraction, before real finalization can move them.
    for (const auto& dir : {stagingToLock / "locked", stagingToLock / "locked" / "nested"}) {
      const HANDLE lock = CreateFileW(dir.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (lock != INVALID_HANDLE_VALUE) stagingLocks.push_back(lock);
    }
  } else if (addLeftover && !addedLeftover && stagingLocks.size() == 2 &&
             text.ends_with(L" (Windows error 32).")) {
    // The top-level move inventory is already fixed when source cleanup logs this warning.
    std::ofstream file(stagingToLock / "late.txt", std::ios::binary);
    file << "Not part of the installed fixture";
    file.close();
    addedLeftover = file.good();
  }
  return ::PostMessageW(window, message, wParam, lParam);
}

int wmain(int argc, wchar_t** argv) {
  HWND capture = nullptr;
  try {
    RequireWorkflow(argc == 4 || argc == 5, "Usage: installer_workflow_tests <new-output-parent> <direct|staged> <success|failure> [--root-level|--lock-staged-tree]");
    const std::filesystem::path output(argv[1]);
    const std::wstring route(argv[2]);
    const std::wstring expectation(argv[3]);
    const bool rootLevel = argc == 5 && std::wstring_view(argv[4]) == L"--root-level";
    const bool lockStagedTree = argc == 5 && std::wstring_view(argv[4]) == L"--lock-staged-tree";
    RequireWorkflow(argc == 4 || rootLevel || lockStagedTree, "Unknown workflow test layout");
    RequireWorkflow(output.is_absolute() && !std::filesystem::exists(output), "Use a new absolute output parent");
    RequireWorkflow(route == L"direct" || route == L"staged", "Unknown workflow test route");
    RequireWorkflow(expectation == L"success" || expectation == L"failure", "Unknown workflow test expectation");
    RequireWorkflow(!modlist::InstallationOverlapsPackage(output, ExeFolder()), "Workflow output overlaps its input package");
    const auto manifest = modlist::ManifestLoader{}.LoadFromFile(ManifestPath());
    RequireWorkflow(manifest.ok(), manifest.error().c_str());
    const auto package = modlist::DiscoverPackageNear(ArchiveFolder(), false);
    RequireWorkflow(package.ok(), package.error().c_str());
    const auto final = (rootLevel ? output : output / "installed") / modlist::PathFromUtf8(manifest.value().archiveName);
    const auto staging = output / "Unpacking";
    if (lockStagedTree) {
      RequireWorkflow(route == L"staged" && modlist::PipelinedSevenZipExtractor::CanUse(manifest.value()),
          "Locked staging test requires a staged run with chunk verification");
      stagingToLock = staging;
      stagingLocks.reserve(2);
      addLeftover = expectation == L"failure";
    }
    std::filesystem::create_directories(output);

    modlist::PipelinedSevenZipExtractor inspector;
    const auto library = inspector.LocateLibrary(ExeFolder());
    RequireWorkflow(library.ok(), library.error().c_str());
    modlist::PipelinedExtractionConfig config;
    config.sevenZipLibrary = library.value();
    config.archiveFolder = ArchiveFolder();
    config.manifest = &manifest.value();
    const auto plan = inspector.PlanExtraction(config, final, staging);
    if (expectation == L"success") RequireWorkflow(plan.ok(), plan.error().c_str());
    if (route == L"direct" && expectation == L"success")
      RequireWorkflow(plan.value().direct || plan.value().entryCount == 0, "Direct workflow fixture exceeds the direct path budget");

    g_instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.hInstance = g_instance;
    windowClass.lpfnWndProc = CaptureWorkerMessages;
    windowClass.lpszClassName = L"ModlistWorkflowMessageCapture";
    RequireWorkflow(RegisterClassW(&windowClass) != 0, "Cannot register worker message capture");
    capture = CreateWindowExW(0, windowClass.lpszClassName, L"", 0, 0, 0, 0, 0,
                              HWND_MESSAGE, nullptr, g_instance, nullptr);
    RequireWorkflow(capture != nullptr, "Cannot create message-only worker capture");
    g_stopRequested = false;
    // Run the production install worker; no desktop window or Documents changes.
    RunInstallWorker(capture, package.value(), route == L"direct" ? final : staging, final);
    DrainWorkerMessages();
    if (lockStagedTree) {
      RequireWorkflow(stagingLocks.size() == 2, "The staged fixture directories were not locked");
      RequireWorkflow(!addLeftover || addedLeftover, "The late staging file was not created");
    }
    RequireWorkflow(completed && !g_workerRunning, "Worker did not publish completion");
    RequireWorkflow(installed == (expectation == L"success"), "Worker installation result differs from expectation");
    if (installed) {
      RequireWorkflow(lastProgress == 100 && !validationFailed, "Successful worker did not publish final progress");
      RequireWorkflow(std::filesystem::is_directory(final), "Successful worker did not create the final folder");
      if (route == L"staged" && !lockStagedTree) {
        RequireWorkflow(!std::filesystem::exists(staging), "Successful staged install left its unpacking folder");
      }
    } else {
      RequireWorkflow(!reportedHundred, "Failed worker published completed installation progress");
    }
    if (lockStagedTree) {
      RequireWorkflow(stagingRetainedWarning == installed, "Staging warning does not match installation outcome");
      RequireWorkflow(!addLeftover || stagingFileError, "Worker failed for a reason other than the late staging file");
      RequireWorkflow(std::filesystem::is_empty(staging / "locked" / "nested"),
          "Locked staging directory still contains original payload");
      RequireWorkflow(!addLeftover || std::filesystem::is_regular_file(staging / "late.txt"),
          "Refusal did not preserve the late staging file");
      ReleaseStagingLocks();
      const auto digest = modlist::Sha256::FileHexDigest(ManifestPath());
      RequireWorkflow(digest.ok(), "Cannot identify the fixture package");
      const auto ticket = modlist::PrepareFolderCleanup(
          staging, ExeFolder(), modlist::FolderCleanupKind::OwnedStaging, digest.value());
      RequireWorkflow(ticket.ok(), "Retained staging did not keep recoverable ownership");
      std::cout << "staging_retained=1\n";
    }
    std::cout << "worker_success=" << installed << "\nvalidation_failed=" << validationFailed
              << "\nfinal_folder=" << modlist::PathToUtf8(final) << '\n';
    DestroyWindow(capture);
    return 0;
  } catch (const std::exception& error) {
    ReleaseStagingLocks();
    DrainWorkerMessages();
    if (capture) DestroyWindow(capture);
    std::cerr << "FAIL worker workflow: " << error.what() << '\n';
    return 1;
  }
}
