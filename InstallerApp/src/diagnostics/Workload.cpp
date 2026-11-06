#include "diagnostics/Diagnostics.h"
#include "common/PathText.h"

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace modlist::diagnostics {

std::filesystem::path TestParentForDrive(const std::filesystem::path& drive) {
  auto root=drive.lexically_normal().make_preferred();
  const auto name=root.root_name().wstring();
  if(!root.is_absolute() || root!=root.root_path() || name.size()!=2 || name[1]!=L':' ||
     !((name[0]>=L'A' && name[0]<=L'Z') || (name[0]>=L'a' && name[0]<=L'z')))
    throw std::invalid_argument("Choose a local disk root");
  return root/"ModlistDiagnostics";
}

std::vector<FileSpec> BuildWorkload(uint64_t totalBytes, bool smoke) {
  if (totalBytes < 1024 * 1024) throw std::invalid_argument("Dataset is too small");
  constexpr uint64_t gib = 1024ull * 1024 * 1024;
  const uint64_t count = smoke ? 512 : 200000;
  const uint64_t smallBytes = totalBytes / 5;
  std::vector<FileSpec> files;
  files.reserve(static_cast<size_t>(count) + 450);
  uint64_t largeBytes = totalBytes - smallBytes - 4096;
  for (uint64_t index = 0; largeBytes != 0; ++index) {
    const auto bytes = std::min(largeBytes, 2 * gib);
    files.push_back({"bulk/large-" + std::to_string(index) + ".bin", bytes});
    largeBytes -= bytes;
  }
  files.push_back({std::filesystem::path(L"\u041c\u043e\u0434\u044b") / L"\u0442\u0435\u0441\u0442.bin", 4096});
  for (uint64_t index = 0; index < count; ++index) {
    std::ostringstream path;
    path << "small/group-" << std::setfill('0') << std::setw(4) << index / 100
         << "/file-" << std::setw(6) << index << ".bin";
    files.push_back({path.str(), smallBytes / count + (index < smallBytes % count ? 1 : 0), true});
  }
  files.push_back({"empty.txt", 0});
  for (int index = 0; index < (smoke ? 4 : 400); ++index) {
    files.push_back({"empty/folder-" + std::to_string(index), 0, false, true});
  }
  return files;
}

std::string ValidateConfig(const Config& config) {
  if (config.sourceParent.empty() || config.destinationParent.empty()) return "Choose source and destination folders";
  if (!config.sourceParent.is_absolute() || !config.destinationParent.is_absolute()) return "Folders must have absolute paths";
  const auto normalized = [](const auto& path) {
    auto text = path.lexically_normal().wstring();
    while (text.size() > path.root_path().wstring().size() &&
           (text.back() == L'\\' || text.back() == L'/')) text.pop_back();
    return text;
  };
  if (PathTextEqual(normalized(config.sourceParent), normalized(config.destinationParent))) return "Source and destination must be different folders";
  if (config.sizeGiB != 10 && config.sizeGiB != 20 && config.sizeGiB != 30) return "Dataset size must be 10, 20, or 30 GiB";
  if (config.threads < 1 || config.threads > 64) return "Decoder threads must be between 1 and 64";
  return {};
}
}
