#include "core/State.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include "core/DownloadStore.hpp"
#include "core/Sha256.hpp"
#include "core/Vault.hpp"
#include <atomic>
#include <cstdio>
#include <cctype>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <fstream>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil {
// 64 hex characters: random ones are lowercase, the ones made from the
// console uppercase (the servers' pattern).
static bool validDeviceKey(const std::string& key) {
    return key.size() == 64 && key.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos;
}
std::string randomKey() {
    unsigned char entropy[32]{};
#ifdef __SWITCH__
    randomGet(entropy, sizeof(entropy));
#else
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(entropy), sizeof(entropy))) return "";
#endif
    const char* hex = "0123456789abcdef";
    std::string key;
    for (auto byte : entropy) { key += hex[byte >> 4]; key += hex[byte & 15]; }
    return key;
}
namespace {
std::mutex identityLock;
DeviceIdentity identity;
std::function<DeviceIdentity()> identitySource;
// Credentials made or moved in while the console's ID could not be read:
// written then, they would read as another console's once it can be. Kept
// in memory for this run instead, and bound at the first load that reads it.
std::mutex serversLock;
std::optional<std::vector<std::string>> servers;
std::mutex unboundLock;
std::optional<State> unbound;
void keepUnbound(const State& state) {
    std::lock_guard<std::mutex> held(unboundLock);
    State kept;
    kept.deviceKey = state.deviceKey;
    kept.accessKeys = state.accessKeys;
    kept.pairSecrets = state.pairSecrets;
    unbound = std::move(kept);
}
std::optional<State> unboundCopy() {
    std::lock_guard<std::mutex> held(unboundLock);
    return unbound;
}
void dropUnbound() {
    std::lock_guard<std::mutex> held(unboundLock);
    unbound.reset();
}
std::atomic<int> rebound{int(Rebound::None)};
// Settings by name in the settings table; credentials by kind.
constexpr const char* DeviceKind = "device";
constexpr const char* FingerprintKind = "fingerprint";
constexpr const char* AccessKind = "access";
constexpr const char* SecretKind = "secret";
constexpr const char* SealKind = "seal";
constexpr const char* ReboundKind = "rebound"; // per server, the made key it took
constexpr const char* DropboxKind = "dropbox"; // the Dropbox link, its tokens as JSON
constexpr const char* PendingSetting = "pendingInstall";
DeviceIdentity currentIdentity() {
    std::lock_guard<std::mutex> held(identityLock);
    if (identity.fingerprint.empty() && identitySource) identity = identitySource();
    return identity;
}
// The key the credentials are encrypted under, drawn from the console's seal
// key: without the console it cannot be made, so a card read elsewhere (a
// PC, another console) shows only ciphertext. Empty with no identity.
std::string storageKey(const DeviceIdentity& id) {
    return id.sealKey.empty() ? "" : hexBytes(hmacSha256(id.sealKey, "cookingfoil-storage-v1"));
}
// Kinds whose values are secrets: kept encrypted. The fingerprint, the seal
// and the rebound marks are not secret and stay as they are.
bool secretKind(const std::string& kind) {
    return kind == DeviceKind || kind == AccessKind || kind == SecretKind || kind == DropboxKind;
}
std::string encrypt(const std::string& key, const std::string& value) {
    const auto nonce = hexBytes(randomKey()).substr(0, 12);
    return nonce.size() == 12 ? vault::seal(key, value, nonce) : "";
}
// Every secret row decrypted in place. False when one is encrypted and does
// not open under `key` (another console's, changed, or no key this run).
// `plain` tells a row from before encryption, read as it is.
bool decryptRows(DownloadStore::StateRows& rows, const std::string& key, bool& plain) {
    plain = false;
    for (auto& [at, value] : rows.credentials) {
        if (!secretKind(at.first)) continue;
        if (!vault::sealed(value)) {
            plain = plain || !value.empty();
            continue;
        }
        const auto opened = key.empty() ? std::nullopt : vault::open(key, value);
        if (!opened) return false;
        value = *opened;
    }
    return true;
}
// Every credential but the seal, in one string, in key order.
std::string sealed(const DownloadStore::StateRows& rows) {
    std::string text;
    for (const auto& [at, value] : rows.credentials)
        if (at.first != SealKind) text += at.first + '\0' + at.second + '\0' + value + '\n';
    return text;
}
std::string seal(const DeviceIdentity& id, const DownloadStore::StateRows& rows) {
    return id.sealKey.empty() ? "" : hmacSha256(id.sealKey, sealed(rows));
}
std::string flag(bool on) { return on ? "1" : "0"; }
// state.json of earlier builds, as it read there; nothing when it is not one.
std::optional<State> parseLegacy(const std::string& text) {
    Error unread;
    auto root = parseJson(text, unread);
    if (unread || !json_object_is_type(root.get(), json_type_object)) return std::nullopt;
    State state;
    state.deviceKey = stringField(root.get(), "deviceKey");
    state.lastServer = stringField(root.get(), "lastServer");
    state.lastScreen = stringField(root.get(), "lastScreen", "home");
    state.language = stringField(root.get(), "language");
    // Earlier builds saved "skipSignatureCheck": false on every save, not as
    // a choice, so only the newer keys count.
    auto* skip = field(root.get(), "ignoreSignature");
    if (json_object_is_type(skip, json_type_boolean)) state.skipSignatureCheck = json_object_get_boolean(skip);
    auto* firmware = field(root.get(), "ignoreFirmware");
    if (json_object_is_type(firmware, json_type_boolean)) state.skipFirmwareCheck = json_object_get_boolean(firmware);
    auto* shortcuts = field(root.get(), "makeShortcuts");
    if (json_object_is_type(shortcuts, json_type_boolean)) state.makeShortcuts = json_object_get_boolean(shortcuts);
    state.installTarget = stringField(root.get(), "installTarget");
    const auto byServer = [&](const char* name, std::map<std::string, std::string>& into) {
        auto* object = field(root.get(), name);
        if (!json_object_is_type(object, json_type_object)) return;
        json_object_object_foreach(object, url, value) {
            if (json_object_is_type(value, json_type_string)) into[url] = json_object_get_string(value);
        }
    };
    byServer("accessKeys", state.accessKeys);
    byServer("pairMessages", state.pairMessages);
    byServer("pairSecrets", state.pairSecrets);
    auto* queue = field(root.get(), "queue");
    if (json_object_is_type(queue, json_type_array)) state.queueJson = json_object_to_json_string_ext(queue, JSON_C_TO_STRING_PLAIN);
    return state;
}
// What a server URL, a key or a setting may be; anything else is dropped.
State cleaned(State state) {
    if (state.installTarget != "sd" && state.installTarget != "nand") state.installTarget.clear();
    if (state.lastScreen.empty()) state.lastScreen = "home";
    const auto keep = [](std::map<std::string, std::string>& by, auto&& good) {
        for (auto at = by.begin(); at != by.end();)
            at = normalizeUrl(at->first).empty() || !good(at->second) ? by.erase(at) : std::next(at);
    };
    keep(state.accessKeys, [](const std::string& v) { return !v.empty(); });
    keep(state.pairMessages, [](const std::string& v) { return !v.empty(); });
    keep(state.pairSecrets, validDeviceKey);
    if (!parsePending(state.queueJson)) state.queueJson = "[]";
    return state;
}
State fromRows(const DownloadStore::StateRows& rows) {
    State state;
    const auto setting = [&](const char* name, const std::string& otherwise = "") {
        const auto at = rows.settings.find(name);
        return at == rows.settings.end() ? otherwise : at->second;
    };
    state.language = setting("language");
    state.installTarget = setting("installTarget");
    state.lastServer = setting("lastServer");
    state.lastScreen = setting("lastScreen", "home");
    state.skipSignatureCheck = setting("ignoreSignature", "1") == "1";
    state.skipFirmwareCheck = setting("ignoreFirmware", "1") == "1";
    state.makeShortcuts = setting("makeShortcuts", "1") == "1";
    state.sounds = setting("sounds", "1") == "1";
    state.queueJson = setting(PendingSetting, "[]");
    state.pairMessages = rows.notes;
    for (const auto& [at, value] : rows.credentials) {
        if (at.first == DeviceKind) state.deviceKey = value;
        else if (at.first == AccessKind) state.accessKeys[at.second] = value;
        else if (at.first == SecretKind) state.pairSecrets[at.second] = value;
    }
    return cleaned(std::move(state));
}
DownloadStore::StateRows toRows(const State& given, const DeviceIdentity& id) {
    const auto state = cleaned(given);
    DownloadStore::StateRows rows;
    rows.settings = {{"language", state.language},
                     {"installTarget", state.installTarget},
                     {"lastServer", state.lastServer},
                     {"lastScreen", state.lastScreen},
                     {"ignoreSignature", flag(state.skipSignatureCheck)},
                     {"ignoreFirmware", flag(state.skipFirmwareCheck)},
                     {"makeShortcuts", flag(state.makeShortcuts)},
                     {"sounds", flag(state.sounds)}};
    if (state.queueJson != "[]") rows.settings[PendingSetting] = state.queueJson;
    rows.notes = state.pairMessages;
    rows.credentials[{DeviceKind, ""}] = state.deviceKey;
    rows.credentials[{FingerprintKind, ""}] = id.fingerprint;
    for (const auto& [server, key] : state.accessKeys) rows.credentials[{AccessKind, server}] = key;
    for (const auto& [server, secret] : state.pairSecrets) rows.credentials[{SecretKind, server}] = secret;
    if (!state.derivedKey.empty() && state.derivedKey != state.deviceKey)
        for (const auto& server : state.rebound) rows.credentials[{ReboundKind, server}] = state.derivedKey;
    return rows;
}
// The secret rows encrypted and the whole set sealed, as stored. With no
// identity nothing is written from these (saveState keeps what is stored).
void protect(DownloadStore::StateRows& rows, const DeviceIdentity& id) {
    const auto key = storageKey(id);
    if (!key.empty())
        for (auto& [at, value] : rows.credentials)
            if (secretKind(at.first) && !vault::sealed(value)) value = encrypt(key, value);
    rows.credentials.erase({SealKind, ""});
    rows.credentials[{SealKind, ""}] = seal(id, rows);
}
// Credentials that are not this console's: why, or None when they are.
Rebound check(const DownloadStore::StateRows& rows, const DeviceIdentity& id) {
    const auto value = [&](const char* kind) {
        const auto at = rows.credentials.find({kind, ""});
        return at == rows.credentials.end() ? std::string() : at->second;
    };
    // An encrypted one is looked at once it is opened (loadState).
    if (!vault::sealed(value(DeviceKind)) && !validDeviceKey(value(DeviceKind))) return Rebound::Changed;
    // The console gave nothing to check against this time: kept as they are
    // rather than dropped for a fault of the moment.
    if (id.fingerprint.empty()) {
        logLine("state: no console identity; credentials kept unchecked");
        return Rebound::None;
    }
    if (value(FingerprintKind) != id.fingerprint) return Rebound::Moved;
    if (value(SealKind) != seal(id, rows)) return Rebound::Changed;
    return Rebound::None;
}
// A new device key, the one made from this console when its ID was read;
// whatever servers gave the old one goes with it.
bool rekey(State& state, const DeviceIdentity& id) {
    state.deviceKey = id.deviceKey.empty() ? randomKey() : id.deviceKey;
    state.accessKeys.clear();
    state.pairSecrets.clear();
    state.rebound.clear();
    return !state.deviceKey.empty();
}
// The made key this run, and the servers that took it (a record for
// another made key is not this console's).
void attach(State& state, const DownloadStore::StateRows& rows, const DeviceIdentity& id) {
    state.derivedKey = id.deviceKey;
    state.rebound.clear();
    if (id.deviceKey.empty()) return;
    for (const auto& [at, value] : rows.credentials)
        if (at.first == ReboundKind && value == id.deviceKey) state.rebound.insert(at.second);
}
const char* reasonName(Rebound why) {
    return why == Rebound::Moved ? "another console's" : why == Rebound::Changed ? "edited" : "lost";
}
} // namespace
DeviceIdentity deriveIdentity(uint64_t deviceId) {
    if (!deviceId) return {};
    char text[32];
    std::snprintf(text, sizeof text, "device-id %016llx", static_cast<unsigned long long>(deviceId));
    const std::string consoleValue = text;
    const auto digest = [&](const char* purpose) {
        Sha256 hash;
        const std::string text = std::string("CookingFoil ") + purpose + '\0' + consoleValue;
        hash.update(text.data(), text.size());
        return hash.hex();
    };
    // The device key: HMAC-SHA256 keyed by the ID's eight bytes (little
    // endian, as the console holds it), apart from the two above.
    std::string bytes;
    for (int i = 0; i < 8; ++i) bytes += char((deviceId >> (8 * i)) & 0xff);
    auto key = hmacSha256(bytes, "cookingfoil-device-key-v1");
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return {digest("device fingerprint v1"), hexBytes(digest("state seal v1")), key};
}
bool needsRebind(const State& state, const std::string& server) {
    return !state.derivedKey.empty() && state.derivedKey != state.deviceKey && !state.rebound.count(server);
}
std::string deviceKeyFor(const State& state, const std::string& server) {
    return state.derivedKey.empty() || needsRebind(state, server) ? state.deviceKey : state.derivedKey;
}
void setKnownServers(std::optional<std::vector<std::string>> list) {
    std::lock_guard<std::mutex> held(serversLock);
    servers = std::move(list);
}
std::optional<std::vector<std::string>> knownServers() {
    std::lock_guard<std::mutex> held(serversLock);
    return servers;
}
bool settleDeviceKey(State& state, const std::vector<std::string>& servers) {
    if (state.derivedKey.empty() || state.derivedKey == state.deviceKey) return false;
    for (const auto& server : servers)
        if (needsRebind(state, server)) return false;
    state.deviceKey = state.derivedKey;
    state.rebound.clear();
    return true;
}
void setIdentitySource(std::function<DeviceIdentity()> source) {
    std::lock_guard<std::mutex> held(identityLock);
    identitySource = std::move(source);
    identity = {};
}
void setDeviceIdentity(DeviceIdentity id) {
    std::lock_guard<std::mutex> held(identityLock);
    identitySource = nullptr;
    identity = std::move(id);
}
Rebound takeRebound() { return Rebound(rebound.exchange(int(Rebound::None))); }
bool saveState(const std::string& dbPath, const State& state, Error& error) {
    DownloadStore store;
    std::string failure;
    const auto id = currentIdentity();
    auto rows = toRows(state, id);
    bool ok = store.open(dbPath, failure);
    if (ok && !id.fingerprint.empty()) {
        // The Dropbox link is saved on its own (saveDropbox): it stays, while
        // the stored rows are this console's.
        const auto stored = store.stateRows(failure);
        ok = stored.has_value();
        if (ok && check(*stored, id) == Rebound::None)
            if (const auto link = stored->credentials.find({DropboxKind, ""}); link != stored->credentials.end())
                rows.credentials[link->first] = link->second;
        if (ok) protect(rows, id);
    }
    if (ok && id.fingerprint.empty()) {
        // No console identity this run: credentials bound before stay as
        // they are, since rows written without it would read as another
        // console's at the next start. Settings are written all the same.
        const auto stored = store.stateRows(failure);
        ok = stored.has_value();
        if (ok && !stored->credentials.count({DeviceKind, ""})) {
            // None bound yet: this run's stay in memory (keepUnbound).
            keepUnbound(state);
            rows.credentials.clear();
            logLine("state: no console identity; credentials kept in memory until it can be read");
        } else if (ok) {
            const auto plain = [](auto credentials) {
                credentials.erase({FingerprintKind, ""});
                credentials.erase({SealKind, ""});
                return credentials;
            };
            if (plain(stored->credentials) != plain(rows.credentials))
                logLine("state: no console identity; new credentials kept for this run only");
            rows.credentials = stored->credentials;
        }
    }
    if (!ok || !store.saveStateRows(rows, failure)) {
        error.message = failure.empty() ? "state not saved" : failure;
        return false;
    }
    if (!id.fingerprint.empty()) dropUnbound(); // bound now
    return true;
}
bool saveSettings(const std::string& dbPath, const State& state, Error& error) {
    DownloadStore store;
    std::string failure;
    auto rows = toRows(state, currentIdentity());
    std::optional<DownloadStore::StateRows> stored;
    if (store.open(dbPath, failure)) stored = store.stateRows(failure);
    if (stored) {
        rows.credentials = stored->credentials;
        if (store.saveStateRows(rows, failure)) return true;
    }
    error.message = failure.empty() ? "settings not saved" : failure;
    return false;
}
std::optional<std::string> loadDropbox(const std::string& dbPath) {
    DownloadStore store;
    std::string failure;
    std::optional<DownloadStore::StateRows> rows;
    if (store.open(dbPath, failure)) rows = store.stateRows(failure);
    const auto id = currentIdentity();
    if (!rows || id.fingerprint.empty()) return std::nullopt;
    const auto link = rows->credentials.find({DropboxKind, ""});
    if (link == rows->credentials.end()) return std::nullopt;
    if (check(*rows, id) != Rebound::None) {
        logLine("dropbox: the stored link is not this console's; link again");
        return std::nullopt;
    }
    auto opened = vault::open(storageKey(id), link->second);
    if (!opened) logLine("dropbox: the stored link does not open with this console's key; link again");
    return opened;
}
bool saveDropbox(const std::string& dbPath, const std::string& json, Error& error) {
    DownloadStore store;
    std::string failure;
    const auto id = currentIdentity();
    const auto key = storageKey(id);
    if (key.empty()) {
        error.message = tr("Could not read this console's ID, so the Dropbox link was not saved.");
        return false;
    }
    std::optional<DownloadStore::StateRows> rows;
    if (store.open(dbPath, failure)) rows = store.stateRows(failure);
    if (!rows || check(*rows, id) != Rebound::None) {
        error.message = failure.empty() ? tr("Could not read the app's records on the SD card.") : failure;
        return false;
    }
    if (json.empty()) rows->credentials.erase({DropboxKind, ""});
    else {
        const auto sealedLink = encrypt(key, json);
        if (sealedLink.empty()) { error.message = tr("Could not read the app's records on the SD card."); return false; }
        rows->credentials[{DropboxKind, ""}] = sealedLink;
    }
    protect(*rows, id);
    if (!store.saveStateRows(*rows, failure)) { error.message = failure; return false; }
    return true;
}
Parsed<State> loadState(const std::string& dbPath, bool* busy) {
    Parsed<State> result;
    DownloadStore store;
    std::string failure;
    std::optional<DownloadStore::StateRows> rows;
    if (store.open(dbPath, failure)) rows = store.stateRows(failure);
    if (!rows) {
        logLine("state: " + failure);
        if (busy) *busy = DownloadStore::busy(failure);
        result.error.message = tr("Could not read the app's records on the SD card.");
        return result;
    }
    const bool setAside = DownloadStore::takeSetAside();
    const auto id = currentIdentity();
    const auto legacy = (std::filesystem::path(dbPath).parent_path() / "state.json").string();
    std::error_code ec;
    Rebound why = Rebound::None;
    State state;
    bool moved = false; // state.json read in; it goes once the state is saved
    if (rows->credentials.count({DeviceKind, ""})) {
        why = check(*rows, id);
        auto opened = *rows;
        bool plain = false;
        const bool readable = decryptRows(opened, storageKey(id), plain);
        if (why == Rebound::None && !readable && id.fingerprint.empty()) {
            // Encrypted, and the console's ID could not be read this run:
            // the settings go on, the keys wait for a load that can open them.
            logLine("state: no console identity; the encrypted credentials wait for it");
            opened.credentials.clear();
            result.value = fromRows(opened);
            return result;
        }
        if (why == Rebound::None && !readable) why = Rebound::Changed;
        if (why == Rebound::None && !validDeviceKey(opened.credentials[{DeviceKind, ""}])) why = Rebound::Changed;
        if (!readable) opened.credentials.clear(); // the settings stay; the keys are made again below
        state = fromRows(opened);
        attach(state, *rows, id);
        if (why == Rebound::None && plain && !storageKey(id).empty()) {
            // Written before encryption: encrypted now, once.
            Error saved;
            if (saveState(dbPath, state, saved)) logLine("state: credentials encrypted with the console's key");
            else logLine("state: " + saved.message);
        }
        if (why == Rebound::None) {
            // Every server took the made key: it is the stored one from now.
            if (const auto list = knownServers(); list && settleDeviceKey(state, *list)) {
                Error saved;
                if (saveState(dbPath, state, saved)) logLine("state: every server took the console's key; stored");
                else logLine("state: " + saved.message);
            }
            result.value = std::move(state);
            return result;
        }
    } else if (auto kept = unboundCopy()) {
        // Made earlier this run without the console's ID: the same key goes
        // on, and is bound below once the ID can be read.
        state = fromRows(*rows);
        state.derivedKey = id.deviceKey;
        state.deviceKey = kept->deviceKey;
        state.accessKeys = kept->accessKeys;
        state.pairSecrets = kept->pairSecrets;
        if (id.fingerprint.empty()) {
            result.value = std::move(state);
            return result;
        }
        logLine("state: console identity read; credentials bound");
        // Moved in from a state.json earlier this run: it goes once bound.
        moved = legacy != dbPath && std::filesystem::exists(legacy, ec);
    } else if (legacy != dbPath && (std::filesystem::exists(legacy, ec) || std::filesystem::exists(legacy + ".old", ec))) {
        // Earlier builds kept it all in state.json: its key goes on, so the
        // server sees the same console and nothing is approved again.
        auto old = parseLegacy(readFile(legacy));
        if (!old) old = parseLegacy(readFile(legacy + ".old")); // a save cut off between its renames
        moved = true;
        if (old) {
            state = cleaned(std::move(*old));
            if (!validDeviceKey(state.deviceKey)) why = Rebound::Changed;
            logLine("state: state.json moved into cookingfoil.db");
        } else {
            // Kept for whoever wants to look; a new key, approved again.
            std::filesystem::rename(legacy, legacy + ".broken", ec);
            logLine("state: state.json unreadable, kept as state.json.broken");
            why = Rebound::Recreated;
        }
    } else {
        why = setAside ? Rebound::Recreated : Rebound::None; // a first start says nothing
        result.created = true;
    }
    if (why != Rebound::None || state.deviceKey.empty()) {
        if (!rekey(state, id)) { result.error.message = tr("Could not create the device key."); return result; }
        if (why != Rebound::None) {
            logLine(std::string("state: ") + reasonName(why) + " credentials; a new device key");
            rebound = int(why);
        }
    }
    state.derivedKey = id.deviceKey;
    Error saved;
    if (saveState(dbPath, state, saved)) {
        // Unbound (no console ID), state.json stays to be moved in again.
        if (moved && !id.fingerprint.empty()) {
            std::filesystem::remove(legacy, ec);
            std::filesystem::remove(legacy + ".old", ec);
        }
    } else
        logLine("state: " + saved.message);
    result.value = std::move(state);
    return result;
}
std::string serializePending(const PendingInstall& pending) {
    if (pending.files.empty()) return "[]";
    Json list(json_object_new_array());
    auto* entry = json_object_new_object();
    auto add = [](json_object* o, const char* key, const std::string& value) { json_object_object_add(o, key, json_object_new_string(value.c_str())); };
    add(entry, "name", pending.name); add(entry, "icon", pending.icon); add(entry, "target", pending.target);
    if (!pending.server.empty()) add(entry, "server", pending.server);
    auto* files = json_object_new_array();
    for (const auto& f : pending.files) {
        auto* file = json_object_new_object();
        add(file, "url", f.url); add(file, "name", f.name); add(file, "type", f.type);
        add(file, "titleId", f.titleId); add(file, "baseId", f.baseId); add(file, "versionName", f.versionName);
        if (!f.format.empty()) add(file, "format", f.format);
        json_object_object_add(file, "size", json_object_new_int64(int64_t(f.size)));
        json_object_object_add(file, "version", json_object_new_int64(f.version));
        json_object_array_add(files, file);
    }
    json_object_object_add(entry, "files", files);
    json_object_array_add(list.get(), entry);
    return json_object_to_json_string_ext(list.get(), JSON_C_TO_STRING_PLAIN);
}
std::optional<PendingInstall> parsePending(const std::string& queueJson) {
    Error error;
    auto root = parseJson(queueJson, error);
    if (error || !json_object_is_type(root.get(), json_type_array) || json_object_array_length(root.get()) == 0)
        return std::nullopt;
    auto* entry = json_object_array_get_idx(root.get(), 0);
    PendingInstall pending;
    pending.name = stringField(entry, "name");
    pending.icon = stringField(entry, "icon");
    pending.target = stringField(entry, "target", "sd") == "nand" ? "nand" : "sd";
    pending.server = stringField(entry, "server");
    auto* files = field(entry, "files");
    if (!json_object_is_type(files, json_type_array)) return std::nullopt;
    for (size_t i = 0; i < json_object_array_length(files) && i < 512; ++i) {
        auto* file = json_object_array_get_idx(files, i);
        Item item;
        item.url = stringField(file, "url");
        item.name = stringField(file, "name");
        item.type = stringField(file, "type", "base");
        item.titleId = stringField(file, "titleId");
        item.baseId = stringField(file, "baseId");
        item.versionName = stringField(file, "versionName");
        if (stringField(file, "format") == "nsp") item.format = "nsp"; // the only one named so
        item.size = uint64_t(std::max<int64_t>(0, integerField(file, "size")));
        item.version = uint32_t(std::max<int64_t>(0, integerField(file, "version")));
        if (!item.url.empty()) pending.files.push_back(std::move(item));
    }
    if (pending.files.empty()) return std::nullopt;
    return pending;
}
}
