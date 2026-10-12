#include "core/Retro.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <cctype>
#include <set>
namespace foil::retro {
namespace {
std::string url(json_object *object, const char *key, const std::string &base) {
  return urlField(object, key, base);
}
uint64_t positive(json_object *object, const char *key) {
  const auto value = integerField(object, key, 0);
  return value > 0 ? uint64_t(value) : 0;
}
std::vector<std::string> parts(const std::string &path) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    const auto slash = path.find('/', start);
    out.push_back(path.substr(start, slash - start));
    if (slash == std::string::npos) return out;
    start = slash + 1;
  }
}
// Relative, forward slashes, no empty, "." or ".." parts, no drive letters.
bool plainPath(const std::string &path) {
  if (path.empty() || path[0] == '/' || path.find_first_of("\\:") != std::string::npos) return false;
  for (const auto &part : parts(path))
    if (part.empty() || part == "." || part == "..") return false;
  return true;
}
} // namespace

namespace {
// Empty id (or one with odd characters) means the entry is skipped.
System parseSystem(json_object *object, const std::string &base) {
  System s;
  s.id = stringField(object, "id");
  if (s.id.empty() || s.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos) return {};
  s.name = stringField(object, "name", s.id);
  s.defaultCore = stringField(object, "defaultCore");
  s.coreName = stringField(object, "coreName");
  s.iconUrl = url(object, "iconUrl", base);
  s.romCount = int(std::min<int64_t>(std::max<int64_t>(integerField(object, "romCount"), 0), 1 << 30));
  if (auto *names = field(object, "names"); json_object_is_type(names, json_type_object)) {
    static const char *codes[] = {"ko", "en", "ja", "zh"};
    for (int k = 0; k < 4; ++k)
      if (const auto name = stringField(names, codes[k]); !name.empty()) {
        s.names.resize(4);
        s.names[k] = name;
      }
  }
  return s;
}
// Empty id or bundle URL means the entry is skipped.
Game parseGame(json_object *object, const std::string &base) {
  Game g;
  g.id = stringField(object, "id");
  g.name = stringField(object, "name");
  g.file = stringField(object, "file");
  g.system = stringField(object, "system");
  g.core = stringField(object, "core");
  g.original = stringField(object, "original");
  g.variant = stringField(object, "variant");
  g.region = stringField(object, "region");
  static const std::set<std::string> Regions{"kr", "jp", "us", "eu", "asia", "br", "world"};
  if (!Regions.count(g.region)) g.region.clear();
  if (auto *edition = field(object, "edition"); json_object_is_type(edition, json_type_object)) {
    g.edition.korean = json_object_get_boolean(field(edition, "korean"));
    g.edition.source = stringField(edition, "source");
    if (g.edition.source != "official" && g.edition.source != "patch")
      g.edition.source.clear();
    g.edition.from = stringField(edition, "from");
    if (!Regions.count(g.edition.from)) g.edition.from.clear();
    g.edition.version = stringField(edition, "version");
    if (g.edition.version.size() > 24) g.edition.version.clear();
  }
  g.size = positive(object, "size");
  g.addedAt = integerField(object, "addedAt");
  g.rating = int(std::clamp<int64_t>(integerField(object, "rating"), 0, 100));
  g.ratingCount = int(std::clamp<int64_t>(integerField(object, "ratingCount"), 0, 1 << 30));
  g.iconUrl = url(object, "iconUrl", base);
  g.bundleUrl = url(object, "bundleUrl", base);
  g.forwarderUrl = url(object, "forwarderUrl", base);
  g.forwarderTitleId = stringField(object, "forwarderTitleId");
  if (g.forwarderTitleId.size() != 16 ||
      g.forwarderTitleId.find_first_not_of("0123456789ABCDEFabcdef") != std::string::npos)
    g.forwarderUrl.clear(), g.forwarderTitleId.clear();
  g.forwarderSize = positive(object, "forwarderSize");
  if (auto *install = field(object, "install"); json_object_is_type(install, json_type_object)) {
    g.romPath = stringField(install, "rom");
    if (!bundlePathAllowed(g.romPath)) g.romPath.clear();
    g.bezelPath = stringField(install, "bezelPng");
    if (!bundlePathAllowed(g.bezelPath)) g.bezelPath.clear();
  }
  if (auto *bezel = field(object, "bezel"); json_object_is_type(bezel, json_type_object))
    g.bezel = json_object_get_boolean(field(bezel, "hasBezel"));
  if (g.bundleUrl.empty()) g.id.clear();
  if (g.name.empty()) g.name = g.file;
  return g;
}
std::vector<Game> parseGameList(json_object *list, const std::string &base) {
  std::vector<Game> out;
  if (!json_object_is_type(list, json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(list) && out.size() < 200; ++i)
    if (auto g = parseGame(json_object_array_get_idx(list, i), base); !g.id.empty()) out.push_back(std::move(g));
  return out;
}
} // namespace

std::string filterQuery(const Filter &f) {
  static const char *hex = "0123456789ABCDEF";
  std::string q;
  for (const unsigned char c : f.query) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') q += char(c);
    else {
      q += '%';
      q += hex[c >> 4];
      q += hex[c & 15];
    }
  }
  std::string out = "&sort=" + (f.sort.empty() ? std::string("rating") : f.sort);
  if (!q.empty()) out += "&q=" + q;
  if (f.minRating > 0) out += "&minRating=" + std::to_string(std::min(f.minRating, 100));
  if (f.bezel) out += "&bezel=1";
  if (f.minSize) out += "&minSize=" + std::to_string(f.minSize);
  if (f.maxSize) out += "&maxSize=" + std::to_string(f.maxSize);
  return out;
}
std::vector<System> parseSystems(const std::string &json, const std::string &base) {
  std::vector<System> out;
  Error error;
  auto root = parseJson(json, error);
  auto *list = root ? field(root.get(), "systems") : nullptr;
  if (!json_object_is_type(list, json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(list) && out.size() < 200; ++i)
    if (auto s = parseSystem(json_object_array_get_idx(list, i), base); !s.id.empty()) out.push_back(std::move(s));
  return out;
}
Page parseGames(const std::string &json, const std::string &base) {
  Page page;
  Error error;
  auto root = parseJson(json, error);
  if (!root) return page;
  page.total = int(std::max<int64_t>(integerField(root.get(), "total"), 0));
  page.page = int(std::max<int64_t>(integerField(root.get(), "page", 1), 1));
  page.pageSize = int(std::clamp<int64_t>(integerField(root.get(), "pageSize", 50), 1, 200));
  page.games = parseGameList(field(root.get(), "games"), base);
  return page;
}
std::vector<Shelf> parseHome(const std::string &json, const std::string &base) {
  std::vector<Shelf> out;
  Error error;
  auto root = parseJson(json, error);
  auto *list = root ? field(root.get(), "systems") : nullptr;
  if (!json_object_is_type(list, json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(list) && out.size() < 200; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    Shelf shelf{parseSystem(object, base), parseGameList(field(object, "games"), base)};
    std::set<std::string> seen;
    shelf.games.erase(std::remove_if(shelf.games.begin(), shelf.games.end(),
                                     [&](const Game &g) { return !g.original.empty() && !seen.insert(g.original).second; }),
                      shelf.games.end());
    if (shelf.system.id.empty() || shelf.games.empty()) continue;
    out.push_back(std::move(shelf));
  }
  return out;
}
Pack parsePack(const std::string &json, const std::string &base) {
  Pack pack;
  Error error;
  auto root = parseJson(json, error);
  if (!root) return pack;
  pack.version = stringField(root.get(), "version");
  pack.bundleUrl = url(root.get(), "bundleUrl", base);
  pack.bundleSize = positive(root.get(), "bundleSize");
  auto *list = field(root.get(), "files");
  if (!json_object_is_type(list, json_type_array)) return pack;
  for (size_t i = 0; i < json_object_array_length(list) && pack.files.size() < 100000; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    PackFile f;
    f.path = stringField(object, "path");
    f.sha256 = stringField(object, "sha256");
    f.size = positive(object, "size");
    f.url = url(object, "url", base);
    f.once = json_object_get_boolean(field(object, "once"));
    if (!packPathAllowed(f.path) || f.url.empty()) continue;
    pack.files.push_back(std::move(f));
  }
  return pack;
}
bool packPathAllowed(const std::string &path) {
  if (!plainPath(path)) return false;
  return path == RetroArchApp || (path.rfind("retroarch/", 0) == 0 && path.size() > 10);
}
bool bundlePathAllowed(const std::string &path) {
  if (!plainPath(path)) return false;
  const auto p = parts(path);
  if (p[0] == "roms") return p.size() >= 3;
  return p.size() >= 3 && p[0] == "retroarch" && (p[1] == "overlays" || p[1] == "config");
}
std::string corePath(const std::string &core) {
  if (core.empty() || core.size() > 64 || core.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
    return "";
  return "retroarch/cores/" + core + "_libretro_libnx.nro";
}
std::string systemName(const System &system, const std::string &languageCode) {
  static const char *codes[] = {"ko", "en", "ja", "zh"};
  for (size_t k = 0; k < system.names.size(); ++k)
    if (languageCode == codes[k] && !system.names[k].empty()) return system.names[k];
  return system.name;
}

PackRecord parsePackRecord(const std::string &json) {
  PackRecord record;
  Error error;
  auto root = parseJson(json, error);
  if (!root) return record;
  record.version = stringField(root.get(), "version");
  if (auto *files = field(root.get(), "files"); json_object_is_type(files, json_type_object)) {
    json_object_object_foreach(files, key, value) {
      if (json_object_is_type(value, json_type_string)) record.files[key] = json_object_get_string(value);
    }
  }
  return record;
}
std::string serializePackRecord(const PackRecord &record) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "version", json_object_new_string(record.version.c_str()));
  auto *files = json_object_new_object();
  for (const auto &[path, sha] : record.files)
    json_object_object_add(files, path.c_str(), json_object_new_string(sha.c_str()));
  json_object_object_add(root.get(), "files", files);
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
PackPlan planPack(const Pack &pack, const PackRecord &record, bool (*exists)(const std::string &),
                  uint64_t (*sizeOf)(const std::string &), const std::string &sd) {
  PackPlan plan;
  for (const auto &file : pack.files) {
    const auto local = sd + "/" + file.path;
    const bool there = exists(local);
    // A settings file the user may have changed is only ever created.
    if (file.once && there) continue;
    const auto known = record.files.find(file.path);
    if (there && known != record.files.end() && known->second == file.sha256 && sizeOf(local) == file.size) continue;
    plan.fetch.push_back(file);
    plan.bytes += file.size;
  }
  return plan;
}
std::string serializeGame(const Game &game) {
  Json root(json_object_new_object());
  const auto text = [&](const char *key, const std::string &value) {
    json_object_object_add(root.get(), key, json_object_new_string(value.c_str()));
  };
  text("id", game.id);
  text("name", game.name);
  text("file", game.file);
  text("system", game.system);
  text("core", game.core);
  text("original", game.original);
  text("variant", game.variant);
  text("region", game.region);
  auto *edition = json_object_new_object();
  json_object_object_add(edition, "korean", json_object_new_boolean(game.edition.korean));
  json_object_object_add(edition, "source", json_object_new_string(game.edition.source.c_str()));
  json_object_object_add(edition, "from", json_object_new_string(game.edition.from.c_str()));
  json_object_object_add(edition, "version", json_object_new_string(game.edition.version.c_str()));
  json_object_object_add(root.get(), "edition", edition);
  text("iconUrl", game.iconUrl);
  text("bundleUrl", game.bundleUrl);
  text("forwarderUrl", game.forwarderUrl);
  text("forwarderTitleId", game.forwarderTitleId);
  json_object_object_add(root.get(), "size", json_object_new_int64(int64_t(game.size)));
  json_object_object_add(root.get(), "forwarderSize", json_object_new_int64(int64_t(game.forwarderSize)));
  json_object_object_add(root.get(), "rating", json_object_new_int(game.rating));
  auto *install = json_object_new_object();
  json_object_object_add(install, "rom", json_object_new_string(game.romPath.c_str()));
  json_object_object_add(install, "bezelPng", json_object_new_string(game.bezelPath.c_str()));
  json_object_object_add(root.get(), "install", install);
  auto *bezel = json_object_new_object();
  json_object_object_add(bezel, "hasBezel", json_object_new_boolean(game.bezel));
  json_object_object_add(root.get(), "bezel", bezel);
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
Game parseGameJson(const std::string &json) {
  Error error;
  auto root = parseJson(json, error);
  if (!root || !json_object_is_type(root.get(), json_type_object)) return {};
  return parseGame(root.get(), "");
}
} // namespace foil::retro
