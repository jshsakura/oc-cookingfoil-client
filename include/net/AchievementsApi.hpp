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
                                                     const std::string &username,
                                                     const std::atomic<bool> *cancel = nullptr);
// A game's achievements; `missing` when the server matched none.
Parsed<achievements::Game> gameAchievements(const Server &server, const State &state, const std::string &gameId,
                                            bool &missing, const std::atomic<bool> *cancel);
// The retro games with achievements and the player's progress; `old` when
// the server has no such list yet.
Parsed<achievements::Games> achievementGames(const Server &server, const State &state, bool &old,
                                             const std::atomic<bool> *cancel);
// RetroAchievements' login with the password: the token, or the reason.
// A refusal (RetroAchievements answered and said no, a wrong password) has
// its reason in the value's error too; a failed connection, an error page or
// any answer without RetroAchievements' own reason does not.
Parsed<achievements::Login> signInRetroAchievements(const std::string &user, const std::string &password,
                                                    const std::atomic<bool> *cancel = nullptr);
// The phone sign-in: a link for the console's public key (base64url); `old`
// when the server has no such link (an older server).
Parsed<achievements::LinkStart> startAchievementsLink(const Server &server, const State &state,
                                                      const std::string &publicKey, bool &old);
// Waits up to 25 seconds for the phone's envelope. `waiting` when nothing
// came yet, `gone` when the link expired or was replaced.
struct SealedWait {
  achievements::Sealed sealed;
  bool waiting = false, gone = false;
};
Parsed<SealedWait> waitSealed(const Server &server, const State &state, const std::string &code,
                              const std::atomic<bool> *cancel);
// Tells the server how the sign-in went; `reason` "unreadable" when the
// envelope did not open (the phone asks again without using up a try).
// Its answer: "done", "wrong" or "failed".
Parsed<std::string> sendLinkResult(const Server &server, const State &state, const std::string &code, bool ok,
                                   const std::string &username, const std::string &reason,
                                   const std::atomic<bool> *cancel);
} // namespace foil::net
