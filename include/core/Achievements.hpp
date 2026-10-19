#pragma once
// RetroAchievements: the account the server keeps (its id only), a game's
// achievements as the server matched them, the login RetroAchievements
// answers, and the three lines of retroarch.cfg that sign RetroArch in.
#include "core/Types.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>
struct json_object;
namespace foil::achievements {
// GET/PUT /api/retro/achievements/account. `enabled`: the server has a key.
struct Account {
  bool enabled = false;
  std::string username; // empty when none is registered
};
Account parseAccount(const std::string &json);
// The summary under /api/retro/games/:id, when the server matched the game.
struct Summary {
  int64_t raGameId = 0;
  int count = 0, points = 0;
  bool byTitle = false; // matched by the title, not the ROM's hash
};
// The detail's "achievements" object; raGameId 0 when there is none.
Summary parseSummary(const std::string &detailJson);
// The same object where a list sends it on each game (null: none).
Summary parseSummary(json_object *object);
struct Achievement {
  int64_t id = 0;
  std::string title, description;
  int points = 0;
  bool earned = false, earnedHardcore = false;
  int64_t earnedAt = 0; // Unix seconds, 0 when not earned
  std::string badge, badgeLocked; // full addresses, empty when none
};
// GET /api/retro/games/:id/achievements.
struct Game {
  Summary summary;
  std::string title;
  bool hasUser = false; // a RetroAchievements id is registered
  std::string username;
  int earned = 0, earnedHardcore = 0, userPoints = 0;
  std::vector<Achievement> list;
};
Game parseGame(const std::string &json, const std::string &base);
// GET /api/retro/achievements/games: the server's retro games that have
// achievements, with the player's progress (all 0 without a registered id).
struct GameRow {
  std::string gameId, title, system, icon; // icon: a full address, empty when none
  int64_t raGameId = 0;
  int count = 0, points = 0, earned = 0, earnedHardcore = 0;
  bool byTitle = false;     // matched by the title: the console's ROM may not earn them
  int64_t lastEarnedAt = 0; // Unix seconds, 0 when nothing is earned
};
struct Games {
  bool hasUser = false;
  std::string username;
  int userPoints = 0;
  std::vector<GameRow> list;
};
Games parseGames(const std::string &json, const std::string &base);
// Unix seconds as the console's local date, "2026-10-19".
std::string localDay(int64_t at);
// RetroAchievements' dorequest.php?r=login2 answer: the token, or why not.
struct Login {
  std::string user, token, error;
};
Login parseLogin(const std::string &json);
// The PUT body: {"username": "..."}.
std::string accountBody(const std::string &username);
// retroarch.cfg lines `key = "value"`: the text with each key set (replaced
// where it stands, else added at the end), every other line kept.
std::string setConfig(const std::string &text, const std::map<std::string, std::string> &values);
// A key's value, "" when it is missing.
std::string configValue(const std::string &text, const std::string &key);
// Where RetroArch keeps its settings on the card.
constexpr const char *ConfigPath = "retroarch/retroarch.cfg";
// What retroarch.cfg says of the sign-in: its user, when a token is there too.
std::string signedInUser(const std::string &configText);
// The values that sign RetroArch in, and the ones that sign it out.
std::map<std::string, std::string> signInValues(const std::string &user, const std::string &token);
std::map<std::string, std::string> signOutValues();

// The phone sign-in (/api/retro/achievements/link): the console shows a QR
// with its one-time public key, the phone seals the id and password to it,
// the server only carries the envelope and cannot open it.
struct LinkStart {
  std::string code, path; // path: "/ra-link?c=<code>"
  int64_t expiresAt = 0;  // Unix seconds
};
LinkStart parseLinkStart(const std::string &json);
// The QR: the server the console is on, the path, and the key after # so the
// page reads it in the phone and the server never sees it.
std::string linkUrl(const std::string &server, const std::string &path, const std::string &publicKey);
struct Sealed {
  std::string epk, iv, ct; // base64url as they came
};
Sealed parseSealed(const std::string &json);
// The base64url (or plain base64) bytes; "" when it is not base64.
std::string decodeBase64Url(const std::string &text);
// HKDF-SHA256 (RFC 5869) with no salt, 32 bytes out.
std::string hkdf(const std::string &ikm, const std::string &info);
struct Credentials {
  std::string user, password;
};
// Opens the envelope with the console's secret: ECDH with the phone's key,
// HKDF, AES-256-GCM with the code as additional data. An error when anything
// does not fit, which is also what a swapped or altered envelope gives.
Parsed<Credentials> openEnvelope(const std::string &secret, const Sealed &sealed, const std::string &code);
// POST .../result: {"ok":true,"username":...}, or {"ok":false} with the
// reason when there is one.
std::string linkResultBody(bool ok, const std::string &username, const std::string &reason = "");
// Its answer: "done", "wrong" (the phone asks again) or "failed".
std::string parseLinkState(const std::string &json);
} // namespace foil::achievements
