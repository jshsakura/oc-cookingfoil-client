// RetroAchievements: the server's answers, RetroAchievements' login, and the
// retroarch.cfg lines that sign RetroArch in without touching the rest.
#include "Test.hpp"
#include "core/Achievements.hpp"
namespace a = foil::achievements;
int runAchievementsTests() {
  int failures = 0;
  // The account: the key's state and the id, either of them missing.
  const auto account = a::parseAccount(R"({"enabled":true,"username":"player"})");
  CHECK(account.enabled && account.username == "player");
  const auto none = a::parseAccount(R"({"enabled":false,"username":null})");
  CHECK(!none.enabled && none.username.empty());
  CHECK(!a::parseAccount("not json").enabled);
  // The detail's summary: matched by hash or by title, or not at all.
  const auto summary = a::parseSummary(R"({"id":"g","achievements":{"raGameId":42,"count":12,"points":120,"matchedBy":"title"}})");
  CHECK(summary.raGameId == 42 && summary.count == 12 && summary.points == 120 && summary.byTitle);
  CHECK(a::parseSummary(R"({"id":"g"})").raGameId == 0);
  // A game's list: server paths made full, the user's progress, earned ones.
  const auto game = a::parseGame(
      R"({"raGameId":42,"title":"Comet Run","matchedBy":"hash","count":2,"points":15,
          "user":{"username":"player","earned":1,"earnedHardcore":0,"points":5},
          "list":[{"id":1,"title":"First","description":"Start","points":5,"earned":true,"earnedHardcore":false,
                   "earnedAt":1773219600,"badge":"/api/retro/achievements/badge/1.png","badgeLocked":"/api/retro/achievements/badge/1_lock.png"},
                  {"id":2,"title":"Second","points":10,"earned":false,"earnedAt":null,"badge":null},
                  {"id":3,"title":""}]})",
      "https://shop.example");
  CHECK(game.summary.raGameId == 42 && !game.summary.byTitle && game.hasUser && game.username == "player");
  CHECK(game.earned == 1 && game.userPoints == 5);
  CHECK(game.list.size() == 2); // the untitled one is left out
  if (game.list.size() == 2) {
    CHECK(game.list[0].earned && game.list[0].earnedAt == 1773219600);
    CHECK(game.list[0].badge == "https://shop.example/api/retro/achievements/badge/1.png");
    CHECK(game.list[0].badgeLocked == "https://shop.example/api/retro/achievements/badge/1_lock.png");
    CHECK(!game.list[1].earned && game.list[1].earnedAt == 0 && game.list[1].badge.empty());
  }
  CHECK(!a::parseGame(R"({"raGameId":1,"user":null,"list":[]})", "").hasUser);
  // RetroAchievements' login: the token, or its reason.
  const auto login = a::parseLogin(R"({"Success":true,"User":"Player","Token":"AAAA0000","Score":10})");
  CHECK(login.user == "Player" && login.token == "AAAA0000" && login.error.empty());
  const auto refused = a::parseLogin(R"({"Success":false,"Error":"Invalid User/Password combination. Please try again"})");
  CHECK(refused.token.empty() && !refused.error.empty());
  CHECK(a::accountBody("pla\"yer") == R"({"username":"pla\"yer"})");
  // retroarch.cfg: the lines set where they stand or added, the rest kept.
  const std::string cfg = "video_fullscreen = \"true\"\r\ncheevos_enable = \"false\"\n# cheevos_token = \"x\"\ncheevos_username = old\n";
  const auto in = a::setConfig(cfg, a::signInValues("Player", "AAAA0000"));
  CHECK(a::configValue(in, "video_fullscreen") == "true");
  CHECK(a::configValue(in, "cheevos_enable") == "true");
  CHECK(a::configValue(in, "cheevos_username") == "Player");
  CHECK(a::configValue(in, "cheevos_token") == "AAAA0000");
  CHECK(in.find("# cheevos_token = \"x\"") != std::string::npos); // a comment stays a comment
  CHECK(in.find('\r') == std::string::npos);
  CHECK(a::signedInUser(in) == "Player");
  // Set twice, nothing grows: each key once.
  const auto again = a::setConfig(in, a::signInValues("Player", "AAAA0000"));
  CHECK(again == in);
  const auto out = a::setConfig(in, a::signOutValues());
  CHECK(a::signedInUser(out).empty() && a::configValue(out, "cheevos_token").empty());
  CHECK(a::configValue(out, "cheevos_username") == "Player"); // the id stays filled in
  // A missing file: the lines alone. A quote in a value cannot break the line.
  CHECK(a::signedInUser(a::setConfig("", a::signInValues("a\"b", "T"))) == "ab");
  CHECK(a::signedInUser("").empty());
  return failures;
}
