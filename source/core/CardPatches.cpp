#include "core/Json.hpp"
#include "core/CardPatches.hpp"
#include "core/TitleGuard.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
namespace foil {
namespace fs = std::filesystem;
namespace {
std::string upper(std::string text) {
  for (auto &c : text) c = char(std::toupper(static_cast<unsigned char>(c)));
  return text;
}
// A game's (or its update's or DLC's) Title ID: 0100… above the system's.
bool gameTitle(const std::string &name) {
  if (name.size() != 16 || name.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return false;
  const auto id = std::stoull(name, nullptr, 16);
  return (id >> 56) == 0x01 && installableTitle(id);
}
// Every regular file below `dir`, by its path from the card's root.
void filesBelow(const fs::path &root, const fs::path &dir, std::vector<std::pair<std::string, uint64_t>> &out) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return;
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
       it.increment(ec)) {
    // A Mac's "._" files and the like are not part of any patch.
    if (systemJunk(it->path().filename().string()) || !it->is_regular_file(ec)) continue;
    std::error_code sizeError;
    const auto size = it->file_size(sizeError);
    // The path below the card's root by its text: fs::relative canonicalizes,
    // which fails on the console's "sdmc:" root and took the size with it.
    auto path = it->path().generic_string();
    const auto top = root.generic_string();
    if (path.compare(0, top.size(), top) == 0) path.erase(0, top.size());
    while (!path.empty() && path.front() == '/') path.erase(0, 1);
    out.emplace_back(path, sizeError ? 0 : uint64_t(size));
  }
}
} // namespace
std::vector<CardPatch> scanCardPatches(const std::string &sdRoot, const std::map<std::string, PatchRecord> &records) {
  std::set<std::string> known; // upper case: the card does not care
  for (const auto &[id, record] : records) {
    for (const auto &path : record.files) known.insert(upper(path));
    for (const auto &path : record.cheats) known.insert(upper(path));
  }
  const fs::path root(sdRoot);
  std::map<std::string, CardPatch> found;
  const auto add = [&](const std::string &titleId, const char *kind, const fs::path &dir) {
    std::vector<std::pair<std::string, uint64_t>> files;
    filesBelow(root, dir, files);
    auto base = baseTitleId(titleId);
    for (const auto &[path, size] : files) {
      if (known.count(upper(path))) continue;
      if (std::string(kind) == "cheats" && !isCheatPath(path)) continue;
      auto &patch = found[base];
      patch.titleId = base;
      if (std::find(patch.kinds.begin(), patch.kinds.end(), kind) == patch.kinds.end()) patch.kinds.push_back(kind);
      ++patch.files;
      patch.bytes += size;
    }
  };
  std::error_code ec;
  for (fs::directory_iterator it(root / "atmosphere" / "contents", ec), end; !ec && it != end; it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (!gameTitle(name) || !it->is_directory(ec)) continue;
    for (const char *kind : {"romfs", "exefs", "cheats"}) add(upper(name), kind, it->path() / kind);
  }
  for (fs::directory_iterator it(root / "SaltySD" / "plugins" / "FPSLocker" / "patches", ec), end; !ec && it != end;
       it.increment(ec)) {
    const auto name = it->path().filename().string();
    if (gameTitle(name) && it->is_directory(ec)) add(upper(name), "fpslocker", it->path());
  }
  std::vector<CardPatch> out;
  static const std::vector<std::string> order{"romfs", "exefs", "cheats", "fpslocker"};
  for (auto &[id, patch] : found) {
    std::sort(patch.kinds.begin(), patch.kinds.end(), [](const std::string &a, const std::string &b) {
      return std::find(order.begin(), order.end(), a) < std::find(order.begin(), order.end(), b);
    });
    out.push_back(std::move(patch));
  }
  return out;
}
const UserPatch *guessCardPatch(const CardPatch &card, const std::vector<UserPatch> &server) {
  const UserPatch *only = nullptr, *sameFiles = nullptr;
  int games = 0, files = 0;
  for (const auto &patch : server) {
    if (!patch.homebrewApp.empty() || patch.titleId.size() != card.titleId.size() ||
        !std::equal(patch.titleId.begin(), patch.titleId.end(), card.titleId.begin(),
                    [](char a, char b) { return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b)); }))
      continue;
    only = &patch;
    ++games;
    if (patch.files > 0 && size_t(patch.files) == card.files) {
      sameFiles = &patch;
      ++files;
    }
  }
  if (files == 1) return sameFiles;
  return games == 1 ? only : nullptr;
}
} // namespace foil
