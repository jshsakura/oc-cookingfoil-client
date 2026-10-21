#pragma once
// Retro games served by the shop (/api/retro): systems, paged game lists,
// per-game bundles (a tar from the SD root) and the RetroArch base pack.
#include "core/Achievements.hpp"
#include "core/Types.hpp"
#include "core/Votes.hpp"
#include <map>
namespace foil::retro {
struct System {
  std::string id, name, defaultCore, coreName, iconUrl; // iconUrl: the system's colour icon
  std::vector<std::string> names; // ko, en, ja, zh; any may be empty
  int romCount = 0;
};
struct Game {
  std::string id, name, file, system, core, iconUrl, bundleUrl;
  std::string original; // the game's original title; patched versions share it
  std::string variant;  // the version or patch tag ("J-K v20230715 v1 font.galmuri"); empty for none
  std::string region;   // kr jp us eu asia br world, or empty; shown as a flag instead of the variant
  // The variant's tags, read by the server: official or patch
  // (empty when it sent none), the release it came from (jp, us) and its version.
  struct Edition {
    bool korean = false;
    std::string source, from, version;
  } edition;
  // Its HOME Menu shortcut: a small NSP that starts the game's core and ROM.
  std::string forwarderUrl, forwarderTitleId;
  uint64_t forwarderSize = 0;
  std::string romPath;   // where the bundle puts the ROM, SD-root relative
  std::string bezelPath; // and the bezel picture, when it has one
  uint64_t size = 0;
  int64_t addedAt = 0;
  int rating = 0;      // 0-100 community score; 0 when the server has none
  int ratingCount = 0; // how many people rated it
  int variants = 1;    // versions of it the server has (a list shows one, its pick)
  bool verified = false; // an admin checked that it runs
  votes::Votes votes;
  // Its bezel: one made for this game, the system's default, or none.
  enum class Bezel { None, System, Game } bezel = Bezel::None;
  // Its RetroAchievements match; count 0 when it has none.
  achievements::Summary achievements;
  // What the server knows of it, for its tile: genre keys (named by the
  // facets or themes that carry them), most players at once, release year.
  std::vector<std::string> genres;
  int players = 0, year = 0;
  // The configured server it came from (its url), and the others holding
  // the same game (same id): never serialized, the queue keeps its own.
  std::string server;
  std::vector<std::string> alsoOn;
};
// One version of a game, as its detail lists them: picking one means its id.
struct Variant {
  std::string id, name, variant, region;
  Game::Edition edition;
  uint64_t size = 0;
  bool representative = false; // the server's pick for the list
  bool missing = false;        // a file it needs (a parent zip, a BIOS) is not there: it would not run
};
// What the server knows about a game beyond its files; any of it may be missing.
struct Meta {
  std::string description, publisher, developer, boxartUrl;
  int releaseDate = 0; // YYYYMMDD, or YYYY0000 when only the year is known
  int players = 0;
  std::vector<std::string> genres, screenshots;
  achievements::Summary achievements; // raGameId 0: not matched on RetroAchievements
};
// /api/retro/games/:id: the game in full, with its versions (when it has more than one).
struct Detail {
  Game game;
  std::vector<Variant> variants;
  Meta meta;
};
// What the system list asks the server for: search words, order, and the
// filter popup's conditions (0 / false: not set).
struct Filter {
  std::string query, sort = "rating"; // rating, name, size (largest first), added
  int minRating = 0;
  bool bezel = false; // a bezel made for the game, not the system's default
  uint64_t minSize = 0, maxSize = 0;
  // Only what the system's facets offer is ever set (0 / empty: not set).
  std::vector<std::string> genres; // any of these genre keys
  int players = 0;                 // at least this many at once
  int decade = 0;                  // released in 1990..1999 for 1990
  std::string region;              // a ROM tagged with it (kr, jp, us…)
  bool korean = false;             // a Korean release or a Korean patch
  bool achievements = false;       // matched on RetroAchievements
  bool operator==(const Filter &o) const {
    return query == o.query && sort == o.sort && minRating == o.minRating && bezel == o.bezel &&
           minSize == o.minSize && maxSize == o.maxSize && genres == o.genres && players == o.players &&
           decade == o.decade && region == o.region && korean == o.korean && achievements == o.achievements;
  }
  bool operator!=(const Filter &o) const { return !(*this == o); }
};
// The query string for /api/retro/games after system and page.
std::string filterQuery(const Filter &filter);
struct Page {
  int total = 0, page = 1, pageSize = 50;
  std::vector<Game> games;
};
// One row of the retro home: a system and its best rated games.
struct Shelf {
  System system;
  std::vector<Game> games;
};
// A row across systems (/api/retro/themes): a genre or another theme the
// server picked, named in the asked language.
struct Theme {
  std::string id, name;
  std::vector<Game> games;
};
// What a system's list can be filtered by (/api/retro/systems/:id/facets),
// each with how many games it keeps. Only these are offered; an empty list
// or a 0 count means the filter is not shown.
struct Facet {
  std::string key, name; // a genre's key and name; players and decades: the number as text
  int count = 0;
};
struct Facets {
  std::vector<Facet> genres, players, decades, regions; // regions: the tag as the key
  int korean = 0, achievements = 0;
  bool empty() const {
    return genres.empty() && players.empty() && decades.empty() && regions.empty() && !korean && !achievements;
  }
};
// Several servers' replies as one; each list in the servers' order, as
// {server url, its part}. A game on several (same id) is listed once from
// the first, naming the others.
// Pages of one system: merged in the filter's order (`sort` as Filter::sort).
std::vector<Game> mergeGames(std::vector<std::pair<std::string, std::vector<Game>>> lists, const std::string &sort);
// Games already naming their servers, as one list again (a later page of
// several servers joining): each id once, in the filter's order.
void orderGames(std::vector<Game> &games, const std::string &sort);
// The home: a system's shelves join in the order systems first appear, its
// games best rated first with one card per original title, and its count
// the sum of the servers' (an upper bound when they share games).
std::vector<Shelf> mergeShelves(std::vector<std::pair<std::string, std::vector<Shelf>>> lists);
// Theme rows join by id in the order they first appear, their games best
// rated first with one card per original title.
std::vector<Theme> mergeThemes(std::vector<std::pair<std::string, std::vector<Theme>>> lists);
// Several servers' facets as one: counts summed per key (an upper bound when
// they share games), genres by count, players and decades in order.
Facets mergeFacets(const std::vector<Facets> &all);
// The theme rows the home shows: a game (by id or original title) only in
// the first row that has it, a row left with fewer than `fewest` games
// dropped, at most `rows` rows of `perRow` games. Few, varied, never long.
constexpr int ThemeRows = 6, ThemeGames = 12, ThemeFewest = 6;
std::vector<Theme> spreadThemes(std::vector<Theme> themes, int rows = ThemeRows, int perRow = ThemeGames,
                                int fewest = ThemeFewest);
struct PackFile {
  std::string path, sha256, url;
  uint64_t size = 0;
  bool once = false; // never replace a copy already on the card (user settings)
};
struct Pack {
  std::string version;
  std::vector<PackFile> files;
  std::string bundleUrl; // the whole pack as one tar, when the server has it
  uint64_t bundleSize = 0;
};
// URLs come as shop paths ("/api/...") or full http(s) addresses; `base`
// is the shop address used for the former. Anything else is dropped.
std::vector<System> parseSystems(const std::string &json, const std::string &base = "");
Page parseGames(const std::string &json, const std::string &base);
// /api/retro/home: systems without games are left out, and versions of one
// game (the same `original`) take one card, the best rated first.
std::vector<Shelf> parseHome(const std::string &json, const std::string &base = "");
// /api/retro/themes: themes without a name or games are left out.
std::vector<Theme> parseThemes(const std::string &json, const std::string &base = "");
Facets parseFacets(const std::string &json);
// One game in the server's own shape, for 다운로드's history (다시 시도).
std::string serializeGame(const Game &game);
Game parseGameJson(const std::string &json);
Detail parseDetail(const std::string &json, const std::string &base);
// Files outside the allowed places are left out (see packPathAllowed).
Pack parsePack(const std::string &json, const std::string &base);
// The base pack may write retroarch/... and the RetroArch app itself.
bool packPathAllowed(const std::string &path);
// A game bundle may write its ROM under roms/<system>/ and its bezel and
// per-game config under retroarch/overlays/ and retroarch/config/.
bool bundlePathAllowed(const std::string &path);
// What a failed game or file download says. A game the server no longer
// knows by that id (one saved before its ids changed) is fetched again
// from the list.
std::string bundleFailure(long status, bool game);
// 19900227 -> "1990.02.27"; a month or day the server does not know (00) is left off.
std::string releaseLabel(int date);
// The name shown for a system in the interface language.
std::string systemName(const System &system, const std::string &languageCode);
// Where the RetroArch app is expected (the forwarder chainloads it).
constexpr const char *RetroArchApp = "switch/retroarch_switch.nro";
// Where RetroArch keeps a game's core ("fbneo" -> retroarch/cores/fbneo_libretro_libnx.nro);
// empty for no core or an odd name.
std::string corePath(const std::string &core);

// What was taken from the base pack last time: path -> sha256. Kept in
// state outside the pack so a changed file on the server is fetched again.
struct PackRecord {
  std::string version;
  std::map<std::string, std::string> files;
};
PackRecord parsePackRecord(const std::string &json);
std::string serializePackRecord(const PackRecord &record);
// The files to fetch: missing on the card, changed on the server, or never
// recorded; `exists` and `sizeOf` look at the card.
struct PackPlan {
  std::vector<PackFile> fetch;
  uint64_t bytes = 0;
};
PackPlan planPack(const Pack &pack, const PackRecord &record, bool (*exists)(const std::string &),
                  uint64_t (*sizeOf)(const std::string &), const std::string &sd);
// Whether the plan comes as the whole pack's tar (bundleSize bytes) rather
// than file by file: only for many files that are most of the pack. A new
// core with a few hundred small files fetches those, not the whole pack.
bool planByBundle(const PackPlan &plan, uint64_t bundleSize);
} // namespace foil::retro
