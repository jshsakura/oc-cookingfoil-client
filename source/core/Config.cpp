#include <cerrno>
#include <cstring>
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include "core/Log.hpp"
#include <fstream>
#include <fcntl.h>
#include <unistd.h>
namespace foil {
namespace {
std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); }); return s; }
std::string urlPart(CURLU* url, CURLUPart part, unsigned flags = 0) {
    char* raw = nullptr;
    if (curl_url_get(url, part, &raw, flags) != CURLUE_OK) return {};
    std::string value(raw); curl_free(raw); return value;
}
bool headerSafe(const std::string& value) { return value.find_first_of("\r\n") == std::string::npos; }
}
std::string normalizeUrl(const std::string& input) {
    std::string url = input;
    const auto first = url.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    url = url.substr(first, url.find_last_not_of(" \t") - first + 1);
    for (unsigned char c : url) if (c <= 32 || c == 127) return {};
    if (url.find("://") == std::string::npos) url = "https://" + url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    const auto lowered = lower(url);
    for (const std::string suffix : {"/shop.tfl", "/shop.json"}) {
        if (lowered.size() >= suffix.size() && lowered.compare(lowered.size() - suffix.size(), suffix.size(), suffix) == 0) {
            url.resize(url.size() - suffix.size()); break;
        }
    }
    auto* handle = curl_url();
    if (!handle) return {};
    const bool parsed = curl_url_set(handle, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK;
    const auto scheme = lower(urlPart(handle, CURLUPART_SCHEME));
    const bool valid = parsed && (scheme == "http" || scheme == "https") && !urlPart(handle, CURLUPART_HOST).empty()
        && urlPart(handle, CURLUPART_USER).empty() && urlPart(handle, CURLUPART_PASSWORD).empty()
        && urlPart(handle, CURLUPART_QUERY).empty() && urlPart(handle, CURLUPART_FRAGMENT).empty();
    std::string normalized = valid ? urlPart(handle, CURLUPART_URL) : "";
    curl_url_cleanup(handle);
    while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
    return normalized;
}
std::string originOf(const std::string& url) {
    auto* h = curl_url();
    if (!h || curl_url_set(h, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) { if (h) curl_url_cleanup(h); return {}; }
    auto result = lower(urlPart(h, CURLUPART_SCHEME)) + "://" + lower(urlPart(h, CURLUPART_HOST));
    const auto port = urlPart(h, CURLUPART_PORT, CURLU_NO_DEFAULT_PORT);
    if (!port.empty()) result += ":" + port;
    curl_url_cleanup(h);
    return result;
}
Parsed<Config> parseConfig(const std::string& json) {
    Parsed<Config> result;
    auto root = parseJson(json, result.error);
    if (result.error) return result;
    auto fail = [&](const std::string& message) { result.error.message = message; };
    if (!json_object_is_type(root.get(), json_type_object)) { fail(tr("Write the config as a JSON object.")); return result; }
    auto* servers = field(root.get(), "servers");
    if (servers && !json_object_is_type(servers, json_type_array)) { fail(tr("servers must be an array.")); return result; }
    const auto count = servers ? json_object_array_length(servers) : 0;
    if (count > 32) { fail(tr("You can add up to 32 servers.")); return result; }
    for (size_t i = 0; i < count; ++i) {
        auto* object = json_object_array_get_idx(servers, i);
        Server server;
        if (!json_object_is_type(object, json_type_object)) { fail(tr("Check the server entries.")); return result; }
        for (const auto* key : {"url", "title", "username", "password", "cfClientId", "cfClientSecret", "enrollToken"}) {
            auto* value = field(object, key);
            if (value && (!json_object_is_type(value, json_type_string) || !headerSafe(json_object_get_string(value)))) {
                fail(trf("{0} must be a string without line breaks.", {key})); return result;
            }
        }
        server.url = normalizeUrl(stringField(object, "url"));
        if (server.url.empty()) { fail(trf("Check the HTTP/HTTPS address of server {0}.", {std::to_string(i + 1)})); return result; }
        server.title = stringField(object, "title");
        if (server.title.empty()) server.title = originOf(server.url).substr(originOf(server.url).find("://") + 3);
        server.username = stringField(object, "username"); server.password = stringField(object, "password");
        server.cfClientId = stringField(object, "cfClientId"); server.cfClientSecret = stringField(object, "cfClientSecret");
        // Letters, digits, '_' and '-' only (it goes into a JSON body); any
        // other is no token at all.
        server.enrollToken = stringField(object, "enrollToken");
        if (server.enrollToken.size() > 128 || server.enrollToken.find_first_not_of(
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") != std::string::npos)
            server.enrollToken.clear();
        auto* enabled = field(object, "enabled");
        if (enabled && !json_object_is_type(enabled, json_type_boolean)) { fail(tr("enabled must be true or false.")); return result; }
        server.enabled = !enabled || json_object_get_boolean(enabled);
        result.value.servers.push_back(server);
    }
    for (const auto* key : {"language", "installTarget"}) {
        auto* value = field(root.get(), key);
        if (value && !json_object_is_type(value, json_type_string)) { fail(trf("{0} must be a string.", {key})); return result; }
    }
    result.value.language = stringField(root.get(), "language", "ko");
    result.value.installTarget = stringField(root.get(), "installTarget", "sd");
    if (result.value.language != "ko" && result.value.language != "en" && result.value.language != "ja" && result.value.language != "zh") fail(tr("language must be one of ko, en, ja, zh."));
    if (result.value.installTarget != "sd" && result.value.installTarget != "nand") fail(tr("installTarget must be sd or nand."));
    return result;
}
std::string serializeConfig(const Config& config, bool redact) {
    Json root(json_object_new_object());
    auto* servers = json_object_new_array();
    for (const auto& s : config.servers) {
        auto* object = json_object_new_object();
        auto add = [&](const char* key, const std::string& value) { json_object_object_add(object, key, json_object_new_string(value.c_str())); };
        add("title", s.title); add("url", s.url); add("username", s.username);
        add("password", redact && !s.password.empty() ? "••••" : s.password);
        add("cfClientId", redact && !s.cfClientId.empty() ? "••••" : s.cfClientId);
        add("cfClientSecret", redact && !s.cfClientSecret.empty() ? "••••" : s.cfClientSecret);
        if (!s.enrollToken.empty()) add("enrollToken", redact ? "••••" : s.enrollToken);
        json_object_object_add(object, "enabled", json_object_new_boolean(s.enabled));
        json_object_array_add(servers, object);
    }
    json_object_object_add(root.get(), "servers", servers);
    json_object_object_add(root.get(), "language", json_object_new_string(config.language.c_str()));
    json_object_object_add(root.get(), "installTarget", json_object_new_string(config.installTarget.c_str()));
    return std::string(json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE)) + "\n";
}
namespace {
// The file's own bytes, without readFile's ".old" fallback.
std::string readExactly(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return std::string(std::istreambuf_iterator<char>(input), {});
}
// The servers are the one thing that cannot be got back from anywhere else.
// When config.json is gone, or reads with no servers while a copy with them
// is left beside it (a write cut between moving the old file aside and
// putting the new one in place, or an older build that then made a fresh
// empty one), that copy goes back: the new one first (.tmp, synced before
// any rename), then the one before it (.old), then the last saved (.bak).
// A deliberate save leaves none of .tmp and .old and writes .bak alike, so
// servers removed on purpose stay removed.
bool restoreServers(const std::string& path) {
    for (const auto* side : {".tmp", ".old", ".bak"}) {
        const auto copy = readExactly(path + side);
        if (copy.empty()) continue;
        const auto parsed = parseConfig(copy);
        if (parsed.error || parsed.value.servers.empty()) continue;
        std::string error;
        if (!atomicWrite(path, copy, error)) {
            logLine(std::string("config: could not restore from ") + side + ": " + error);
            return false;
        }
        atomicWrite(path + ".bak", copy, error); // a copy stays for next time
        std::error_code ec;
        std::filesystem::remove(path + ".tmp", ec);
        std::filesystem::remove(path + ".old", ec);
        logLine(std::string("config: servers restored from ") + side);
        return true;
    }
    return false;
}
} // namespace
bool saveConfig(const std::string& path, const Config& config, std::string& error) {
    const auto text = serializeConfig(config);
    if (!atomicWrite(path, text, error)) return false;
    // The last saved, kept apart: what goes back if config.json is lost.
    std::string ignored;
    atomicWrite(path + ".bak", text, ignored);
    return true;
}
bool dropEnrollToken(const std::string& path, const std::string& serverUrl, std::string& error) {
    if (path.empty()) return true;
    auto config = parseConfig(readFile(path));
    if (config.error) { error = config.error.message; return false; }
    bool changed = false;
    for (auto& server : config.value.servers)
        if (server.url == serverUrl && !server.enrollToken.empty()) { server.enrollToken.clear(); changed = true; }
    return !changed || saveConfig(path, config.value, error);
}
Parsed<Config> loadConfig(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        const auto text = readExactly(path);
        // Nothing in it (cut while being made) reads as missing.
        const bool blank = text.find_first_not_of(" \t\r\n") == std::string::npos;
        auto parsed = parseConfig(text);
        if (blank || (!parsed.error && parsed.value.servers.empty())) {
            // What the card holds when it reads with no servers, for the log.
            std::string beside;
            for (const auto* side : {".tmp", ".old", ".bak"})
                if (std::filesystem::exists(path + side, ec)) beside += std::string(" ") + side;
            logLine("config: no servers in " + std::to_string(text.size()) + " bytes;" +
                    (beside.empty() ? std::string(" nothing beside it") : " beside it:" + beside));
            if (restoreServers(path)) return parseConfig(readExactly(path));
        }
        return parsed;
    }
    // Gone: a copy left beside it comes back before a fresh empty one is made.
    if (!ec && restoreServers(path)) return parseConfig(readExactly(path));
    Parsed<Config> result;
    if (ec) { result.error.message = ec.message(); return result; }
    const auto parent = std::filesystem::path(path).parent_path();
    if (!parent.empty() && !makeDirectories(parent.string())) { result.error.message = std::strerror(errno); return result; }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        if (errno == EEXIST) return parseConfig(readFile(path));
        result.error.message = tr("Could not create the config file."); return result;
    }
    const auto contents = serializeConfig(result.value);
    size_t offset = 0;
    while (offset < contents.size()) {
        const auto written = ::write(fd, contents.data() + offset, contents.size() - offset);
        if (written <= 0) break;
        offset += written;
    }
    const bool flushed = ::fsync(fd) == 0;
    const bool closed = ::close(fd) == 0;
    result.created = offset == contents.size() && flushed && closed;
    if (!result.created) result.error.message = tr("Could not write the config file.");
    return result;
}
}
