#include "../src/ui/WinInstallerApp.cpp"

#include <iostream>
#include <stdexcept>

namespace {
bool completed = false;
bool installed = false;
bool validationFailed = false;
int lastProgress = -1;

void RequireWorkflow(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

LRESULT CALLBACK CaptureWorkerMessages(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == kLogMessage || message == kStatusMessage) {
    std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
    if (text && message == kLogMessage) std::cout << Narrow(*text) << '\n';
    return 0;
  }
  if (message == kWorkerFinishedMessage) {
    completed = true;
    installed = wParam != 0;
    return 0;
  }
  if (message == kProgressMessage) {
    lastProgress = static_cast<int>(wParam);
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

int wmain(int argc, wchar_t** argv) {
  HWND capture = nullptr;
  try {
    RequireWorkflow(argc == 4 || argc == 5, "Usage: installer_workflow_tests <new-output-parent> <direct|staged> <success|failure> [--root-level]");
    const std::filesystem::path output(argv[1]);
    const std::wstring route(argv[2]);
    const std::wstring expectation(argv[3]);
    const bool rootLevel = argc == 5 && std::wstring_view(argv[4]) == L"--root-level";
    RequireWorkflow(argc == 4 || rootLevel, "Unknown workflow test layout");
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
    RequireWorkflow(completed && !g_workerRunning, "Worker did not publish completion");
    RequireWorkflow(installed == (expectation == L"success"), "Worker installation result differs from expectation");
    if (installed) {
      RequireWorkflow(lastProgress == 100 && !validationFailed, "Successful worker did not publish final progress");
      RequireWorkflow(std::filesystem::is_directory(final), "Successful worker did not create the final folder");
      if (route == L"staged") RequireWorkflow(!std::filesystem::exists(staging), "Successful staged install left its unpacking folder");
    } else {
      RequireWorkflow(lastProgress != 100, "Failed worker published completed installation progress");
    }
    std::cout << "worker_success=" << installed << "\nvalidation_failed=" << validationFailed
              << "\nfinal_folder=" << modlist::PathToUtf8(final) << '\n';
    DestroyWindow(capture);
    return 0;
  } catch (const std::exception& error) {
    DrainWorkerMessages();
    if (capture) DestroyWindow(capture);
    std::cerr << "FAIL worker workflow: " << error.what() << '\n';
    return 1;
  }
}
