#pragma once
// Patches on the SD card that no record of this app accounts for: the
// user's own, or ones an earlier install of the app put there before its
// folder (and the ledger in it) was deleted. Which patch they are cannot be
// told, but the game can (its Title ID names the folder) and so can what
// they change. Read only: nothing here touches the files.
#include "core/Patches.hpp"
#include <map>
#include <string>
#include <vector>
namespace foil {
struct CardPatch {
  std::string titleId;            // the base game's, upper case
  std::vector<std::string> kinds; // romfs, exefs, cheats, fpslocker; in that order
  size_t files = 0;
  uint64_t bytes = 0;
};
// Looks below atmosphere/contents/<TID>/{romfs,exefs,cheats} and
// SaltySD/plugins/FPSLocker/patches/<TID> for games' Title IDs only (never a
// system module's folder), leaving out each file a record lists. One entry
// per game, by Title ID.
std::vector<CardPatch> scanCardPatches(const std::string &sdRoot, const std::map<std::string, PatchRecord> &records);
// The server's patch a card patch most likely is (one this app put there
// before its records were lost): the only one for the game with as many
// files, or the game's only patch. Several that fit, or none: nullptr, and
// it stays unknown rather than misnamed.
const UserPatch *guessCardPatch(const CardPatch &card, const std::vector<UserPatch> &server);
} // namespace foil
