// Two servers' library lists as one: each item names its server, a file both
// hold is listed once from the first and names the other, and different
// versions or kinds of a title stay apart.
#include "Test.hpp"
#include "core/Catalog.hpp"
#include "core/Homebrew.hpp"
#include "core/Patches.hpp"
#include "core/Retro.hpp"
#include "core/Selection.hpp"
#include <algorithm>
namespace {
foil::Item item(const std::string &titleId, const std::string &type, uint32_t version, const std::string &url) {
  foil::Item i;
  i.titleId = titleId;
  i.baseId = titleId.substr(0, 13) + "000";
  i.type = type;
  i.version = version;
  i.url = url;
  i.name = titleId;
  return i;
}
} // namespace
int runMergeCatalogTests() {
  int failures = 0;
  const std::string a = "https://a.example", b = "https://b.example";
  const std::string base = "0100AAAA00010000", update = "0100AAAA00010800";
  std::vector<foil::ServerItems> lists{
      {a, {item(base, "base", 0, a + "/g1"), item(update, "update", 65536, a + "/u1")}},
      {b,
       {item(base, "base", 0, b + "/g1"), item(update, "update", 131072, b + "/u2"),
        item("0100BBBB00020000", "base", 0, b + "/g2"), item("", "base", 0, b + "/odd")}}};
  const auto merged = foil::mergeServers(lists);
  CHECK(merged.size() == 5);
  const auto find = [&](const std::string &url) {
    return std::find_if(merged.begin(), merged.end(), [&](const foil::Item &i) { return i.url == url; });
  };
  // The base both hold: once, from the first, naming the second.
  CHECK(find(a + "/g1") != merged.end() && find(b + "/g1") == merged.end());
  CHECK(find(a + "/g1")->server == a && find(a + "/g1")->alsoOn == (std::vector<foil::Item::Copy>{{b, b + "/g1"}}));
  // Updates of other versions are other files.
  CHECK(find(a + "/u1") != merged.end() && find(b + "/u2") != merged.end());
  CHECK(find(b + "/u2")->server == b && find(b + "/u2")->alsoOn.empty());
  CHECK(find(b + "/g2")->server == b);
  // No Title ID: kept as it is.
  CHECK(find(b + "/odd") != merged.end());
  // One server: every item names it, nothing else changes.
  const auto one = foil::mergeServers({{a, {item(base, "base", 0, a + "/g1")}}});
  CHECK(one.size() == 1 && one[0].server == a && one[0].alsoOn.empty());
  // Grouped, the merged list makes one game with its two updates.
  const auto groups = foil::groupCatalog(merged);
  const auto game = std::find_if(groups.begin(), groups.end(), [&](const foil::Group &g) { return g.id == base; });
  CHECK(game != groups.end());
  // Installing it: only the base is on both, so with the update on there is
  // nothing to choose; with the update off either server can send it.
  foil::Selection pick(*game);
  CHECK(pick.servers().size() == 1);
  for (size_t row = 0; row < pick.parts().size(); ++row)
    if (pick.parts()[row].type == "update" && pick.enabled(row)) pick.toggle(row);
  CHECK(pick.servers() == (std::vector<std::string>{a, b}));
  pick.server = b;
  CHECK(pick.files().size() == 1 && pick.files()[0].server == b && pick.files()[0].url == b + "/g1");
  pick.server.clear();
  CHECK(pick.files()[0].server == a && pick.files()[0].url == a + "/g1");
  // Homebrew by app id: the same build once naming the other server, a
  // different build kept from whichever was released later.
  const auto app = [](const std::string &id, const std::string &hash, const std::string &released) {
    foil::homebrew::App x;
    x.id = id;
    x.hash = hash;
    x.releasedAt = released;
    return x;
  };
  const auto apps = foil::homebrew::mergeServers(
      {{a, {app("same", "h1", "2026-01-01"), app("newer", "old", "2026-01-01"), app("only-a", "h3", "")}},
       {b, {app("same", "h1", "2026-01-01"), app("newer", "new", "2026-03-01"), app("only-b", "h4", "")}}});
  const auto byId = [&](const std::string &id) {
    return *std::find_if(apps.begin(), apps.end(), [&](const foil::homebrew::App &x) { return x.id == id; });
  };
  CHECK(apps.size() == 4);
  CHECK(byId("same").server == a && byId("same").alsoOn == std::vector<std::string>{b});
  CHECK(byId("newer").server == b && byId("newer").hash == "new" && byId("newer").alsoOn.empty());
  CHECK(byId("only-a").server == a && byId("only-b").server == b);
  // Patches by id the same way: the card's records know a patch by its id.
  const auto patch = [](const std::string &id, const std::string &hash, int64_t updated) {
    foil::UserPatch x;
    x.id = id;
    x.hash = hash;
    x.updatedAt = updated;
    return x;
  };
  const auto patches = foil::mergePatchServers(
      {{a, {patch("ko", "h1", 1), patch("fps", "old", 1)}}, {b, {patch("ko", "h1", 5), patch("fps", "new", 9)}}});
  CHECK(patches.size() == 2);
  CHECK(patches[0].id == "ko" && patches[0].server == a);
  CHECK(patches[1].id == "fps" && patches[1].server == b && patches[1].hash == "new");
  // Retro: one game on both once, the rest in the filter's order.
  const auto rom = [](const std::string &id, const std::string &name, int rating, const std::string &original = "") {
    foil::retro::Game g;
    g.id = id;
    g.name = name;
    g.rating = rating;
    g.original = original;
    return g;
  };
  const auto games = foil::retro::mergeGames(
      {{a, {rom("snes-1", "Alpha", 90), rom("snes-2", "Bravo", 50)}}, {b, {rom("snes-1", "Alpha", 90), rom("snes-3", "Charlie", 70)}}},
      "rating");
  CHECK(games.size() == 3);
  CHECK(games[0].id == "snes-1" && games[0].server == a && games[0].alsoOn == std::vector<std::string>{b});
  CHECK(games[1].id == "snes-3" && games[1].server == b);
  CHECK(games[2].id == "snes-2");
  // A later page joins without losing whose games they are.
  auto list = games;
  auto more = rom("snes-4", "Delta", 80);
  more.server = b;
  list.push_back(more);
  list.push_back(games[0]);
  foil::retro::orderGames(list, "rating");
  CHECK(list.size() == 4 && list[1].id == "snes-4" && list[1].server == b && list[0].server == a);
  // Home shelves: one per system, counts summed, one card per original title.
  foil::retro::Shelf sa{{}, {rom("snes-1", "Alpha", 90, "alpha"), rom("snes-2", "Bravo", 50)}};
  sa.system.id = "snes";
  sa.system.romCount = 2;
  foil::retro::Shelf sb{{}, {rom("snes-9", "Alpha (K)", 95, "alpha")}};
  sb.system.id = "snes";
  sb.system.romCount = 1;
  foil::retro::Shelf nes{{}, {rom("nes-1", "Echo", 60)}};
  nes.system.id = "nes";
  nes.system.romCount = 1;
  const auto shelves = foil::retro::mergeShelves({{a, {sa}}, {b, {sb, nes}}});
  CHECK(shelves.size() == 2 && shelves[0].system.id == "snes" && shelves[1].system.id == "nes");
  CHECK(shelves[0].system.romCount == 3);
  CHECK(shelves[0].games.size() == 2 && shelves[0].games[0].id == "snes-9" && shelves[0].games[0].server == b);
  CHECK(shelves[1].games[0].server == b);
  return failures;
}
