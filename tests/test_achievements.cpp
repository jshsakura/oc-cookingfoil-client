// RetroAchievements: the server's answers, RetroAchievements' login, and the
// retroarch.cfg lines that sign RetroArch in without touching the rest.
#include "Test.hpp"
#include "core/Achievements.hpp"
#include "core/Sha256.hpp"
#include "platform/Seal.hpp"
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
  // The games with achievements: progress clamped, a title match marked,
  // rows without an id or a name left out, the icon made absolute.
  const auto games = a::parseGames(R"({"user":{"username":"player","points":420},"list":[
    {"gameId":"g1","title":"Alpha","system":"snes","raGameId":12,"count":40,"points":400,"earned":45,"earnedHardcore":50,
     "matchedBy":"hash","lastEarnedAt":1760000000,"iconUrl":"/media/g1.png"},
    {"gameId":"g2","title":"Beta","raGameId":13,"count":10,"points":100,"earned":0,"matchedBy":"title","lastEarnedAt":null},
    {"gameId":"","title":"No id"},{"gameId":"g4"}]})", "http://192.0.2.10:8080");
  CHECK(games.hasUser && games.username == "player" && games.userPoints == 420 && games.list.size() == 2);
  if (games.list.size() == 2) {
    CHECK(games.list[0].earned == 40 && games.list[0].earnedHardcore == 40 && !games.list[0].byTitle);
    CHECK(games.list[0].icon == "http://192.0.2.10:8080/media/g1.png" && games.list[0].lastEarnedAt == 1760000000);
    CHECK(games.list[1].byTitle && games.list[1].lastEarnedAt == 0 && games.list[1].icon.empty());
  }
  CHECK(!a::parseGames(R"({"user":null,"list":[]})", "").hasUser);
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
  // The phone sign-in. A vector sealed by Python's cryptography (the same
  // primitives as the phone page): device scalar 0x11.., phone scalar 0x22..
  const std::string secret(32, '\x11'), code = "K7Q2-M9XA";
  const a::Sealed sealed{"BNZak5d8qj0bCBhS_1ennkZfFmBXcwS66tUF3TpIWJzzUBheiVNy32Ih6joTdVfkc_3bZ1XwW9UHw8Uz_OnJEoU", // privacy-ok: fixed test vector
                         "AAECAwQFBgcICQoL", "OSdgxFURo_dNP8QDVcAuAHQVpJKXQMIKLZtWIsdtJSyWxZ3M0x90PMsncdb0XvhN"}; // privacy-ok: fixed test vector
  CHECK(foil::seal::sharedX(secret, a::decodeBase64Url(sealed.epk)) ==
        foil::hexBytes("ccfc261f58193c98ca4ad4a53bbac6f0ee29bc4d48438090446908622ca79af6")); // privacy-ok: fixed test vector
  CHECK(a::hkdf(foil::hexBytes("ccfc261f58193c98ca4ad4a53bbac6f0ee29bc4d48438090446908622ca79af6"), "cookingfoil-ra-link") == // privacy-ok: fixed test vector
        foil::hexBytes("5b2c5cd4768737fd4335d8fb975f7644de9c6a360eca494865b37a02cdd6d513")); // privacy-ok: fixed test vector
  const auto opened = a::openEnvelope(secret, sealed, code);
  CHECK(!opened.error && opened.value.user == "player" && opened.value.password == "pa ss\"word");
  // Another link's code, another key, or one changed byte: nothing opens.
  CHECK(a::openEnvelope(secret, sealed, "K7Q2-M9XB").error);
  CHECK(a::openEnvelope(std::string(32, '\x12'), sealed, code).error);
  auto altered = sealed;
  altered.ct[5] = altered.ct[5] == 'A' ? 'B' : 'A';
  CHECK(a::openEnvelope(secret, altered, code).error);
  CHECK(a::openEnvelope(secret, {"not a point", sealed.iv, sealed.ct}, code).error);
  // A fresh key: a real point, and both sides reach the same secret.
  foil::seal::Key one, two;
  CHECK(foil::seal::generate(one) && foil::seal::generate(two));
  CHECK(one.point.size() == 65 && one.point[0] == '\x04' && one.secret.size() == 32 && one.secret != two.secret);
  CHECK(foil::seal::sharedX(one.secret, two.point) == foil::seal::sharedX(two.secret, one.point));
  CHECK(foil::seal::sharedX(one.secret, std::string(65, '\x04')).empty()); // off the curve
  // base64url, padded or not; a stray character is not base64.
  CHECK(a::decodeBase64Url("AAECAwQFBgcICQoL") == std::string("\0\1\2\3\4\5\6\7\x08\t\n\x0b", 12));
  CHECK(a::decodeBase64Url("_-8") == "\xff\xef" && a::decodeBase64Url("_-8=") == "\xff\xef");
  CHECK(a::decodeBase64Url("ab$d").empty() && a::decodeBase64Url("A").empty());
  // The server's answers, and the QR: the key after #, a path only on this server.
  const auto link = a::parseLinkStart(R"({"code":"K7Q2","expiresAt":1760000300,"path":"/ra-link?c=K7Q2"})");
  CHECK(link.code == "K7Q2" && link.expiresAt == 1760000300 && link.path == "/ra-link?c=K7Q2");
  CHECK(a::parseLinkStart(R"({"code":"K7Q2","path":"//example.org/x"})").path == "/ra-link?c=K7Q2");
  CHECK(a::linkUrl("http://192.0.2.10:8080/", link.path, "BAbc") == "http://192.0.2.10:8080/ra-link?c=K7Q2#k=BAbc");
  CHECK(a::linkResultBody(true, "Player") == R"({"ok":true,"username":"Player"})");
  CHECK(a::linkResultBody(false, "Player") == R"({"ok":false})");
  CHECK(a::linkResultBody(false, "", "unreadable") == R"({"ok":false,"reason":"unreadable"})");
  CHECK(a::parseLinkState(R"({"state":"wrong"})") == "wrong");
  return failures;
}
