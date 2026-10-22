#include "core/ShopSaves.hpp"
#include "core/Homebrew.hpp"
#include "core/I18n.hpp"
#include "core/SaveBackups.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <set>
#include <filesystem>
namespace foil::shopsaves {
namespace {
std::string url(json_object *object, const char *key, const std::string &base) {
  const auto value = stringField(object, key);
  if (value.rfind("https://", 0) == 0 || value.rfind("http://", 0) == 0) return value;
  if (value.rfind("/", 0) == 0 && value.rfind("//", 0) != 0 && !base.empty()) return base + value;
  return "";
}
bool hexId(const std::string &id) {
  return id.size() == 16 && id.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos;
}
} // namespace
std::vector<ShopSave> parse(const std::string &json, const std::string &base) {
  std::vector<ShopSave> out;
  Error error;
  auto root = parseJson(json, error);
  auto *list = root ? field(root.get(), "saves") : nullptr;
  if (!json_object_is_type(list, json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(list) && out.size() < 5000; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    ShopSave s;
    s.id = stringField(object, "id");
    s.titleId = stringField(object, "titleId");
    std::transform(s.titleId.begin(), s.titleId.end(), s.titleId.begin(), ::toupper);
    s.url = url(object, "url", base);
    // No Title ID: an app's save, for its own switch/<app> folder only.
    if (!field(object, "titleId")) {
      s.homebrewApp = stringField(object, "homebrew");
      s.installRoot = stringField(object, "installRoot");
      while (!s.installRoot.empty() && s.installRoot.back() == '/') s.installRoot.pop_back();
      if (!homebrew::appFolderAllowed(s.homebrewApp) || s.installRoot != "switch/" + s.homebrewApp) continue;
    } else if (!hexId(s.titleId)) {
      continue;
    }
    if (s.id.empty() || s.id.size() > 64 || s.url.empty() ||
        s.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
      continue;
    s.name = stringField(object, "name", s.id);
    s.description = stringField(object, "description");
    // The last line credits the source ("출처: …", "Source: …"); the list
    // has no room for it, so it is left off.
    if (const auto nl = s.description.rfind('\n'); nl != std::string::npos) {
      const auto last = s.description.substr(nl + 1);
      if (last.rfind("출처:", 0) == 0 || last.rfind("Source:", 0) == 0) // i18n: as is
        s.description.erase(s.description.find_last_not_of(" \n", nl) + 1);
    }
    s.saveType = stringField(object, "saveType");
    if (s.saveType != "device" && s.saveType != "account") s.saveType.clear();
    s.gameVersion = stringField(object, "gameVersion");
    s.hash = stringField(object, "hash");
    s.size = uint64_t(std::max<int64_t>(integerField(object, "size"), 0));
    s.files = int(std::clamp<int64_t>(integerField(object, "files"), 0, 1 << 20));
    s.updatedAt = integerField(object, "updatedAt");
    auto *verified = field(object, "verified");
    s.verified = json_object_is_type(verified, json_type_boolean) && json_object_get_boolean(verified);
    s.votes = votes::fromItem(object);
    if (auto *game = field(object, "game"); json_object_is_type(game, json_type_object)) {
      s.gameName = stringField(game, "name");
      s.gameIcon = url(game, "icon", base);
      s.gameNames = languageTexts(game, "names");
    }
    out.push_back(std::move(s));
  }
  return out;
}
bool pathAllowed(const std::string &path) {
  if (path.empty() || path[0] == '/' || path.find_first_of("\\:") != std::string::npos) return false;
  size_t start = 0;
  for (;;) {
    const auto slash = path.find('/', start);
    const auto part = path.substr(start, slash - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (slash == std::string::npos) return true;
    start = slash + 1;
  }
}
bool homebrewPathAllowed(const ShopSave &save, const std::string &path) {
  return !save.homebrewApp.empty() && save.installRoot == "switch/" + save.homebrewApp && pathAllowed(path) &&
         homebrew::insideApp(save.homebrewApp, save.installRoot + "/" + path);
}
bool placeFiles(const std::string &from, const std::string &to, const std::string &keep, std::string &error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  std::vector<std::string> files;
  for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::recursive_directory_iterator();
       it.increment(ec))
    if (it->is_regular_file(ec)) files.push_back(it->path().string().substr(from.size() + 1));
  if (ec) {
    error = tr("Could not read the downloaded save files.");
    return false;
  }
  const auto move = [&](const std::string &a, const std::string &b, const std::string &rel) {
    makeDirectories(fs::path(b).parent_path().string());
    fs::rename(a, b, ec);
    if (ec) error = trf("Could not write the file: {0}", {rel});
    return !ec;
  };
  for (const auto &rel : files) {
    if (fs::exists(to + "/" + rel, ec) && !move(to + "/" + rel, keep + "/" + rel, rel)) return false;
    if (!move(from + "/" + rel, to + "/" + rel, rel)) return false;
  }
  fs::remove_all(from, ec);
  return true;
}
Error targetMismatch(const ShopSave &save, bool deviceTarget) {
  if (save.saveType.empty()) return {};
  return saves::kindMismatch(save.saveType == "device", deviceTarget);
}
std::string gameName(const ShopSave &save, const std::string &languageCode) {
  if (const auto *own = textIn(save.gameNames, languageCode)) return *own;
  if (!save.gameName.empty()) return save.gameName;
  if (!save.gameNames.empty()) return save.gameNames.front().text;
  return save.titleId;
}
std::vector<ShopSave> mergeServers(std::vector<std::pair<std::string, std::vector<ShopSave>>> lists) {
  std::vector<ShopSave> merged;
  std::set<std::string> hashes;
  for (auto &[server, saves] : lists)
    for (auto &save : saves) {
      save.server = server;
      if (save.hash.empty() || hashes.insert(save.hash).second) merged.push_back(std::move(save));
    }
  return merged;
}
} // namespace foil::shopsaves
