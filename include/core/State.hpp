#pragma once
#include "core/Types.hpp"
#include <map>
#include <optional>
namespace foil {
struct State {
    std::string deviceKey, lastServer, lastScreen = "home";
    std::string language; // chosen in 설정; empty follows the console
    // Install content whose signature does not check out (needs sigpatches).
    bool skipSignatureCheck = true;
    // Install games that need newer firmware than the console has.
    bool skipFirmwareCheck = true;
    // Send the log to the server on its own when an error shows.
    // Install a HOME Menu shortcut with each retro game and homebrew app.
    bool makeShortcuts = true;
    // "sd" or "nand", chosen in 설정; empty follows config.json's installTarget.
    std::string installTarget;
    std::map<std::string, std::string> accessKeys;
    // Per server: the note sent with the approval request so the admin knows
    // which device asks. Absent until the player types one.
    std::map<std::string, std::string> pairMessages;
    std::string queueJson = "[]";
};
Parsed<State> loadState(const std::string& path);
// An install that has not finished, kept in state.json's queue so it can be
// picked up again after the app closed or the install failed.
struct PendingInstall {
    std::string name, icon, target;
    std::vector<Item> files;
};
std::string serializePending(const PendingInstall& pending); // "[]" when there is none
std::optional<PendingInstall> parsePending(const std::string& queueJson);
bool saveState(const std::string& path, const State& state, Error& error);
}
