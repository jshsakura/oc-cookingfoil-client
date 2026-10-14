#pragma once
// Retro games served by the shop (/api/retro): systems, paged game lists,
// per-game bundles (a tar from the SD root) and the RetroArch base pack.
#include "core/Types.hpp"
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
  bool bezel = false;
};
// One version of a game, as its detail lists them: picking one means its id.
struct Variant {
  std::string id, name, variant, region;
  Game::Edition edition;
  uint64_t size = 0;
  bool representative = false; // the server's pick for the list
  bool missing = false;        // a file it needs (a parent zip, a BIOS) is not there: it would not run
};
// /api/retro/games/:id: the game in full, with its versions (when it has more than one).
struct Detail {
  Game game;
  std::vector<Variant> variants;
};
// What the system list asks the server for: search words, order, and the
// filter popup's conditions (0 / false: not set).
struct Filter {
  std::string query, sort = "rating"; // rating, name, size (largest first), added
  int minRating = 0;
  bool bezel = false; // a bezel made for the game, not the system's default
  uint64_t minSize = 0, maxSize = 0;
  bool operator==(const Filter &o) const {
    return query == o.query && sort == o.sort && minRating == o.minRating && bezel == o.bezel &&
           minSize == o.minSize && maxSize == o.maxSize;
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
} // namespace foil::retro
