// 세이브 > 드롭박스's list: installed games and games with Dropbox backups.
#include "Test.hpp"
#include "ui/DropboxRows.hpp"
namespace {
foil::InstalledGame installed(uint64_t id, const std::string &title, const std::string &name) {
  foil::InstalledGame game;
  game.applicationId = id;
  game.titleId = title;
  game.name = name;
  return game;
}
} // namespace
int runDropboxRowsTests() {
  int failures = 0;
  const std::vector<foil::InstalledGame> games = {installed(0x0100000000002000ULL, "0100000000002000", "Zephyr Trail"),
                                                  installed(0x0100000000001000ULL, "0100000000001000", "amber Fields"),
                                                  installed(0x0100000000003000ULL, "0100000000003000", "Comet Run")};
  // Comet Run is installed and on Dropbox; two more are on Dropbox only,
  // one the shop knows and one it does not; an empty folder lists nothing.
  const foil::net::DropboxRevisions remote = {
      {0x0100000000003000ULL, {{"20260311_090000", 1773219600, 10}}},
      {0x0100000000004000ULL, {{"20260312_090000", 1773306000, 20}, {"20260301_120000", 1772366400, 30}}},
      {0x0100000000005000ULL, {{"20260313_090000", 1773392400, 40}}},
      {0x0100000000006000ULL, {}}};
  foil::Group shop;
  shop.id = "0100000000004000";
  shop.representative.name = "Bramble Keep";
  shop.representative.icon = "http://fixture/bramble.png";
  const foil::ui::GroupLookup lookup = [&](const std::string &id) -> const foil::Group * {
    return id == shop.id ? &shop : nullptr;
  };
  const auto rows = foil::ui::dropboxRows(games, remote, lookup);
  // The union, each game once: three installed plus two on Dropbox only.
  CHECK(rows.size() == 5);
  if (rows.size() == 5) {
    // Games with Dropbox backups first, then by name (case aside).
    CHECK(rows[0].name == "0100000000005000"); // not in the catalog: its title id
    CHECK(rows[1].name == "Bramble Keep");
    CHECK(rows[2].name == "Comet Run");
    CHECK(rows[3].name == "amber Fields");
    CHECK(rows[4].name == "Zephyr Trail");
    CHECK(!rows[0].installed && rows[0].game == -1 && rows[0].remote == 1 && rows[0].icon.empty());
    CHECK(!rows[1].installed && rows[1].remote == 2 && rows[1].icon == "http://fixture/bramble.png");
    CHECK(rows[1].titleId == "0100000000004000" && rows[1].applicationId == 0x0100000000004000ULL);
    // Installed and on Dropbox: one installed row that counts the backups.
    CHECK(rows[2].installed && rows[2].game == 2 && rows[2].remote == 1);
    CHECK(rows[3].installed && rows[3].game == 1 && rows[3].remote == 0);
    CHECK(rows[4].installed && rows[4].game == 0);
  }
  // Search keeps names and title ids that match, either kind of game.
  const auto bramble = foil::ui::dropboxRows(games, remote, lookup, "bramble");
  CHECK(bramble.size() == 1 && !bramble.front().installed);
  const auto byId = foil::ui::dropboxRows(games, remote, lookup, "3000");
  CHECK(byId.size() == 1 && byId.front().name == "Comet Run");
  CHECK(foil::ui::dropboxRows(games, remote, lookup, "nothing like it").empty());
  // No catalog at all: names fall back to title ids.
  const auto bare = foil::ui::dropboxRows({}, remote, nullptr);
  CHECK(bare.size() == 3);
  for (const auto &row : bare) CHECK(row.name == row.titleId && !row.installed);
  // No Dropbox list yet: the installed games alone, by name.
  const auto local = foil::ui::dropboxRows(games, {}, lookup);
  CHECK(local.size() == 3 && local[0].name == "amber Fields" && local[2].name == "Zephyr Trail");
  // Y's views. Latest: only what is on Dropbox, newest backup first, the
  // installed game too when it has backups there.
  const auto latest = foil::ui::dropboxRows(games, remote, lookup, "", foil::ui::DropboxView::Latest);
  CHECK(latest.size() == 3);
  if (latest.size() == 3) {
    CHECK(latest[0].name == "0100000000005000"); // 03-13
    CHECK(latest[1].name == "Bramble Keep");     // 03-12
    CHECK(latest[2].name == "Comet Run" && latest[2].installed && latest[2].newest == 1773219600);
  }
  // Name: the same games by name.
  const auto named = foil::ui::dropboxRows(games, remote, lookup, "", foil::ui::DropboxView::Name);
  CHECK(named.size() == 3 && named[1].name == "Bramble Keep" && named[2].name == "Comet Run");
  // Nothing on Dropbox: those views list nothing, whatever is installed.
  CHECK(foil::ui::dropboxRows(games, {}, lookup, "", foil::ui::DropboxView::Latest).empty());
  // With this console: installed games that have a save or backup are added,
  // the rest left out; Dropbox's own stay whatever the console holds.
  const auto withConsole = foil::ui::dropboxRows(games, remote, lookup, "", foil::ui::DropboxView::WithConsole,
                                                 [](uint64_t id) { return id == 0x0100000000002000ULL; });
  CHECK(withConsole.size() == 4);
  if (withConsole.size() == 4) {
    CHECK(withConsole[2].name == "Comet Run"); // no save, but on Dropbox
    CHECK(withConsole[3].name == "Zephyr Trail" && withConsole[3].remote == 0);
  }
  return failures;
}
