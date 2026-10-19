#pragma once
// RetroAchievements: the account the server keeps (its id only), a game's
// achievements as the server matched them, the login RetroAchievements
// answers, and the three lines of retroarch.cfg that sign RetroArch in.
#include "core/Types.hpp"
#include <cstdint>
#include <map>
#include <string>
#include <vector>
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
} // namespace foil::achievements
