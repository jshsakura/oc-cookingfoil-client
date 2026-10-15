#include "Test.hpp"
#include "core/Retro.hpp"
#include <set>
using namespace foil::retro;
namespace {
std::set<std::string> onCard;
bool exists(const std::string &path) { return onCard.count(path) > 0; }
std::string shortFile; // a file on the card cut short
uint64_t sizeOf(const std::string &path) { return path == shortFile ? 4 : 10; }
} // namespace
int runRetroTests() {
  int failures = 0;
  const auto systems = parseSystems(R"({"systems":[
    {"id":"gba","name":"Game Boy Advance","names":{"ko":"게임보이 어드밴스"},"defaultCore":"mgba","coreName":"mGBA","romCount":181},
    {"id":"../x","name":"bad"},{"id":""}]})");
  CHECK(systems.size() == 1);
  if (!systems.empty()) {
    CHECK(systems[0].romCount == 181 && systems[0].coreName == "mGBA");
    CHECK(systemName(systems[0], "ko") == "게임보이 어드밴스");
    CHECK(systemName(systems[0], "ja") == "Game Boy Advance");
    // A region the server names is kept, anything else dropped.
    CHECK(parseGameJson(R"({"id":"a","bundleUrl":"https://h/b","region":"kr"})").region == "kr");
    CHECK(parseGameJson(R"({"id":"a","bundleUrl":"https://h/b","region":"mars"})").region.empty());
    CHECK(parseGameJson(serializeGame(parseGameJson(R"({"id":"a","bundleUrl":"https://h/b","region":"eu"})"))).region == "eu");
    // A game's detail: the game in full, then its versions; an id that is not
    // a plain name is dropped, and missing says it would not run.
    {
      const auto d = parseDetail(R"({"id":"arcade-1","name":"U.N. Squadron","variants":2,"bundleUrl":"/b1",
        "variants":[{"id":"arcade-1","name":"U.N. Squadron","variant":"USA","region":"us","size":1274531,"representative":true},
                    {"id":"arcade-2","name":"Area 88","variant":"Japan","region":"jp","size":2561379,"edition":{"korean":true},"missing":true},
                    {"id":"../x","name":"bad"}]})", "https://h");
      CHECK(d.game.id == "arcade-1" && d.game.bundleUrl == "https://h/b1");
      CHECK(d.variants.size() == 2);
      if (d.variants.size() == 2) {
        CHECK(d.variants[0].representative && !d.variants[0].missing && d.variants[0].region == "us");
        CHECK(d.variants[1].id == "arcade-2" && d.variants[1].missing && d.variants[1].edition.korean);
        CHECK(d.variants[1].size == 2561379 && d.variants[1].variant == "Japan");
      }
      CHECK(d.meta.description.empty() && d.meta.screenshots.empty() && !d.meta.releaseDate);
      CHECK(parseGameJson(R"({"id":"a","bundleUrl":"https://h/b","variants":3})").variants == 3);
      CHECK(parseGameJson(R"({"id":"a","bundleUrl":"https://h/b"})").variants == 1);
    }
    // What the server knows of a game besides its files: pictures resolved
    // against the shop, a date only when it is one, odd entries dropped.
    {
      const auto m = parseDetail(R"({"id":"a","bundleUrl":"/b","description":"Shoot.","publisher":"Capcom",
        "developer":"Capcom","releaseDate":19900227,"numberOfPlayers":2,"genres":["슈팅",3,""],
        "boxartUrl":"/api/retro/games/a/boxart?v=1","screenshots":["/s1.png","https://cdn/s2.png","ftp://x",7]})", "https://h").meta;
      CHECK(m.description == "Shoot." && m.publisher == "Capcom" && m.developer == "Capcom" && m.players == 2);
      CHECK(m.releaseDate == 19900227 && m.boxartUrl == "https://h/api/retro/games/a/boxart?v=1");
      CHECK(m.genres.size() == 1 && m.genres[0] == "슈팅");
      CHECK(m.screenshots.size() == 2 && m.screenshots[0] == "https://h/s1.png" && m.screenshots[1] == "https://cdn/s2.png");
      CHECK(!parseDetail(R"({"id":"a","bundleUrl":"/b","releaseDate":1990})", "https://h").meta.releaseDate);
      // Only the year, or the year and month: the unknown part is left off.
      CHECK(parseDetail(R"({"id":"a","bundleUrl":"/b","releaseDate":19900000})", "https://h").meta.releaseDate == 19900000);
      CHECK(releaseLabel(19900227) == "1990.02.27" && releaseLabel(19900000) == "1990" && releaseLabel(19900200) == "1990.02");
    }
    // The edition's tags, each checked; unknown ones are dropped.
    {
      const auto g = parseGameJson(R"({"id":"a","bundleUrl":"https://h/b","edition":{"korean":true,"source":"patch","from":"jp","version":"v1.1"}})");
      CHECK(g.edition.korean && g.edition.source == "patch" && g.edition.from == "jp" && g.edition.version == "v1.1");
      CHECK(parseGameJson(serializeGame(g)).edition.source == "patch");
      CHECK(parseGameJson(R"({"id":"a","bundleUrl":"https://h/b","edition":{"source":"bootleg","from":"xx"}})").edition.source.empty());
    }
    // A game's core on the card, by RetroArch's own file names.
    CHECK(corePath("fbneo") == "retroarch/cores/fbneo_libretro_libnx.nro");
    CHECK(corePath("").empty() && corePath("../x").empty() && corePath("a/b").empty());
  }
  CHECK(parseSystems("not json").empty());

  const auto page = parseGames(R"({"total":181,"page":2,"pageSize":50,"games":[
    {"id":"g1","name":"Mother 3","file":"Mother 3.gba","system":"gba","size":33554432,
     "iconUrl":"/api/retro/games/g1/icon","bundleUrl":"/api/retro/games/g1/bundle","bezel":{"hasBezel":true},"install":{"rom":"roms/gba/Mother 3.gba","bezelPng":"retroarch/overlays/GameBezels/GBA/Mother 3.png"}},
    {"id":"g2","file":"Only File.gba","bundleUrl":"https://shop.example/b","iconUrl":"javascript:x"},
    {"id":"g3","name":"No bundle"}]})", "https://shop.example");
  CHECK(page.total == 181 && page.page == 2);
  CHECK(page.games.size() == 2);
  if (page.games.size() == 2) {
    CHECK(page.games[0].bundleUrl == "https://shop.example/api/retro/games/g1/bundle");
    CHECK(page.games[0].iconUrl == "https://shop.example/api/retro/games/g1/icon");
    CHECK(page.games[0].bezel == Game::Bezel::Game && page.games[0].size == 33554432);
    CHECK(page.games[1].bezel == Game::Bezel::None);
    CHECK(page.games[0].romPath == "roms/gba/Mother 3.gba");
    CHECK(page.games[0].bezelPath == "retroarch/overlays/GameBezels/GBA/Mother 3.png");
    CHECK(page.games[1].name == "Only File.gba"); // the file name when there is no name
    CHECK(page.games[1].iconUrl.empty());
  }

  // Home: a shelf per system with games, one card per original title.
  const auto shelves = parseHome(R"j({"systems":[
    {"id":"snes","name":"SNES","romCount":3,"games":[
      {"id":"z1","name":"Zelda (KO)","original":"Zelda","rating":95,"ratingCount":40,"bundleUrl":"/b/z1"},
      {"id":"z2","name":"Zelda (KO v2)","original":"Zelda","rating":95,"bundleUrl":"/b/z2"},
      {"id":"m1","name":"Mario","rating":101,"bundleUrl":"/b/m1","forwarderUrl":"/f/m1.nsp","forwarderTitleId":"05D8F62C1705B000","forwarderSize":287448},
      {"id":"x1","name":"No original A","bundleUrl":"/b/x1"},
      {"id":"x2","name":"No original B","bundleUrl":"/b/x2"}]},
    {"id":"nes","name":"NES","romCount":0,"games":[]}]})j",
                                 "https://shop.example");
  CHECK(shelves.size() == 1);
  if (shelves.size() == 1) {
    CHECK(shelves[0].games.size() == 4);
    if (shelves[0].games.size() == 4) {
      CHECK(shelves[0].games[0].id == "z1" && shelves[0].games[0].rating == 95 && shelves[0].games[0].ratingCount == 40);
      CHECK(shelves[0].games[1].id == "m1" && shelves[0].games[1].rating == 100); // clamped
      CHECK(shelves[0].games[1].forwarderUrl == "https://shop.example/f/m1.nsp");
      CHECK(shelves[0].games[1].forwarderTitleId == "05D8F62C1705B000" && shelves[0].games[1].forwarderSize == 287448);
      CHECK(shelves[0].games[0].forwarderUrl.empty());
      CHECK(shelves[0].games[2].rating == 0);
    }
  }
  CHECK(parseHome("{}").empty());
  {
    Filter f;
    CHECK(filterQuery(f) == "&sort=rating");
    f.query = "젤 다";
    f.sort = "size";
    f.minRating = 120;
    f.bezel = true;
    f.maxSize = 1048576;
    CHECK(filterQuery(f) == "&sort=size&q=%EC%A0%A4%20%EB%8B%A4&minRating=100&bezel=1&maxSize=1048576");
  }

  CHECK(packPathAllowed("switch/retroarch_switch.nro"));
  CHECK(packPathAllowed("retroarch/cores/mgba_libretro_libnx.nro"));
  CHECK(!packPathAllowed("switch/other.nro"));
  CHECK(!packPathAllowed("retroarch/../atmosphere/x"));
  CHECK(!packPathAllowed("/retroarch/x"));
  CHECK(!packPathAllowed("retroarch/"));
  CHECK(!packPathAllowed("atmosphere/contents/x"));
  // A game saved under an id the server dropped asks for the list again;
  // other failures keep their HTTP code.
  CHECK(bundleFailure(404, true).find("목록에서 다시") != std::string::npos);
  CHECK(bundleFailure(500, true) == "게임을 받지 못했습니다. HTTP 500");
  CHECK(bundleFailure(404, false) == "파일을 받지 못했습니다. HTTP 404");
  CHECK(bundlePathAllowed("roms/gba/Mother 3.gba"));
  CHECK(bundlePathAllowed("retroarch/overlays/GameBezels/GBA/Mother 3.png"));
  CHECK(bundlePathAllowed("retroarch/config/mGBA/Mother 3.cfg"));
  CHECK(!bundlePathAllowed("roms/gba"));
  CHECK(!bundlePathAllowed("retroarch/retroarch.cfg"));
  CHECK(!bundlePathAllowed("roms/../switch/x.nro"));
  CHECK(!bundlePathAllowed("switch/retroarch_switch.nro"));

  const auto pack = parsePack(R"({"version":"v1","files":[
    {"path":"switch/retroarch_switch.nro","size":10,"sha256":"a","url":"/api/retro/file/a"},
    {"path":"retroarch/retroarch.cfg","size":10,"sha256":"b","url":"/api/retro/file/b","once":true},
    {"path":"retroarch/cores/x.nro","size":10,"sha256":"c","url":"/api/retro/file/c"},
    {"path":"atmosphere/x","size":1,"sha256":"d","url":"/api/retro/file/d"}]})", "https://shop.example");
  CHECK(pack.version == "v1" && pack.files.size() == 3);
  // Nothing on the card: everything is fetched.
  onCard.clear();
  PackRecord record;
  CHECK(planPack(pack, record, exists, sizeOf, "sdmc:").fetch.size() == 3);
  // The settings file is never replaced; recorded unchanged files are kept;
  // a file whose hash changed on the server is fetched again.
  onCard = {"sdmc:/switch/retroarch_switch.nro", "sdmc:/retroarch/retroarch.cfg", "sdmc:/retroarch/cores/x.nro"};
  record.files = {{"switch/retroarch_switch.nro", "a"}, {"retroarch/cores/x.nro", "old"}};
  const auto plan = planPack(pack, record, exists, sizeOf, "sdmc:");
  CHECK(plan.fetch.size() == 1);
  if (plan.fetch.size() == 1) CHECK(plan.fetch[0].path == "retroarch/cores/x.nro");
  CHECK(plan.bytes == 10);
  // The card decides, never the record alone: a recorded file that is gone
  // or whose size changed is fetched again. The plan reads sizes only; no
  // file is hashed to decide.
  record.files["retroarch/cores/x.nro"] = "c";
  CHECK(planPack(pack, record, exists, sizeOf, "sdmc:").fetch.empty());
  onCard.erase("sdmc:/switch/retroarch_switch.nro");
  CHECK(planPack(pack, record, exists, sizeOf, "sdmc:").fetch.size() == 1);
  onCard.insert("sdmc:/switch/retroarch_switch.nro");
  shortFile = "sdmc:/retroarch/cores/x.nro";
  const auto cut = planPack(pack, record, exists, sizeOf, "sdmc:");
  CHECK(cut.fetch.size() == 1 && cut.fetch[0].path == "retroarch/cores/x.nro");
  shortFile.clear();
  record.files["retroarch/cores/x.nro"] = "old";
  const auto again = parsePackRecord(serializePackRecord(record));
  CHECK(again.files == record.files);
  // A game kept in 다운로드's history comes back whole for 다시 시도.
  Game kept;
  kept.id = "nes-1";
  kept.name = "Zelda (KO)";
  kept.system = "nes";
  kept.iconUrl = "https://shop.example/i.png";
  kept.bundleUrl = "https://shop.example/b.tar";
  kept.forwarderUrl = "https://shop.example/f.nsp";
  kept.forwarderTitleId = "0500000000000001";
  kept.forwarderSize = 300;
  kept.romPath = "roms/nes/zelda.zip";
  kept.bezelPath = "retroarch/overlays/zelda.png";
  kept.size = 1234;
  kept.bezel = Game::Bezel::System;
  kept.variant = "J-K v20230715 v1 font.galmuri";
  const auto back = parseGameJson(serializeGame(kept));
  CHECK(back.id == "nes-1" && back.name == "Zelda (KO)" && back.system == "nes" && back.variant == kept.variant);
  CHECK(back.bundleUrl == kept.bundleUrl && back.iconUrl == kept.iconUrl);
  CHECK(back.forwarderUrl == kept.forwarderUrl && back.forwarderTitleId == kept.forwarderTitleId && back.forwarderSize == 300);
  CHECK(back.romPath == kept.romPath && back.bezelPath == kept.bezelPath && back.size == 1234 &&
        back.bezel == Game::Bezel::System);
  // The bezel's kind: named by the server, else from its two flags.
  const auto bezelOf = [](const std::string &bezel) {
    return parseGameJson(R"({"id":"a","bundleUrl":"https://h/b","bezel":)" + bezel + "}").bezel;
  };
  CHECK(bezelOf(R"({"kind":"game","hasBezel":true,"generic":true})") == Game::Bezel::Game);
  CHECK(bezelOf(R"({"kind":"system","hasBezel":true})") == Game::Bezel::System);
  CHECK(bezelOf(R"({"kind":"none","hasBezel":true})") == Game::Bezel::None);
  CHECK(bezelOf(R"({"hasBezel":true,"generic":true})") == Game::Bezel::System);
  CHECK(bezelOf(R"({"hasBezel":true,"generic":false})") == Game::Bezel::Game);
  CHECK(bezelOf(R"({"hasBezel":false})") == Game::Bezel::None);
  CHECK(parseGameJson("not json").id.empty());
  return failures;
}
