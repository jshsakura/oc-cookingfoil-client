#pragma once
// 잘 돼요 / 안 돼요 on homebrew, retro games, patches and saves: the counts a
// list carries, and this console's own vote.
#include "core/Types.hpp"
#include <string>
struct json_object;
namespace foil::votes {
struct Votes {
  int up = 0, down = 0;
  std::string mine; // "up", "down" or "" (none)
  bool offered = false; // the server counts votes on it (an older one sends none)
};
// An item's "votes" {up, down} and "my_vote"; zero when the server sends none.
Votes fromItem(json_object *item);
// The answer to a vote: {up, down, mine}.
Votes parseAnswer(const std::string &json);
// The body for a vote: pressing the one already cast takes it back.
std::string body(const Votes &now, const std::string &pressed);
} // namespace foil::votes
