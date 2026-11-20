#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace registry_fixture {
std::wstring path32, path64;
std::vector<DWORD> reads;

LSTATUS WINAPI Read(HKEY key, LPCWSTR subkey, LPCWSTR value, DWORD flags,
    LPDWORD, PVOID data, LPDWORD bytes) {
  const DWORD view = flags & (RRF_SUBKEY_WOW6432KEY | RRF_SUBKEY_WOW6464KEY);
  if (key != HKEY_LOCAL_MACHINE || std::wstring_view(subkey) != L"SOFTWARE\\Bethesda Softworks\\Skyrim Special Edition" ||
      std::wstring_view(value) != L"Installed Path") return ERROR_INVALID_PARAMETER;
  reads.push_back(view);
  const auto& path = view == RRF_SUBKEY_WOW6432KEY ? path32 : path64;
  if (path.empty()) return ERROR_FILE_NOT_FOUND;
  const auto size = static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t));
  if (*bytes < size) return ERROR_MORE_DATA;
  std::memcpy(data, path.c_str(), size);
  *bytes = size;
  return ERROR_SUCCESS;
}
}  // namespace registry_fixture

#define MODLIST_PATCH_INSTALLER
#define RegGetValueW registry_fixture::Read
#include "../src/ui/WinInstallerApp.cpp"
#undef RegGetValueW

namespace {
namespace fs = std::filesystem;

void Need(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

struct RegistryFixtures {
  fs::path home, package, actual, other;
  RegistryFixtures() {
    home = fs::temp_directory_path() / (L"mi-patch-registry-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Need(fs::create_directory(home), "Registry fixture directory already exists");
    package = home / L"Package";
    fs::create_directory(package);
    actual = home / L"\u041f\u0440\u0438\u043c\u0435\u0440 TRUEAE";
    other = home / L"Different installation name";
    Create(actual);
    Create(actual / L"Stock Game");
    Create(other);
  }
  ~RegistryFixtures() {
    std::error_code ec;
    fs::remove_all(home, ec); // Only this exclusively created fixture directory.
  }
  static void Create(const fs::path& root) {
    fs::create_directories(root / L"Stock Game" / L"Data");
    fs::create_directory(root / L"mods");
    std::ofstream(root / L"ModOrganizer.exe") << "dummy file";
  }
};
}  // namespace

int main() {
  try {
    RegistryFixtures f;
    using namespace registry_fixture;
    path32 = (f.actual / L"Stock Game").generic_wstring() + L"/";
    path64 = (f.other / L"Stock Game").native();
    auto preferred = FindRegisteredPatchFolder(f.package);
    Need(preferred && preferred->Root() == f.actual, "Registry must use exactly the parent of Stock Game");
    Need(reads == std::vector<DWORD>{RRF_SUBKEY_WOW6432KEY}, "Registry did not prefer the 32-bit view");
    preferred.reset();
    reads.clear();
    path32 = (f.home / L"ordinary Steam game directory").native();
    auto fallback = FindRegisteredPatchFolder(f.package);
    Need(fallback && fallback->Root() == f.other, "Valid 64-bit registry fallback ignored");
    Need(reads == std::vector<DWORD>{RRF_SUBKEY_WOW6432KEY, RRF_SUBKEY_WOW6464KEY}, "Wrong registry view order");
    path64.clear();
    Need(!FindRegisteredPatchFolder(f.package), "Unusable lookup did not fall back to manual selection");
    std::cout << "PASS registry root, 32-bit preference and manual fallback signal\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Patch registry test failed: " << error.what() << '\n';
    return 1;
  }
}
