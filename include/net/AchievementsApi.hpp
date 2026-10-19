#pragma once
// RetroAchievements through the shop (/api/retro/achievements, the id it
// keeps and a game's achievements) and the console's own sign-in with
// RetroAchievements, whose token goes into retroarch.cfg and nowhere else.
#include "core/Achievements.hpp"
#include "core/State.hpp"
#include <atomic>
namespace foil::net {
Parsed<achievements::Account> achievementsAccount(const Server &server, const State &state);
// Registers `username` (the server checks RetroAchievements has it), or
// with an empty one removes it.
Parsed<achievements::Account> setAchievementsAccount(const Server &server, const State &state,
                                                     const std::string &username);
// A game's achievements; `missing` when the server matched none.
Parsed<achievements::Game> gameAchievements(const Server &server, const State &state, const std::string &gameId,
                                            bool &missing, const std::atomic<bool> *cancel);
// RetroAchievements' login with the password: the token, or the reason.
Parsed<achievements::Login> signInRetroAchievements(const std::string &user, const std::string &password);
} // namespace foil::net
