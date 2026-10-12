#include "Test.hpp"
#include "core/Installed.hpp"
#include "platform/Installed.hpp"
int runInstalledTests() {
  int failures = 0;
  {
    foil::InstalledGame a, b, c;
    a.name = "Cloud Garden"; a.bytes = 900; a.lastUpdated = 3;
    b.name = "Astral"; b.bytes = 100; b.lastUpdated = 9;
    c.name = "Brick"; c.bytes = 900; c.lastUpdated = 1;
    const std::vector<foil::InstalledGame> games{a, b, c};
    auto names = [](const std::vector<foil::InstalledGame> &list) {
      std::string joined;
      for (const auto &g : list) joined += g.name.substr(0, 1);
      return joined;
    };
    CHECK(names(foil::sortInstalled(games, foil::InstalledSort::Name)) == "ABC");
    // Equal sizes fall back to name order.
    CHECK(names(foil::sortInstalled(games, foil::InstalledSort::Size)) == "BCA");
    CHECK(names(foil::sortInstalled(games, foil::InstalledSort::Recent)) == "ACB");
    CHECK(names(games) == "CAB");
  }
  constexpr uint64_t id = 0x0100E5E01C098000ULL;
  foil::InstalledGame game;
  game.applicationId = id;
  game.titleId = "0100E5E01C098000";
  foil::InstalledLibrary library{{true, {{game.titleId, 0}}}, {game}};
  foil::SaveEntry save;
  save.applicationId = id;
  save.saveId = 42;
  save.user = {1, 2};
  const foil::RemovalRequest gameRequest{foil::RemovalKind::Game, id, {}};
  const foil::RemovalRequest saveRequest{foil::RemovalKind::Save, id, save};
  CHECK(!foil::validateRemoval(gameRequest, library, {}, id + 0x1000, true, false));
  CHECK(!foil::validateRemoval(saveRequest, library, {save}, id + 0x1000, true, false));
  CHECK(foil::validateRemoval(gameRequest, library, {}, id, true, false));
  CHECK(foil::validateRemoval(saveRequest, library, {save}, id, true, false));
  CHECK(foil::validateRemoval(gameRequest, library, {}, id + 0x1000, false, false));
  CHECK(foil::validateRemoval(gameRequest, library, {}, id + 0x1000, true, true));
  CHECK(foil::validateRemoval(gameRequest, {}, {}, id + 0x1000, true, false));
  CHECK(foil::validateRemoval(gameRequest, library, {}, 0, true, false));
  CHECK(foil::validateRemoval(saveRequest, library, {}, id + 0x1000, true, false));
  auto stale = save;
  stale.user[1]++;
  CHECK(foil::validateRemoval(saveRequest, library, {stale}, id + 0x1000, true, false));
  stale = save;
  stale.applicationId += 0x1000;
  CHECK(foil::validateRemoval(saveRequest, library, {stale}, id + 0x1000, true, false));
  for (const int type : {0, 2, 4, 5, 6}) {
    auto invalid = saveRequest;
    invalid.save.type = type;
    CHECK(foil::validateRemoval(invalid, library, {invalid.save}, id + 0x1000, true, false));
  }
  auto invalid = saveRequest;
  invalid.save.user = {};
  CHECK(foil::validateRemoval(invalid, library, {invalid.save}, id + 0x1000, true, false));
  invalid = saveRequest;
  invalid.save.space = 0;
  CHECK(foil::validateRemoval(invalid, library, {invalid.save}, id + 0x1000, true, false));
  invalid = saveRequest;
  invalid.save.rank = 1;
  CHECK(foil::validateRemoval(invalid, library, {invalid.save}, id + 0x1000, true, false));
  auto shared = saveRequest;
  shared.save.type = 3; shared.save.user = {};
  CHECK(!foil::validateRemoval(shared, library, {shared.save}, id + 0x1000, true, false));
  shared.save.user = {1, 2};
  CHECK(foil::validateRemoval(shared, library, {shared.save}, id + 0x1000, true, false));
  CHECK(foil::platform::readInstalled().error);
  CHECK(foil::platform::readSaves(id).error);
  CHECK(foil::platform::removeInstalled(gameRequest));
  {
    // Only content no record points to is unused; no records at all means
    // the read failed, never "delete everything".
    const auto orphans = foil::orphanContents({"a", "b", "c"}, {"a", "c", "z"});
    CHECK(orphans && orphans->size() == 1 && orphans->front() == "b");
    CHECK(!foil::orphanContents({"a"}, {}));
    CHECK(foil::orphanContents({}, {}) && foil::orphanContents({}, {})->empty());
  }
  return failures;
}
