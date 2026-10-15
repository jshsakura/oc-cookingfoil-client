#include "core/State.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include "core/DownloadStore.hpp"
#include "core/Sha256.hpp"
#include <atomic>
#include <mutex>
#include <algorithm>
#include <filesystem>
#include <fstream>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil {
static bool validDeviceKey(const std::string& key) {
    return key.size() == 64 && key.find_first_not_of("0123456789abcdef") == std::string::npos;
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
std::atomic<int> rebound{int(Rebound::None)};
// Settings by name in the settings table; credentials by kind.
constexpr const char* DeviceKind = "device";
constexpr const char* FingerprintKind = "fingerprint";
constexpr const char* AccessKind = "access";
constexpr const char* SecretKind = "secret";
constexpr const char* SealKind = "seal";
constexpr const char* PendingSetting = "pendingInstall";
DeviceIdentity currentIdentity() {
    std::lock_guard<std::mutex> held(identityLock);
    return identity;
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
                     {"makeShortcuts", flag(state.makeShortcuts)}};
    if (state.queueJson != "[]") rows.settings[PendingSetting] = state.queueJson;
    rows.notes = state.pairMessages;
    rows.credentials[{DeviceKind, ""}] = state.deviceKey;
    rows.credentials[{FingerprintKind, ""}] = id.fingerprint;
    for (const auto& [server, key] : state.accessKeys) rows.credentials[{AccessKind, server}] = key;
    for (const auto& [server, secret] : state.pairSecrets) rows.credentials[{SecretKind, server}] = secret;
    rows.credentials[{SealKind, ""}] = seal(id, rows);
    return rows;
}
// Credentials that are not this console's: why, or None when they are.
Rebound check(const DownloadStore::StateRows& rows, const DeviceIdentity& id) {
    const auto value = [&](const char* kind) {
        const auto at = rows.credentials.find({kind, ""});
        return at == rows.credentials.end() ? std::string() : at->second;
    };
    if (!validDeviceKey(value(DeviceKind))) return Rebound::Changed;
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
// A new device key; whatever servers gave the old one goes with it.
bool rekey(State& state) {
    state.deviceKey = randomKey();
    state.accessKeys.clear();
    state.pairSecrets.clear();
    return !state.deviceKey.empty();
}
const char* reasonName(Rebound why) {
    return why == Rebound::Moved ? "another console's" : why == Rebound::Changed ? "edited" : "lost";
}
} // namespace
DeviceIdentity deriveIdentity(const std::string& consoleValue) {
    if (consoleValue.empty()) return {};
    const auto digest = [&](const char* purpose) {
        Sha256 hash;
        const std::string text = std::string("CookingFoil ") + purpose + '\0' + consoleValue;
        hash.update(text.data(), text.size());
        return hash.hex();
    };
    return {digest("device fingerprint v1"), hexBytes(digest("state seal v1"))};
}
void setDeviceIdentity(DeviceIdentity id) {
    std::lock_guard<std::mutex> held(identityLock);
    identity = std::move(id);
}
Rebound takeRebound() { return Rebound(rebound.exchange(int(Rebound::None))); }
bool saveState(const std::string& dbPath, const State& state, Error& error) {
    DownloadStore store;
    std::string failure;
    if (!store.open(dbPath, failure) || !store.saveStateRows(toRows(state, currentIdentity()), failure)) {
        error.message = failure.empty() ? "state not saved" : failure;
        return false;
    }
    return true;
}
Parsed<State> loadState(const std::string& dbPath) {
    Parsed<State> result;
    DownloadStore store;
    std::string failure;
    std::optional<DownloadStore::StateRows> rows;
    if (store.open(dbPath, failure)) rows = store.stateRows(failure);
    if (!rows) {
        logLine("state: " + failure);
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
        state = fromRows(*rows);
        why = check(*rows, id);
        if (why == Rebound::None) {
            result.value = std::move(state);
            return result;
        }
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
        if (!rekey(state)) { result.error.message = tr("Could not create the device key."); return result; }
        if (why != Rebound::None) {
            logLine(std::string("state: ") + reasonName(why) + " credentials; a new device key");
            rebound = int(why);
        }
    }
    Error saved;
    if (saveState(dbPath, state, saved)) {
        if (moved) {
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
    auto* files = json_object_new_array();
    for (const auto& f : pending.files) {
        auto* file = json_object_new_object();
        add(file, "url", f.url); add(file, "name", f.name); add(file, "type", f.type);
        add(file, "titleId", f.titleId); add(file, "baseId", f.baseId); add(file, "versionName", f.versionName);
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
        item.size = uint64_t(std::max<int64_t>(0, integerField(file, "size")));
        item.version = uint32_t(std::max<int64_t>(0, integerField(file, "version")));
        if (!item.url.empty()) pending.files.push_back(std::move(item));
    }
    if (pending.files.empty()) return std::nullopt;
    return pending;
}
}
