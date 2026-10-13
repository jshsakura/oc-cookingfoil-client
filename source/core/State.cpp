#include "core/State.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
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
bool saveState(const std::string& path, const State& state, Error& error) {
    Json object(json_object_new_object());
    json_object_object_add(object.get(), "deviceKey", json_object_new_string(state.deviceKey.c_str()));
    json_object_object_add(object.get(), "lastServer", json_object_new_string(state.lastServer.c_str()));
    json_object_object_add(object.get(), "lastScreen", json_object_new_string(state.lastScreen.c_str()));
    json_object_object_add(object.get(), "language", json_object_new_string(state.language.c_str()));
    json_object_object_add(object.get(), "ignoreSignature", json_object_new_boolean(state.skipSignatureCheck));
    json_object_object_add(object.get(), "ignoreFirmware", json_object_new_boolean(state.skipFirmwareCheck));
    json_object_object_add(object.get(), "reportErrors", json_object_new_boolean(state.reportErrors));
    json_object_object_add(object.get(), "makeShortcuts", json_object_new_boolean(state.makeShortcuts));
    json_object_object_add(object.get(), "installTarget", json_object_new_string(state.installTarget.c_str()));
    auto* keys = json_object_new_object();
    for (const auto& [url, key] : state.accessKeys) json_object_object_add(keys, url.c_str(), json_object_new_string(key.c_str()));
    json_object_object_add(object.get(), "accessKeys", keys);
    auto* notes = json_object_new_object();
    for (const auto& [url, note] : state.pairMessages)
        if (!note.empty()) json_object_object_add(notes, url.c_str(), json_object_new_string(note.c_str()));
    json_object_object_add(object.get(), "pairMessages", notes);
    Error ignored;
    auto queue = parseJson(state.queueJson, ignored);
    json_object_object_add(object.get(), "queue", queue && json_object_is_type(queue.get(), json_type_array) ? queue.release() : json_object_new_array());
    return atomicWrite(path, json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PRETTY), error.message);
}
Parsed<State> loadState(const std::string& path) {
    Parsed<State> result;
    std::error_code ec;
    if (std::filesystem::exists(path, ec) || std::filesystem::exists(path + ".old", ec)) {
        auto root = parseJson(readFile(path), result.error);
        if (result.error) return result;
        result.value.deviceKey = stringField(root.get(), "deviceKey");
        if (!validDeviceKey(result.value.deviceKey)) { result.error.message = tr("기기 키가 잘못되었습니다. 상태 파일을 확인해 주세요."); return result; }
        result.value.lastServer = stringField(root.get(), "lastServer");
        result.value.lastScreen = stringField(root.get(), "lastScreen", "home");
        result.value.language = stringField(root.get(), "language");
        // Ignoring both checks is the default: most homebrew stores expect
        // sigpatches. Earlier builds saved "skipSignatureCheck": false on every
        // save, not as a choice, so only the newer keys count.
        auto* skip = field(root.get(), "ignoreSignature");
        if (json_object_is_type(skip, json_type_boolean)) result.value.skipSignatureCheck = json_object_get_boolean(skip);
        auto* firmware = field(root.get(), "ignoreFirmware");
        if (json_object_is_type(firmware, json_type_boolean)) result.value.skipFirmwareCheck = json_object_get_boolean(firmware);
        auto* report = field(root.get(), "reportErrors");
        if (json_object_is_type(report, json_type_boolean)) result.value.reportErrors = json_object_get_boolean(report);
        auto* shortcuts = field(root.get(), "makeShortcuts");
        if (json_object_is_type(shortcuts, json_type_boolean)) result.value.makeShortcuts = json_object_get_boolean(shortcuts);
        const auto target = stringField(root.get(), "installTarget");
        if (target == "sd" || target == "nand") result.value.installTarget = target;
        auto* keys = field(root.get(), "accessKeys");
        if (json_object_is_type(keys, json_type_object)) {
            json_object_object_foreach(keys, url, value) {
                if (json_object_is_type(value, json_type_string) && !normalizeUrl(url).empty()) result.value.accessKeys[url] = json_object_get_string(value);
            }
        }
        auto* notes = field(root.get(), "pairMessages");
        if (json_object_is_type(notes, json_type_object)) {
            json_object_object_foreach(notes, url, value) {
                if (json_object_is_type(value, json_type_string) && *json_object_get_string(value) && !normalizeUrl(url).empty())
                    result.value.pairMessages[url] = json_object_get_string(value);
            }
        }
        auto* queue = field(root.get(), "queue");
        if (json_object_is_type(queue, json_type_array)) result.value.queueJson = json_object_to_json_string_ext(queue, JSON_C_TO_STRING_PLAIN);
        return result;
    }
    if (ec) { result.error.message = ec.message(); return result; }
    unsigned char entropy[32]{};
#ifdef __SWITCH__
    randomGet(entropy, sizeof(entropy));
#else
    std::ifstream random("/dev/urandom", std::ios::binary);
    if (!random.read(reinterpret_cast<char*>(entropy), sizeof(entropy))) { result.error.message = tr("기기 키를 만들지 못했습니다."); return result; }
#endif
    const char* hex = "0123456789abcdef";
    for (auto byte : entropy) { result.value.deviceKey += hex[byte >> 4]; result.value.deviceKey += hex[byte & 15]; }
    result.created = saveState(path, result.value, result.error);
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
