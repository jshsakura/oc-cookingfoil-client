#include "core/Retro.hpp"
#include "core/Forwarder.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include <algorithm>
#include <cctype>
#include <map>
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
// A genre key: lower case letters, digits, - and _ (empty for anything else).
std::string plainKey(json_object *value, bool theme = false) {
  if (!json_object_is_type(value, json_type_string)) return {};
  const std::string key = json_object_get_string(value);
  return theme ? (themeIdText(key) ? key : "") : (keyText(key) ? key : "");
}
std::string encoded(const std::string &text) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
// One card per original title, the first (best rated) kept.
void oneCardPerTitle(std::vector<Game> &games) {
  std::set<std::string> seen;
  games.erase(std::remove_if(games.begin(), games.end(),
                             [&](const Game &g) { return !g.original.empty() && !seen.insert(g.original).second; }),
              games.end());
}
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
  s.names = languageTexts(object, "names");
  if (auto *core = field(object, "core"); json_object_is_type(core, json_type_object))
    s.coreOff = stringField(core, "state") == "core-off";
  return s;
}
// Empty id or bundle URL means the entry is skipped.
std::string region(json_object *object, const char *key) {
  static const std::set<std::string> Regions{"kr", "jp", "us", "eu", "asia", "br", "world"};
  const auto code = stringField(object, key);
  return Regions.count(code) ? code : "";
}
Game::Edition edition(json_object *object) {
  Game::Edition e;
  auto *tags = field(object, "edition");
  if (!json_object_is_type(tags, json_type_object)) return e;
  e.korean = json_object_get_boolean(field(tags, "korean"));
  e.source = stringField(tags, "source");
  if (e.source != "official" && e.source != "patch") e.source.clear();
  e.from = region(tags, "from");
  e.version = stringField(tags, "version");
  if (e.version.size() > 24) e.version.clear();
  return e;
}
Game parseGame(json_object *object, const std::string &base) {
  Game g;
  g.id = stringField(object, "id");
  g.name = stringField(object, "name");
  g.file = stringField(object, "file");
  g.system = stringField(object, "system");
  g.core = stringField(object, "core");
  g.original = stringField(object, "original");
  g.variant = stringField(object, "variant");
  g.shortcutName = stringField(object, "shortcutName");
  g.region = region(object, "region");
  g.edition = edition(object);
  g.size = positive(object, "size");
  g.addedAt = integerField(object, "addedAt");
  g.rating = int(std::clamp<int64_t>(integerField(object, "rating"), 0, 100));
  g.ratingCount = int(std::clamp<int64_t>(integerField(object, "ratingCount"), 0, 1 << 30));
  g.variants = int(std::clamp<int64_t>(integerField(object, "variants", 1), 1, 999));
  auto *verified = field(object, "verified");
  g.verified = json_object_is_type(verified, json_type_boolean) && json_object_get_boolean(verified);
  auto *coreOff = field(object, "coreOff");
  g.coreOff = json_object_is_type(coreOff, json_type_boolean) && json_object_get_boolean(coreOff);
  g.votes = votes::fromItem(object);
  g.iconUrl = url(object, "iconUrl", base);
  g.bundleUrl = url(object, "bundleUrl", base);
  g.forwarderUrl = url(object, "forwarderUrl", base);
  g.forwarderTitleId = stringField(object, "forwarderTitleId");
  if (!forwarder::serverShortcutId(g.forwarderTitleId))
    g.forwarderUrl.clear(), g.forwarderTitleId.clear();
  g.forwarderSize = positive(object, "forwarderSize");
  if (auto *install = field(object, "install"); json_object_is_type(install, json_type_object)) {
    g.romPath = stringField(install, "rom");
    if (!bundlePathAllowed(g.romPath)) g.romPath.clear();
    g.bezelPath = stringField(install, "bezelPng");
    if (!bundlePathAllowed(g.bezelPath)) g.bezelPath.clear();
  }
  if (auto *bezel = field(object, "bezel"); json_object_is_type(bezel, json_type_object)) {
    // Its kind when the server names it; else read from the two flags.
    const auto kind = stringField(bezel, "kind");
    if (kind == "game") g.bezel = Game::Bezel::Game;
    else if (kind == "system") g.bezel = Game::Bezel::System;
    else if (kind.empty() && json_object_get_boolean(field(bezel, "hasBezel")))
      g.bezel = json_object_get_boolean(field(bezel, "generic")) ? Game::Bezel::System : Game::Bezel::Game;
  }
  g.achievements = achievements::parseSummary(field(object, "achievements"));
  if (auto *genres = field(object, "genres"); json_object_is_type(genres, json_type_array))
    for (size_t i = 0; i < json_object_array_length(genres) && g.genres.size() < 8; ++i)
      if (const auto key = plainKey(json_object_array_get_idx(genres, i)); !key.empty()) g.genres.push_back(key);
  g.players = int(std::clamp<int64_t>(integerField(object, "players"), 0, 99));
  g.year = int(std::clamp<int64_t>(integerField(object, "year"), 0, 9999));
  g.labels = labels::parseRefs(object, g.hasLabels);
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
  const auto q = encoded(f.query);
  std::string out = "&sort=" + (keyText(f.sort) ? f.sort : std::string("rating"));
  if (!q.empty()) out += "&q=" + q;
  // Names and values were checked when the server's filters were read.
  for (const auto &[name, value] : f.params) out += "&" + name + "=" + value;
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
Page parseGames(const std::string &json, const std::string &base, labels::Book *book) {
  Page page;
  Error error;
  auto root = parseJson(json, error);
  if (!root) return page;
  if (book) *book = labels::parseBook(root.get());
  page.total = int(std::max<int64_t>(integerField(root.get(), "total"), 0));
  page.page = int(std::max<int64_t>(integerField(root.get(), "page", 1), 1));
  page.pageSize = int(std::clamp<int64_t>(integerField(root.get(), "pageSize", 50), 1, 200));
  page.games = parseGameList(field(root.get(), "games"), base);
  return page;
}
Detail parseDetail(const std::string &json, const std::string &base) {
  Detail d;
  Error ignored;
  auto root = parseJson(json, ignored);
  if (!root) return d;
  d.game = parseGame(root.get(), base);
  auto &m = d.meta;
  m.description = stringField(root.get(), "description");
  m.publisher = stringField(root.get(), "publisher");
  m.developer = stringField(root.get(), "developer");
  m.boxartUrl = url(root.get(), "boxartUrl", base);
  if (const auto date = integerField(root.get(), "releaseDate", 0); date >= 19500000 && date <= 21001231)
    m.releaseDate = int(date);
  m.players = int(std::clamp<int64_t>(integerField(root.get(), "numberOfPlayers", 0), 0, 99));
  auto *genres = field(root.get(), "genres");
  if (json_object_is_type(genres, json_type_array))
    for (size_t i = 0; i < json_object_array_length(genres) && m.genres.size() < 8; ++i) {
      auto *genre = json_object_array_get_idx(genres, i);
      if (json_object_is_type(genre, json_type_string) && json_object_get_string_len(genre) > 0)
        m.genres.emplace_back(json_object_get_string(genre));
    }
  // Server paths and full addresses, as the game's other pictures.
  m.screenshots = urlList(root.get(), "screenshots", base, 8);
  m.achievements = achievements::parseSummary(json);
  auto *list = field(root.get(), "variants");
  if (!json_object_is_type(list, json_type_array)) return d;
  for (size_t i = 0; i < json_object_array_length(list) && d.variants.size() < 100; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    Variant v;
    v.id = stringField(object, "id");
    if (v.id.empty() || v.id.size() > 64 || v.id.find_first_of("/?#&% ") != std::string::npos) continue;
    v.name = stringField(object, "name");
    v.variant = stringField(object, "variant");
    v.region = region(object, "region");
    v.edition = edition(object);
    v.size = positive(object, "size");
    v.representative = json_object_get_boolean(field(object, "representative"));
    v.missing = json_object_get_boolean(field(object, "missing"));
    if (v.name.empty()) v.name = stringField(object, "file");
    d.variants.push_back(std::move(v));
  }
  return d;
}
std::string releaseLabel(int date) {
  const auto text = std::to_string(date);
  if (text.size() != 8) return text;
  if (text.compare(4, 2, "00") == 0) return text.substr(0, 4);
  if (text.compare(6, 2, "00") == 0) return text.substr(0, 4) + "." + text.substr(4, 2);
  return text.substr(0, 4) + "." + text.substr(4, 2) + "." + text.substr(6, 2);
}
std::vector<Shelf> parseHome(const std::string &json, const std::string &base, labels::Book *book) {
  std::vector<Shelf> out;
  Error error;
  auto root = parseJson(json, error);
  if (root && book) *book = labels::parseBook(root.get());
  auto *list = root ? field(root.get(), "systems") : nullptr;
  if (!json_object_is_type(list, json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(list) && out.size() < 200; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    Shelf shelf{parseSystem(object, base), parseGameList(field(object, "games"), base)};
    oneCardPerTitle(shelf.games);
    if (shelf.system.id.empty() || shelf.games.empty()) continue;
    out.push_back(std::move(shelf));
  }
  return out;
}
std::vector<Theme> parseThemes(const std::string &json, const std::string &base, labels::Book *book) {
  std::vector<Theme> out;
  Error error;
  auto root = parseJson(json, error);
  if (root && book) *book = labels::parseBook(root.get());
  auto *list = root ? field(root.get(), "themes") : nullptr;
  if (!json_object_is_type(list, json_type_array)) return out;
  for (size_t i = 0; i < json_object_array_length(list) && out.size() < 64; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    Theme theme{plainKey(field(object, "id"), true), stringField(object, "name"), parseGameList(field(object, "games"), base)};
    oneCardPerTitle(theme.games);
    if (const auto icon = stringField(object, "icon"); keyText(icon)) theme.icon = icon;
    if (theme.id.empty() || theme.name.empty() || theme.games.empty()) continue;
    out.push_back(std::move(theme));
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
std::string bundleFailure(long status, bool game) {
  if (game && status == 404)
    return tr("The game's details changed on the server. Download it again from the Retro games list.");
  // The server turned its core off: the files are kept back until it is on.
  if (status == 409) return tr("The server has no core registered for this game.");
  const auto code = std::to_string(status);
  return game ? trf("Could not get the game. HTTP {0}", {code}) : trf("Could not get the file. HTTP {0}", {code});
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
  const auto *found = displayText(system.names, languageCode);
  return found ? *found : system.name;
}

PackRecord parsePackRecord(const std::string &json) {
  PackRecord record;
  Error error;
  auto root = parseJson(json, error);
  if (!root) return record;
  record.version = stringField(root.get(), "version");
  const auto read = [&](const char *name, std::map<std::string, std::string> &into) {
    if (auto *files = field(root.get(), name); json_object_is_type(files, json_type_object)) {
      json_object_object_foreach(files, key, value) {
        if (json_object_is_type(value, json_type_string)) into[key] = json_object_get_string(value);
      }
    }
  };
  read("files", record.files);
  read("refused", record.refused);
  return record;
}
std::string serializePackRecord(const PackRecord &record) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "version", json_object_new_string(record.version.c_str()));
  const auto write = [&](const char *name, const std::map<std::string, std::string> &from) {
    auto *files = json_object_new_object();
    for (const auto &[path, sha] : from)
      json_object_object_add(files, path.c_str(), json_object_new_string(sha.c_str()));
    json_object_object_add(root.get(), name, files);
  };
  write("files", record.files);
  if (!record.refused.empty()) write("refused", record.refused);
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
    if (const auto no = record.refused.find(file.path);
        !there && no != record.refused.end() && !file.sha256.empty() && no->second == file.sha256)
      continue;
    const auto known = record.files.find(file.path);
    if (there && known != record.files.end() && known->second == file.sha256 && sizeOf(local) == file.size) continue;
    plan.fetch.push_back(file);
    plan.bytes += file.size;
  }
  return plan;
}
bool planByBundle(const PackPlan &plan, uint64_t bundleSize) {
  constexpr size_t BundleFrom = 50; // fewer files come one by one whatever their size
  return bundleSize && plan.fetch.size() >= BundleFrom && plan.bytes >= bundleSize / 2;
}
std::string regionName(const std::string &code) {
  if (code == "kr") return tr("Korea|region");
  if (code == "jp") return tr("Japan|region");
  if (code == "us") return tr("North America|region");
  if (code == "eu") return tr("Europe|region");
  if (code == "asia") return tr("Asia|region");
  if (code == "br") return tr("Brazil|region");
  if (code == "world") return tr("World|region");
  return code;
}
std::string shortcutName(const Game &game) {
  if (!game.shortcutName.empty()) return game.shortcutName;
  const auto tag = game.variants < 2 ? std::string() : !game.variant.empty() ? game.variant : regionName(game.region);
  return tag.empty() ? game.name : game.name + " · " + tag;
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
  text("shortcutName", game.shortcutName);
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
  json_object_object_add(root.get(), "variants", json_object_new_int(game.variants));
  auto *install = json_object_new_object();
  json_object_object_add(install, "rom", json_object_new_string(game.romPath.c_str()));
  json_object_object_add(install, "bezelPng", json_object_new_string(game.bezelPath.c_str()));
  json_object_object_add(root.get(), "install", install);
  if (game.achievements.count > 0) {
    auto *ra = json_object_new_object();
    json_object_object_add(ra, "raGameId", json_object_new_int64(game.achievements.raGameId));
    json_object_object_add(ra, "count", json_object_new_int(game.achievements.count));
    json_object_object_add(ra, "points", json_object_new_int(game.achievements.points));
    json_object_object_add(ra, "matchedBy", json_object_new_string(game.achievements.byTitle ? "title" : "hash"));
    json_object_object_add(root.get(), "achievements", ra);
  }
  auto *bezel = json_object_new_object();
  json_object_object_add(bezel, "hasBezel", json_object_new_boolean(game.bezel != Game::Bezel::None));
  json_object_object_add(bezel, "generic", json_object_new_boolean(game.bezel == Game::Bezel::System));
  json_object_object_add(bezel, "kind", json_object_new_string(game.bezel == Game::Bezel::Game     ? "game"
                                                              : game.bezel == Game::Bezel::System ? "system"
                                                                                                   : "none"));
  json_object_object_add(root.get(), "bezel", bezel);
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
Game parseGameJson(const std::string &json) {
  Error error;
  auto root = parseJson(json, error);
  if (!root || !json_object_is_type(root.get(), json_type_object)) return {};
  return parseGame(root.get(), "");
}
namespace {
std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}
// Games named by their server, a game several hold once (by id).
std::vector<Game> tagged(std::vector<std::pair<std::string, std::vector<Game>>> lists) {
  std::vector<Game> games;
  std::map<std::string, size_t> seen;
  for (auto &[server, list] : lists)
    for (auto &game : list) {
      game.server = server;
      const auto [at, fresh] = seen.emplace(game.id, games.size());
      if (fresh) {
        games.push_back(std::move(game));
        continue;
      }
      auto &also = games[at->second].alsoOn;
      if (std::find(also.begin(), also.end(), server) == also.end()) also.push_back(server);
    }
  return games;
}
void sortGames(std::vector<Game> &games, const std::string &sort) {
  std::stable_sort(games.begin(), games.end(), [&](const Game &a, const Game &b) {
    if (sort == "name") return lower(a.name) < lower(b.name);
    if (sort == "size") return a.size > b.size;
    if (sort == "added") return a.addedAt > b.addedAt;
    return a.rating != b.rating ? a.rating > b.rating : a.ratingCount > b.ratingCount;
  });
}
} // namespace
std::vector<Game> mergeGames(std::vector<std::pair<std::string, std::vector<Game>>> lists, const std::string &sort) {
  // One server's pages are in its order already.
  const bool one = lists.size() == 1;
  auto games = tagged(std::move(lists));
  if (!one) sortGames(games, sort);
  return games;
}
void orderGames(std::vector<Game> &games, const std::string &sort) {
  std::map<std::string, size_t> seen;
  std::vector<Game> kept;
  kept.reserve(games.size());
  for (auto &game : games) {
    const auto [at, fresh] = seen.emplace(game.id, kept.size());
    if (fresh) {
      kept.push_back(std::move(game));
      continue;
    }
    auto &also = kept[at->second].alsoOn;
    for (const auto &server : game.alsoOn)
      if (std::find(also.begin(), also.end(), server) == also.end()) also.push_back(server);
    if (game.server != kept[at->second].server && std::find(also.begin(), also.end(), game.server) == also.end())
      also.push_back(game.server);
  }
  games = std::move(kept);
  sortGames(games, sort);
}
std::vector<Theme> mergeThemes(std::vector<std::pair<std::string, std::vector<Theme>>> lists) {
  std::vector<Theme> themes;
  std::map<std::string, size_t> at;
  std::map<std::string, std::vector<std::pair<std::string, std::vector<Game>>>> parts;
  for (auto &[server, list] : lists)
    for (auto &theme : list) {
      if (at.emplace(theme.id, themes.size()).second) themes.push_back({theme.id, theme.name, {}, theme.icon});
      parts[theme.id].push_back({server, std::move(theme.games)});
    }
  for (auto &theme : themes) {
    auto &part = parts[theme.id];
    const bool one = part.size() == 1;
    theme.games = tagged(std::move(part));
    if (one) continue;
    sortGames(theme.games, "rating");
    oneCardPerTitle(theme.games);
  }
  return themes;
}
bool keyText(const std::string &key) {
  return !key.empty() && key.size() <= 32 && key.front() != '-' && key.back() != '-' &&
         key.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
}
bool themeIdText(const std::string &id) {
  if (id.size() > 96) return false;
  size_t from = 0;
  for (size_t colon; (colon = id.find(':', from)) != std::string::npos; from = colon + 1)
    if (!keyText(id.substr(from, colon - from))) return false;
  return keyText(id.substr(from));
}
std::vector<Shelf> themeShelves(std::vector<Theme> themes, int shown) {
  std::vector<Shelf> out;
  for (auto &theme : spreadThemes(std::move(themes), shown))
    out.push_back({{}, std::move(theme.games), std::move(theme.name), theme.id, theme.icon});
  return out;
}
std::vector<Theme> spreadThemes(std::vector<Theme> themes, int rows, int perRow, int fewest) {
  std::vector<Theme> out;
  std::set<std::string> shown;
  const auto key = [](const Game &g) { return g.original.empty() ? "id:" + g.id : "title:" + g.original; };
  for (auto &theme : themes) {
    if (int(out.size()) >= rows) break;
    std::vector<Game> fresh;
    for (auto &game : theme.games)
      if (int(fresh.size()) < perRow && !shown.count("id:" + game.id) && !shown.count(key(game))) fresh.push_back(std::move(game));
    if (int(fresh.size()) < fewest) continue;
    for (const auto &game : fresh) {
      shown.insert("id:" + game.id);
      shown.insert(key(game));
    }
    out.push_back({theme.id, theme.name, std::move(fresh)});
  }
  return out;
}
std::vector<Shelf> mergeShelves(std::vector<std::pair<std::string, std::vector<Shelf>>> lists) {
  std::vector<Shelf> shelves;
  std::map<std::string, size_t> at;
  std::map<std::string, std::vector<std::pair<std::string, std::vector<Game>>>> parts;
  for (auto &[server, list] : lists)
    for (auto &shelf : list) {
      const auto [found, fresh] = at.emplace(shelf.system.id, shelves.size());
      if (fresh) shelves.push_back({shelf.system, {}});
      else shelves[found->second].system.romCount += shelf.system.romCount;
      parts[shelf.system.id].push_back({server, std::move(shelf.games)});
    }
  for (auto &shelf : shelves) {
    auto &part = parts[shelf.system.id];
    const bool one = part.size() == 1;
    shelf.games = tagged(std::move(part));
    if (one) continue;
    sortGames(shelf.games, "rating");
    oneCardPerTitle(shelf.games);
  }
  return shelves;
}
} // namespace foil::retro
