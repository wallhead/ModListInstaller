#include "patch/PatchDeletePaths.h"

#include "common/PathText.h"

#include <algorithm>
#include <utility>

namespace modlist::patch {
namespace {
namespace fs = std::filesystem;
using DeletePaths = Result<std::vector<fs::path>>;

bool IsSafeComponent(std::wstring_view name) {
  if (name.empty() || name.size() > 255 || name.back() == L'.' || name.back() == L' ' ||
      name.find_first_of(L"<>:\"/\\|?*") != std::wstring_view::npos ||
      std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 0x20; })) {
    return false;
  }
  auto stem = name.substr(0, name.find(L'.'));
  while (stem.ends_with(L' ')) stem.remove_suffix(1);
  for (const auto* device : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"}) {
    if (PathTextEqual(stem, device)) return false;
  }
  return !(stem.size() == 4 &&
           (PathTextEqual(stem.substr(0, 3), L"COM") || PathTextEqual(stem.substr(0, 3), L"LPT")) &&
           std::wstring_view(L"123456789\u00b9\u00b2\u00b3").find(stem[3]) != std::wstring_view::npos);
}

class DeletePathCollector {
public:
  Result<void> Add(std::string_view text) {
    if (text.empty() || text.find_first_of("\r\n\0", 0, 3) != std::string_view::npos ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                            nullptr, 0) == 0) {
      return Result<void>::Error("Expected a single UTF-8 path relative to the modlist");
    }
    const auto path = PathFromUtf8(text).make_preferred();
    if (!IsPatchPathAllowed(path)) return Result<void>::Error("Unsafe path or reserved patch workspace");
    // Rebuild from validated components so separator spellings do not bypass ancestor checks.
    fs::path canonical;
    for (const auto& part : path) canonical /= part;
    auto [found, inserted] = unique_.insert(canonical);
    if (!inserted && found->native() != canonical.native()) {
      return Result<void>::Error("Ambiguous case-only duplicate");
    }
    if (inserted) entries_.push_back(std::move(canonical));
    return Result<void>::Ok();
  }

  DeletePaths Finish() {
    const auto distinct = ValidateDistinctPaths(unique_);
    return distinct.ok() ? DeletePaths::Ok(std::move(entries_)) : DeletePaths::Error(distinct.error());
  }

private:
  std::vector<fs::path> entries_;
  std::set<fs::path, PathLess> unique_;
};
}  // namespace

bool PathLess::operator()(const fs::path& a, const fs::path& b) const {
  return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
}

bool IsPatchPathAllowed(const fs::path& path) {
  if (path.empty() || path.has_root_path() || path.native().size() >= 32760 ||
      PathTextEqual(path.begin()->native(), kWorkFolder)) {
    return false;
  }
  size_t depth = 0;
  for (const auto& part : path) {
    if (++depth > 256 || !IsSafeComponent(part.native())) return false;
  }
  return true;
}

Result<void> ValidateDistinctPaths(const std::set<fs::path, PathLess>& paths) {
  for (const auto& path : paths) {
    for (auto parent = path.parent_path(); !parent.empty(); parent = parent.parent_path()) {
      if (paths.contains(parent)) {
        return Result<void>::Error("Delete list has overlapping paths: " + PathToUtf8(parent) +
                                  " and " + PathToUtf8(path));
      }
    }
  }
  return Result<void>::Ok();
}

DeletePaths ParsePatchDeleteList(std::string_view text) {
  if (text.size() > kMaxDeleteListBytes) return DeletePaths::Error("Delete list is too large");
  DeletePathCollector paths;
  size_t lineNumber = 0;
  while (!text.empty()) {
    ++lineNumber;
    const auto split = text.find('\n');
    auto line = text.substr(0, split);
    text.remove_prefix(split == std::string_view::npos ? text.size() : split + 1);
    if (line.ends_with('\r')) line.remove_suffix(1);
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos) continue;
    const auto padded = line.substr(first, line.find_last_not_of(" \t") - first + 1);
    const auto fail = [&](const std::string& reason) {
      return DeletePaths::Error("Delete list line " + std::to_string(lineNumber) + ": " + reason);
    };
    if (padded.front() == '"' || padded.back() == '"') {
      line = padded;  // Ignore only padding outside quotes.
      if (line.size() < 2 || line.front() != '"' || line.back() != '"') return fail("Unmatched quotes");
      line.remove_prefix(1);
      line.remove_suffix(1);
    }
    if (line.starts_with('\\')) line.remove_prefix(1);
    const auto added = paths.Add(line);
    if (!added.ok()) return fail(added.error());
  }
  return paths.Finish();
}

DeletePaths ParseManifestDeletePaths(std::span<const std::string> entries) {
  DeletePathCollector paths;
  size_t bytes = 0;
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& entry = entries[i];
    const size_t separator = i == 0 ? 0 : 1;
    if (separator > kMaxDeleteListBytes - bytes ||
        entry.size() > kMaxDeleteListBytes - bytes - separator) {
      return DeletePaths::Error("Manifest delete_paths is too large");
    }
    bytes += entry.size() + separator;
    const auto added = paths.Add(entry);
    if (!added.ok()) {
      return DeletePaths::Error("Manifest delete_paths entry " + std::to_string(i + 1) + ": " + added.error());
    }
  }
  auto result = paths.Finish();
  if (!result.ok()) return DeletePaths::Error("Manifest delete_paths: " + result.error());
  return result;
}

}  // namespace modlist::patch
