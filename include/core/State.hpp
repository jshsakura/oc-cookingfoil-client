#pragma once
#include "core/Types.hpp"
#include <functional>
#include <map>
#include <optional>
#include <set>
namespace foil {
struct State {
    std::string deviceKey, lastServer, lastScreen = "home";
    std::string language; // chosen in 설정; empty follows the console
    // Install content whose signature does not check out (needs sigpatches).
    bool skipSignatureCheck = true;
    // Install games that need newer firmware than the console has.
    // Send the log to the server on its own when an error shows.
    // Install a HOME Menu shortcut with each retro game and homebrew app.
    bool makeShortcuts = true;
    // The app's sounds (start, cursor, queue, done, error), 설정's 효과음.
    bool sounds = true;
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
    // Per server address: the server's id (X-CookingFoil-Server-Id), so an
    // address with no key can take the one another address of it holds.
    std::map<std::string, std::string> serverIds;
    // The key made from this console (DeviceIdentity::deviceKey) this run;
    // not stored. Empty when the console's ID could not be read: then the
    // stored deviceKey is the one used.
    std::string derivedKey;
    // Servers that took the made key in place of the stored one (POST
    // /api/pair/rebind) while the stored one is still another; stored and
    // sealed, gone once deviceKey becomes the made key.
    std::set<std::string> rebound;
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
    std::string deviceKey;   // 64 uppercase hex: the key servers know this console by
};
// From spl's DeviceId; 0 (not read) gives an empty identity.
DeviceIdentity deriveIdentity(uint64_t deviceId);
// The key to send `server`: the made one once that server took it (or once
// it is the stored one), else the stored one.
std::string deviceKeyFor(const State& state, const std::string& server);
// `server` still knows the console by the stored key and the made one is
// another: it is asked to move (POST /api/pair/rebind) before anything else.
bool needsRebind(const State& state, const std::string& server);
// Once every one of `servers` took the made key, it becomes the stored one.
// True when that changed `state` (to be saved).
bool settleDeviceKey(State& state, const std::vector<std::string>& servers);
// The servers in config.json, for settleDeviceKey at each load and rebind.
// Until set (or once unset with nullopt), the stored key never changes.
void setKnownServers(std::optional<std::vector<std::string>> servers);
std::optional<std::vector<std::string>> knownServers();
// Where the identity comes from; asked again whenever it gave nothing, so a
// read that failed once does not last the whole run.
void setIdentitySource(std::function<DeviceIdentity()> source);
void setDeviceIdentity(DeviceIdentity identity); // a fixed one (tests)
// Why the last load made a new device key, said once: the card came from
// another console, the credentials were edited, or the records were lost.
enum class Rebound { None, Moved, Changed, Recreated };
Rebound takeRebound();
// The state in cookingfoil.db at `dbPath`; a state.json beside it from
// earlier builds moves in once (its device key kept) and goes. Credentials
// that are not this console's, or do not match their seal, give way to a new
// device key; settings stay.
// `busy`: set when the file was held the whole time it waited (another
// write still going), not a fault of the file; reading again later works.
Parsed<State> loadState(const std::string& dbPath, bool* busy = nullptr);
// 32 random bytes as 64 lowercase hex characters; empty when the console
// gave none.
std::string randomKey();
// An install that has not finished, kept in state.json's queue so it can be
// picked up again after the app closed or the install failed.
struct PendingInstall {
    std::string name, icon, target;
    std::vector<Item> files;
    // The configured server it is downloaded from (its url); empty in rows
    // from before there could be more than one: the one in use.
    std::string server;
};
std::string serializePending(const PendingInstall& pending); // "[]" when there is none
std::optional<PendingInstall> parsePending(const std::string& queueJson);
bool saveState(const std::string& dbPath, const State& state, Error& error);
// Only what the player sets (settings, approval notes, the queue): the keys
// stay as stored. For the screen's saves, whose copy of the keys may be
// older than what a connection saved meanwhile.
bool saveSettings(const std::string& dbPath, const State& state, Error& error);
// The Dropbox link (its tokens, as JSON), kept in cookingfoil.db encrypted
// under the console's key beside the other credentials. Nothing when there
// is none, or when it cannot be read here: another console's card, an
// edited record, or the console's ID not read this run.
std::optional<std::string> loadDropbox(const std::string& dbPath);
// Saves it (empty `json`: forgets it). Fails without the console's ID, or
// when the stored credentials are not this console's.
bool saveDropbox(const std::string& dbPath, const std::string& json, Error& error);
}
