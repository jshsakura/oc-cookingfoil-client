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
    // Per server: the secret sent with the approval request and every status
    // check, so only this console can take the key it is approved for (64
    // hex characters). Made before the first request, dropped once the key
    // came.
    std::map<std::string, std::string> pairSecrets;
    // An install state.json held from before 다운로드 kept the queue; put in
    // the queue once, then "[]".
    std::string queueJson = "[]";
};
// The console this card is in. Both are derived from a value only the
// console has (platform/Identity), which is kept nowhere: the fingerprint
// is stored to tell another console, the seal key signs the credentials so
// an edited file is told too. Empty when the console gave nothing.
struct DeviceIdentity {
    std::string fingerprint; // hex, stored
    std::string sealKey;     // raw bytes, never stored
};
DeviceIdentity deriveIdentity(const std::string& consoleValue);
void setDeviceIdentity(DeviceIdentity identity);
// Why the last load made a new device key, said once: the card came from
// another console, the credentials were edited, or the records were lost.
enum class Rebound { None, Moved, Changed, Recreated };
Rebound takeRebound();
// The state in cookingfoil.db at `dbPath`; a state.json beside it from
// earlier builds moves in once (its device key kept) and goes. Credentials
// that are not this console's, or do not match their seal, give way to a new
// device key; settings stay.
Parsed<State> loadState(const std::string& dbPath);
// 32 random bytes as 64 lowercase hex characters; empty when the console
// gave none.
std::string randomKey();
// An install that has not finished, kept in state.json's queue so it can be
// picked up again after the app closed or the install failed.
struct PendingInstall {
    std::string name, icon, target;
    std::vector<Item> files;
};
std::string serializePending(const PendingInstall& pending); // "[]" when there is none
std::optional<PendingInstall> parsePending(const std::string& queueJson);
bool saveState(const std::string& dbPath, const State& state, Error& error);
}
