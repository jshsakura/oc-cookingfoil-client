#include "net/Api.hpp"
#include "core/Version.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Homebrew.hpp"
#include "net/Ranged.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <limits>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <set>
#include <chrono>
#include <thread>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::net {
namespace {
std::string base64(const std::string& input) {
    const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out; unsigned value = 0; int bits = -6;
    for (unsigned char c : input) { value = (value << 8) | c; bits += 8; while (bits >= 0) { out += chars[(value >> bits) & 63]; bits -= 6; } }
    if (bits > -6) out += chars[((value << 8) >> (bits + 8)) & 63];
    while (out.size() % 4) out += '=';
    return out;
}
size_t writeBody(char* data, size_t size, size_t count, void* context) {
    auto& result = *static_cast<Response*>(context);
    const auto bytes = size * count;
    if (result.body.size() + bytes > 32 * 1024 * 1024) { result.error = tr("응답이 너무 큽니다."); return 0; }
    result.body.append(data, bytes); return bytes;
}
size_t writeHeader(char* data, size_t size, size_t count, void* context) {
    const auto bytes = size * count;
    std::string line(data, bytes);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        auto key = line.substr(0, colon); std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
        if (key == "etag") {
            auto value = line.substr(colon + 1); const auto begin = value.find_first_not_of(" \t\r\n");
            static_cast<Response*>(context)->etag = begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
        } else if (key == "content-range") {
            auto value = line.substr(colon + 1); const auto begin = value.find_first_not_of(" \t\r\n");
            static_cast<Response*>(context)->contentRange = begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
        } else if (key == "content-length") {
            static_cast<Response*>(context)->contentLength = std::strtoull(line.c_str() + colon + 1, nullptr, 10);
        } else if (key == "retry-after") {
            const auto seconds = std::strtol(line.c_str() + colon + 1, nullptr, 10);
            static_cast<Response*>(context)->retryAfter = int(std::clamp<long>(seconds, 0, 3600));
        }
    }
    return bytes;
}
bool isHttp(const std::string& url) { const auto origin = originOf(url); return origin.rfind("http://", 0) == 0 || origin.rfind("https://", 0) == 0; }
// A field the server may send as text or as a number ("16.0.0" or 16).
std::string textField(json_object* obj, const char* key) {
    auto* value = field(obj, key);
    if (json_object_is_type(value, json_type_string)) return json_object_get_string(value);
    if (json_object_is_type(value, json_type_int)) return std::to_string(json_object_get_int64(value));
    return {};
}
uint64_t positive(json_object* obj, const char* key) { return static_cast<uint64_t>(std::max<int64_t>(0, integerField(obj, key))); }
std::string titleId(json_object* obj, const char* key) {
    auto id = stringField(obj, key);
    std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) { return std::toupper(c); });
    return id.size() == 16 && id.find_first_not_of("0123456789ABCDEF") == std::string::npos ? id : "";
}
}
std::vector<std::string> authHeaders(const Server& server, const State& state, const std::string& url) {
    std::vector<std::string> headers{"Version: " COOKINGFOIL_VERSION};
    if (originOf(url) != originOf(server.url)) return headers;
    headers.push_back(std::string("Accept-Language: ") + languageCode());
    if (!server.username.empty() || !server.password.empty()) headers.push_back("Authorization: Basic " + base64(server.username + ":" + server.password));
    if (!state.deviceKey.empty()) headers.push_back("X-Device-Key: " + state.deviceKey);
    const auto key = state.accessKeys.find(server.url);
    if (key != state.accessKeys.end() && !key->second.empty()) headers.push_back("X-Access-Key: " + key->second);
    if (!server.cfClientId.empty()) headers.push_back("CF-Access-Client-Id: " + server.cfClientId);
    if (!server.cfClientSecret.empty()) headers.push_back("CF-Access-Client-Secret: " + server.cfClientSecret);
    return headers;
}
// Compared by value (error_.message == offlineMessage()), so both sides go through tr().
const char* offlineMessage() { return tr("본체가 인터넷에 연결되어 있지 않습니다. 인터넷 설정을 확인합니다."); }
bool internetConnected() {
#ifdef __SWITCH__
    // Only for wording: on a network whose DNS blocks the vendor's test
    // servers this reads offline although the server is reachable.
    NifmInternetConnectionType type{};
    NifmInternetConnectionStatus status{};
    u32 strength = 0;
    return R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
           status == NifmInternetConnectionStatus_Connected;
#else
    return true;
#endif
}
int signalStrength() {
#ifdef __SWITCH__
    NifmInternetConnectionType type{};
    NifmInternetConnectionStatus status{};
    u32 strength = 0;
    if (R_FAILED(nifmGetInternetConnectionStatus(&type, &strength, &status)) ||
        status != NifmInternetConnectionStatus_Connected)
        return -1;
    return type == NifmInternetConnectionType_Ethernet ? 3 : int(std::min<u32>(strength, 3));
#else
    return 3;
#endif
}
std::string connectionError(int code, const std::string& url) {
    if ((code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT ||
         code == CURLE_OPERATION_TIMEDOUT) && !internetConnected())
        return offlineMessage();
    auto host = originOf(url);
    if (host.find("://") != std::string::npos) host = host.substr(host.find("://") + 3);
    switch (static_cast<CURLcode>(code)) {
    case CURLE_COULDNT_RESOLVE_HOST:
        return trf("{0} 주소를 찾지 못했습니다. 주소 철자와 본체의 DNS 설정을 확인합니다.", {host});
    case CURLE_COULDNT_CONNECT:
        return trf("{0}에 연결하지 못했습니다. 서버가 켜져 있는지 확인합니다.", {host});
    case CURLE_OPERATION_TIMEDOUT:
        return trf("{0}에서 응답이 없습니다. 잠시 뒤 다시 시도합니다.", {host});
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CONNECT_ERROR: {
        // Certificates are checked against the console's clock; one left far
        // in the past (no time sync) fails every secure connection.
        const auto now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        if (local.tm_year + 1900 < 2026) {
            char date[16];
            std::strftime(date, sizeof date, "%Y-%m-%d", &local);
            return trf("본체 시계가 {0}(으)로 맞지 않아 보안 연결을 만들 수 없습니다. 설정에서 날짜와 시간을 맞춘 뒤 다시 시도합니다.", {date});
        }
        return trf("{0}와 보안 연결을 만들지 못했습니다. 본체 시계와 서버 인증서를 확인합니다.", {host});
    }
    default:
        return curl_easy_strerror(static_cast<CURLcode>(code));
    }
}
Response get(const std::string& url, const std::vector<std::string>& headers, const std::string& etag, const std::atomic<bool>* cancel) {
    Response result;
    if (!isHttp(url)) { result.error = tr("HTTP/HTTPS 주소를 확인해 주세요."); return result; }
    auto* curl = curl_easy_init();
    if (!curl) { result.error = tr("연결을 시작하지 못했습니다."); return result; }
    curl_slist* list = nullptr;
    for (const auto& header : headers) if (header.find_first_of("\r\n") == std::string::npos) list = curl_slist_append(list, header.c_str());
    if (!etag.empty() && etag.find_first_of("\r\n") == std::string::npos) list = curl_slist_append(list, ("If-None-Match: " + etag).c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CookingFoil/" COOKINGFOIL_VERSION);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    // The console has no IPv6 route; AAAA answers only slow down or fail the lookup.
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return static_cast<const std::atomic<bool>*>(data)->load() ? 1 : 0; });
    }
    // Redirects are deliberately surfaced instead of forwarding keys to an
    // arbitrary destination. Configure the server's final origin in JSON.
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
    constexpr int ResolveRetryMs = 1500;
    auto code = curl_easy_perform(curl);
    if (code == CURLE_COULDNT_RESOLVE_HOST && result.body.empty() && !(cancel && cancel->load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ResolveRetryMs));
        code = curl_easy_perform(curl);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    if (code != CURLE_OK && result.error.empty()) result.error = connectionError(code, url);
    if (code != CURLE_OK || result.status >= 400)
        logLine("GET " + url.substr(0, url.find_first_of("?#")) + " status=" + std::to_string(result.status) +
                " curl=" + std::to_string(int(code)) + (result.error.empty() ? "" : " " + result.error));
    curl_slist_free_all(list); curl_easy_cleanup(curl);
    return result;
}
namespace {
struct Stream {
    CURL* curl;
    Response* result;
    const std::function<Start(const Response&)>* start;
    const std::function<bool(const char*, size_t)>* onData;
    bool aborted = false, started = false;
    Start verdict = Start::Drop;
};
size_t streamBody(char* data, size_t size, size_t count, void* context) {
    auto& s = *static_cast<Stream*>(context);
    if (!s.started) {
        s.started = true;
        curl_easy_getinfo(s.curl, CURLINFO_RESPONSE_CODE, &s.result->status);
        s.verdict = (*s.start)(*s.result);
    }
    if (s.verdict == Start::Stop) { s.aborted = true; return 0; }
    if (s.verdict == Start::Drop) return size * count; // error pages are dropped
    if (!(*s.onData)(data, size * count)) { s.aborted = true; return 0; }
    return size * count;
}
}
Response stream(const std::string& url, const std::vector<std::string>& headers,
                const std::function<bool(const char*, size_t)>& onData, const std::atomic<bool>* cancel) {
    return stream(url, headers, [](const Response& reply) { return reply.status == 200 ? Start::Take : Start::Drop; }, onData, cancel);
}
Response stream(const std::string& url, const std::vector<std::string>& headers,
                const std::function<Start(const Response&)>& start,
                const std::function<bool(const char*, size_t)>& onData, const std::atomic<bool>* cancel) {
    Response result;
    if (!isHttp(url)) { result.error = tr("HTTP/HTTPS 주소를 확인해 주세요."); return result; }
    auto* curl = curl_easy_init();
    if (!curl) { result.error = tr("연결을 시작하지 못했습니다."); return result; }
    curl_slist* list = nullptr;
    for (const auto& header : headers) if (header.find_first_of("\r\n") == std::string::npos) list = curl_slist_append(list, header.c_str());
    Stream context{curl, &result, &start, &onData};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CookingFoil/" COOKINGFOIL_VERSION);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 512L * 1024); // fewer, larger writes on the way to the card
    // Large patches take minutes; only a stalled transfer gives up.
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return static_cast<const std::atomic<bool>*>(data)->load() ? 1 : 0; });
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, streamBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    // The ETag comes along (a homebrew download carries its SHA-256 there).
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
    const auto code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    // A body shorter than Content-Length (the file shrank on the server)
    // ends in CURLE_PARTIAL_FILE and counts as a failure.
    if (code != CURLE_OK && !context.aborted) result.error = connectionError(code, url);
    if (code != CURLE_OK || result.status >= 400)
        logLine("STREAM " + url.substr(0, url.find_first_of("?#")) + " status=" + std::to_string(result.status) + " curl=" + std::to_string(int(code)));
    curl_slist_free_all(list); curl_easy_cleanup(curl);
    return result;
}
namespace {
struct Posting {
    CURL* curl;
    Response* result;
    FILE* out = nullptr;
    bool failedWrite = false;
};
size_t postBody(char* data, size_t size, size_t count, void* context) {
    auto& p = *static_cast<Posting*>(context);
    const auto bytes = size * count;
    long status = 0;
    curl_easy_getinfo(p.curl, CURLINFO_RESPONSE_CODE, &status);
    if (p.out && status >= 200 && status < 300) {
        if (std::fwrite(data, 1, bytes, p.out) != bytes) { p.failedWrite = true; return 0; }
        return bytes;
    }
    if (p.result->body.size() + bytes > 4 * 1024 * 1024) { p.result->error = tr("응답이 너무 큽니다."); return 0; }
    p.result->body.append(data, bytes);
    return bytes;
}
size_t readUpload(char* buffer, size_t size, size_t count, void* context) {
    return std::fread(buffer, size, count, static_cast<FILE*>(context));
}
}
Response post(const std::string& url, const std::vector<std::string>& headers, const PostOptions& options, const std::atomic<bool>* cancel) {
    Response result;
    if (!isHttp(url)) { result.error = tr("HTTP/HTTPS 주소를 확인해 주세요."); return result; }
    std::unique_ptr<FILE, int (*)(FILE*)> in(nullptr, std::fclose), out(nullptr, std::fclose);
    curl_off_t uploadSize = 0;
    if (!options.uploadFile.empty()) {
        in.reset(std::fopen(options.uploadFile.c_str(), "rb"));
        if (!in || std::fseek(in.get(), 0, SEEK_END) != 0) { result.error = trf("파일을 읽지 못했습니다: {0}", {options.uploadFile}); return result; }
        uploadSize = static_cast<curl_off_t>(std::ftell(in.get()));
        std::rewind(in.get());
    }
    if (!options.saveTo.empty()) {
        out.reset(std::fopen(options.saveTo.c_str(), "wb"));
        writeBuffer(out.get());
        if (!out) { result.error = trf("파일을 만들지 못했습니다: {0}", {options.saveTo}); return result; }
    }
    auto* curl = curl_easy_init();
    if (!curl) { result.error = tr("연결을 시작하지 못했습니다."); return result; }
    curl_slist* list = nullptr;
    for (const auto& header : headers) if (header.find_first_of("\r\n") == std::string::npos) list = curl_slist_append(list, header.c_str());
    Posting context{curl, &result, out.get()};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
#ifdef __SWITCH__
    curl_easy_setopt(curl, CURLOPT_CAINFO, "romfs:/cacert.pem");
#endif
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CookingFoil/" COOKINGFOIL_VERSION);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    if (in) {
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, readUpload);
        curl_easy_setopt(curl, CURLOPT_READDATA, in.get());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, uploadSize);
    } else {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, options.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(options.body.size()));
    }
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, options.timeout > 0 ? options.timeout : 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    if (cancel) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, cancel);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, +[](void* data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int { return static_cast<const std::atomic<bool>*>(data)->load() ? 1 : 0; });
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, postBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &context);
    const auto code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status);
    if (context.failedWrite && result.error.empty()) result.error = trf("파일을 쓰지 못했습니다: {0}", {options.saveTo});
    if (code != CURLE_OK && result.error.empty()) result.error = connectionError(code, url);
    // Only the address is logged: bodies carry tokens and codes.
    if (code != CURLE_OK || result.status >= 400)
        logLine("POST " + url.substr(0, url.find_first_of("?#")) + " status=" + std::to_string(result.status) +
                " curl=" + std::to_string(int(code)));
    curl_slist_free_all(list); curl_easy_cleanup(curl);
    if (out) {
        const bool closed = std::fclose(out.release()) == 0;
        if (!closed && result.error.empty()) result.error = trf("파일을 쓰지 못했습니다: {0}", {options.saveTo});
        if (!result.error.empty() || result.status < 200 || result.status >= 300) std::remove(options.saveTo.c_str());
    }
    return result;
}
std::vector<UserPatch> parsePatches(json_object* array) {
    std::vector<UserPatch> out;
    if (!json_object_is_type(array, json_type_array)) return out;
    for (size_t i = 0; i < json_object_array_length(array) && out.size() < 5000; ++i) {
        auto* object = json_object_array_get_idx(array, i);
        UserPatch p;
        p.id = stringField(object, "id");
        p.titleId = titleId(object, "title_id");
        // No Title ID: an app's patch, its files below switch/<app>/.
        if (p.titleId.empty()) {
            if (field(object, "title_id")) continue; // a broken Title ID, not a null one
            p.homebrewApp = stringField(object, "homebrew");
            if (!homebrew::appFolderAllowed(p.homebrewApp)) continue;
        }
        if (p.id.empty() || p.id.size() > 64 ||
            p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            continue;
        p.name = stringField(object, "name", p.id);
        p.version = textField(object, "version");
        p.gameVersion = textField(object, "game_version");
        p.author = stringField(object, "author");
        p.description = stringField(object, "description");
        auto* required = field(object, "required");
        p.required = json_object_is_type(required, json_type_boolean) && json_object_get_boolean(required);
        p.hash = stringField(object, "hash");
        p.size = positive(object, "size");
        p.files = int(std::min<uint64_t>(positive(object, "files"), INT32_MAX));
        p.updatedAt = integerField(object, "updated_at");
        if (auto* game = field(object, "game"); json_object_is_type(game, json_type_object)) {
            p.gameName = stringField(game, "name");
            if (auto* names = field(game, "names"); json_object_is_type(names, json_type_object)) {
                static const char* codes[] = {"ko", "en", "ja", "zh"};
                for (int k = 0; k < 4; ++k)
                    if (const auto name = stringField(names, codes[k]); !name.empty()) {
                        p.gameNames.resize(4);
                        p.gameNames[k] = name;
                    }
            }
            // A full URL, or a path on the shop (resolved against it when drawn).
            p.gameIcon = stringField(game, "icon");
            if (!isHttp(p.gameIcon) && p.gameIcon.rfind("/", 0) != 0) p.gameIcon.clear();
        }
        auto* kinds = field(object, "kinds");
        if (json_object_is_type(kinds, json_type_array))
            for (size_t k = 0; k < json_object_array_length(kinds); ++k) {
                auto* kind = json_object_array_get_idx(kinds, k);
                if (json_object_is_type(kind, json_type_string)) p.kinds.emplace_back(json_object_get_string(kind));
            }
        out.push_back(std::move(p));
    }
    return out;
}
std::vector<UserPatch> parsePatches(const std::string& json) {
    Error error;
    auto root = parseJson(json, error);
    return error ? std::vector<UserPatch>{} : parsePatches(field(root.get(), "patches"));
}
std::string fileTitle(const std::string& url) {
    auto name = url.substr(0, url.find_first_of("?#"));
    name = name.substr(name.find_last_of('/') + 1);
    std::string decoded;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size() && std::isxdigit(static_cast<unsigned char>(name[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(name[i + 2]))) {
            decoded += char(std::stoi(name.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else decoded += name[i];
    }
    const auto dot = decoded.find_last_of('.');
    if (dot != std::string::npos && decoded.size() - dot <= 5) decoded.erase(dot);
    std::string result;
    int depth = 0;
    for (const char c : decoded) {
        if (c == '[') ++depth;
        else if (c == ']') depth = std::max(0, depth - 1);
        else if (!depth) result += c == '_' ? ' ' : c;
    }
    const auto first = result.find_first_not_of(' '), last = result.find_last_not_of(' ');
    return first == std::string::npos ? "" : result.substr(first, last - first + 1);
}
Parsed<std::vector<Item>> parseSections(const std::string& json) {
    Error error;
    auto root = parseJson(json, error);
    if (error) return {{}, error};
    return parseSections(root.get());
}
Parsed<std::vector<Item>> parseSections(json_object* root) {
    Parsed<std::vector<Item>> result;
    auto* sections = field(root, "sections");
    if (!json_object_is_type(sections, json_type_array)) { result.error.message = tr("서버 목록 형식을 확인해 주세요."); return result; }
    // Sections may repeat a file (a tab and "all"); each file is listed once.
    std::set<std::string> seen;
    for (size_t i = 0; i < json_object_array_length(sections); ++i) {
        auto* entries = field(json_object_array_get_idx(sections, i), "items");
        if (!json_object_is_type(entries, json_type_array)) continue;
        for (size_t j = 0; j < json_object_array_length(entries); ++j) {
            auto* object = json_object_array_get_idx(entries, j);
            Item item; item.url = stringField(object, "url"); if (!isHttp(item.url) || !seen.insert(item.url).second) continue;
            item.name = stringField(object, "name", "CookingFoil");
            if (item.name.empty() || item.name == "Unknown" || item.name == "CookingFoil") {
                const auto fromFile = fileTitle(stringField(object, "url"));
                if (!fromFile.empty()) item.name = fromFile;
            }
            item.titleId = titleId(object, "title_id"); item.baseId = titleId(object, "base_title_id");
            item.type = stringField(object, "app_type", "base");
            if (item.type != "update" && item.type != "dlc") item.type = "base";
            item.version = static_cast<uint32_t>(std::min<uint64_t>(positive(object, "app_version"), UINT32_MAX));
            item.size = positive(object, "size"); item.addedAt = positive(object, "added_at"); item.releaseDate = positive(object, "release_date");
            item.rank = int(std::min<uint64_t>(positive(object, "rank"), INT32_MAX));
            item.players = int(std::min<uint64_t>(positive(object, "players"), 64));
            item.priceRegular = stringField(object, "price_regular");
            item.priceDiscount = stringField(object, "price_discount");
            item.priceDiscountEnds = stringField(object, "price_discount_ends");
            item.priceCountry = stringField(object, "price_country");
            item.requiredFirmware = textField(object, "required_firmware");
            item.installSize = positive(object, "install_size");
            item.latestVersion = static_cast<uint32_t>(std::min<uint64_t>(positive(object, "latest_version"), UINT32_MAX));
            item.dlcTotal = int(std::min<uint64_t>(positive(object, "dlc_total"), 9999));
            item.score = int(std::min<uint64_t>(positive(object, "score"), 100));
            item.scoreCount = int(std::min<uint64_t>(positive(object, "score_count"), INT32_MAX));
            item.scoreLabel = stringField(object, "score_label");
            item.scoreSource = stringField(object, "score_source");
            item.patches = int(std::min<uint64_t>(positive(object, "patches"), 999));
            if (auto* icons = field(object, "icons"); json_object_is_type(icons, json_type_object)) {
                static const char* codes[] = {"ko", "en", "ja", "zh"};
                for (int k = 0; k < 4; ++k)
                    if (const auto url = stringField(icons, codes[k]); isHttp(url)) {
                        item.icons.resize(4);
                        item.icons[k] = url;
                    }
            }
            if (auto* names = field(object, "names"); json_object_is_type(names, json_type_object)) {
                static const char* codes[] = {"ko", "en", "ja", "zh"};
                for (int k = 0; k < 4; ++k)
                    if (const auto name = stringField(names, codes[k]); !name.empty()) {
                        item.names.resize(4);
                        item.names[k] = name;
                    }
            }
            item.icon = stringField(object, "icon_url"); if (!isHttp(item.icon)) item.icon.clear();
            item.publisher = stringField(object, "publisher"); item.region = stringField(object, "region"); item.versionName = stringField(object, "version_name", stringField(object, "display_version"));
            auto* categories = field(object, "categories");
            if (json_object_is_type(categories, json_type_array)) for (size_t k = 0; k < json_object_array_length(categories); ++k) {
                auto* genre = json_object_array_get_idx(categories, k);
                if (json_object_is_type(genre, json_type_string)) item.categories.emplace_back(json_object_get_string(genre));
            }
            auto* languages = field(object, "languages");
            if (json_object_is_type(languages, json_type_array)) for (size_t k = 0; k < json_object_array_length(languages); ++k) {
                auto* code = json_object_array_get_idx(languages, k);
                if (json_object_is_type(code, json_type_string)) item.languages.emplace_back(json_object_get_string(code));
            }
            result.value.push_back(item);
        }
    }
    return result;
}
std::vector<Featured> parseFeatured(const std::string& json) {
    Error error;
    auto root = parseJson(json, error);
    return error ? std::vector<Featured>{} : parseFeatured(root.get());
}
std::vector<Featured> parseFeatured(json_object* root) {
    std::vector<Featured> result;
    auto* list = field(root, "featured");
    if (!json_object_is_type(list, json_type_array)) return result;
    for (size_t i = 0; i < json_object_array_length(list); ++i) {
        auto* object = json_object_array_get_idx(list, i);
        Featured shelf;
        shelf.id = stringField(object, "id");
        shelf.title = stringField(object, "title");
        shelf.bannerUrl = stringField(object, "banner_url");
        if (!isHttp(shelf.bannerUrl)) shelf.bannerUrl.clear();
        auto* ids = field(object, "title_ids");
        if (json_object_is_type(ids, json_type_array)) for (size_t k = 0; k < json_object_array_length(ids); ++k) {
            auto* id = json_object_array_get_idx(ids, k);
            if (!json_object_is_type(id, json_type_string)) continue;
            std::string value = json_object_get_string(id);
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return std::toupper(c); });
            if (value.size() == 16) shelf.titleIds.push_back(value);
        }
        if (!shelf.title.empty() && !shelf.titleIds.empty()) result.push_back(std::move(shelf));
    }
    return result;
}
std::map<std::string, std::string> parseNames(const std::string& json, const char* table, const std::string& lang) {
    std::map<std::string, std::string> result;
    Error error;
    auto root = parseJson(json, error);
    if (error) return result;
    auto* genres = field(root.get(), table);
    if (!json_object_is_type(genres, json_type_object)) return result;
    json_object_object_foreach(genres, key, names) {
        const auto name = stringField(names, lang.c_str());
        if (!name.empty()) result[key] = name;
    }
    return result;
}
NameTable parseNameTable(const std::string& json, const char* table) {
    Error error;
    auto root = parseJson(json, error);
    return error ? NameTable{} : parseNameTable(root.get(), table);
}
NameTable parseNameTable(json_object* root, const char* table) {
    NameTable result;
    auto* names = field(root, table);
    if (!json_object_is_type(names, json_type_object)) return result;
    json_object_object_foreach(names, key, entry) {
        if (!json_object_is_type(entry, json_type_object)) continue;
        json_object_object_foreach(entry, lang, name)
            if (json_object_is_type(name, json_type_string) && *json_object_get_string(name))
                result[key][lang] = json_object_get_string(name);
    }
    return result;
}
std::map<std::string, std::string> namesIn(const NameTable& table, const std::string& lang) {
    std::map<std::string, std::string> result;
    for (const auto& [key, names] : table) {
        auto found = names.find(lang);
        if (found == names.end()) found = names.find("en");
        if (found != names.end()) result[key] = found->second;
    }
    return result;
}
std::string tidyDescription(const std::string& text) {
    std::string result, line;
    bool gap = false;
    auto flush = [&] {
        const auto end = line.find_last_not_of(" \t\r");
        line = end == std::string::npos ? "" : line.substr(0, end + 1);
        if (line.empty()) gap = !result.empty();
        else {
            if (!result.empty()) result += gap ? "\n\n" : "\n";
            result += line;
            gap = false;
        }
        line.clear();
    };
    for (const char c : text) {
        if (c == '\n') flush();
        else line += c;
    }
    flush();
    return result;
}
Parsed<Detail> parseDetail(const std::string& json) {
    Parsed<Detail> result; auto root = parseJson(json, result.error); if (result.error) return result;
    auto& d = result.value;
    d.name = stringField(root.get(), "name"); d.publisher = stringField(root.get(), "publisher"); d.description = tidyDescription(stringField(root.get(), "description", stringField(root.get(), "intro")));
    d.notes = tidyDescription(stringField(root.get(), "notes"));
    auto* labels = field(root.get(), "categoryLabels");
    if (json_object_is_type(labels, json_type_array)) for (size_t k = 0; k < json_object_array_length(labels); ++k) {
        auto* label = json_object_array_get_idx(labels, k);
        if (json_object_is_type(label, json_type_string)) d.categoryLabels.emplace_back(json_object_get_string(label));
    }
    d.category = stringField(root.get(), "category"); d.banner = stringField(root.get(), "bannerUrl"); if (!isHttp(d.banner)) d.banner.clear();
    d.releaseDate = integerField(root.get(), "releaseDate"); d.players = integerField(root.get(), "numberOfPlayers");
    d.requiredFirmware = textField(root.get(), "requiredFirmware");
    d.dlcTotal = int(std::min<uint64_t>(positive(root.get(), "dlcTotal"), 9999));
    d.score = int(std::min<uint64_t>(positive(root.get(), "score"), 100));
    d.scoreCount = int(std::min<uint64_t>(positive(root.get(), "scoreCount"), INT32_MAX));
    d.scoreLabel = stringField(root.get(), "scoreLabel");
    d.patches = parsePatches(field(root.get(), "patches"));
    // ratingContent may be a single string or a list of descriptors.
    auto* rating = field(root.get(), "ratingContent");
    if (json_object_is_type(rating, json_type_string)) d.ratingContent = json_object_get_string(rating);
    else if (json_object_is_type(rating, json_type_array))
        for (size_t k = 0; k < json_object_array_length(rating); ++k) {
            auto* part = json_object_array_get_idx(rating, k);
            if (!json_object_is_type(part, json_type_string)) continue;
            if (!d.ratingContent.empty()) d.ratingContent += ", ";
            d.ratingContent += json_object_get_string(part);
        }
    auto* price = field(root.get(), "price");
    if (json_object_is_type(price, json_type_object)) {
        d.priceRegular = stringField(price, "regular");
        d.priceDiscount = stringField(price, "discount");
        d.priceCountry = stringField(price, "country");
    }
    auto* shots = field(root.get(), "screenshots");
    if (json_object_is_type(shots, json_type_array)) for (size_t i = 0; i < json_object_array_length(shots); ++i) {
        auto* value = json_object_array_get_idx(shots, i);
        if (json_object_is_type(value, json_type_string) && isHttp(json_object_get_string(value))) d.screenshots.emplace_back(json_object_get_string(value));
    }
    return result;
}
Session::Session(Server server, State state, std::string path, const std::atomic<bool>* cancel) : server_(std::move(server)), state_(std::move(state)), path_(std::move(path)), cancel_(cancel) {}
Parsed<PairingInfo> parsePairingInfo(const std::string& json, const Server& server) {
    Parsed<PairingInfo> result;
    auto root = parseJson(json, result.error); if (result.error) return result;
    if (stringField(root.get(), "status") != "pending") return result;
    const auto url = stringField(root.get(), "pairUrl");
    // Newer servers hand out /pair/ (login first, then back); older ones /admin/pair/.
    auto prefix = server.url + "/pair/";
    if (url.rfind(prefix, 0) != 0) prefix = server.url + "/admin/pair/";
    const auto code = stringField(root.get(), "pairCode");
    if (url.size() != prefix.size() + 32 || url.rfind(prefix, 0) != 0 ||
        url.substr(prefix.size()).find_first_not_of("0123456789abcdef") != std::string::npos ||
        code.size() != 8 || code.find_first_not_of("0123456789ABCDEF") != std::string::npos) return result;
    result.value = {url, code, std::max<int64_t>(0, integerField(root.get(), "expiresAt"))};
    return result;
}
std::string cleanPairMessage(const std::string& text) {
    // Line breaks and other control characters read as spaces, and a run of
    // spaces as one.
    std::string line;
    for (const char c : text) {
        const bool space = static_cast<unsigned char>(c) <= 0x20 || c == 0x7f;
        if (!space) line += c;
        else if (!line.empty() && line.back() != ' ') line += ' ';
    }
    const auto first = line.find_first_not_of(' ');
    if (first == std::string::npos) return "";
    line = line.substr(first, line.find_last_not_of(' ') - first + 1);
    size_t characters = 0, end = 0;
    while (end < line.size() && characters < PairMessageLimit) {
        ++end;
        while (end < line.size() && (static_cast<unsigned char>(line[end]) & 0xC0) == 0x80) ++end;
        ++characters;
    }
    line.resize(end);
    return line.substr(0, line.find_last_not_of(' ') + 1);
}
std::string pairRequestBody(const std::string& deviceKey, const std::string& message) {
    const auto note = cleanPairMessage(message);
    if (note.empty()) return "";
    Json body(json_object_new_object());
    json_object_object_add(body.get(), "deviceKey", json_object_new_string(deviceKey.c_str()));
    json_object_object_add(body.get(), "message", json_object_new_string(note.c_str()));
    return json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN | JSON_C_TO_STRING_NOSLASHESCAPE);
}
void Session::setPairMessage(const std::string& message) {
    const auto note = cleanPairMessage(message);
    if (note.empty()) state_.pairMessages.erase(server_.url);
    else state_.pairMessages[server_.url] = note;
}
bool Session::pair(Error& error) {
    const auto url = server_.url + "/api/pair/status";
    const auto response = get(url, authHeaders(server_, state_, url), "", cancel_);
    if (response.status == 404) { pairingPending_ = false; pairingInfo_ = {}; return true; }
    if (!response.error.empty() || response.status != 200) { error.message = response.error.empty() ? tr("기기 승인 상태를 확인하지 못했습니다.") : response.error; return false; }
    auto root = parseJson(response.body, error); if (error) return false;
    const auto status = stringField(root.get(), "status");
    if (status == "pending") {
        pairingPending_ = true; pairingInfo_ = parsePairingInfo(response.body, server_).value;
        sendPairMessage();
        error.message = tr("서버 웹페이지에서 이 기기를 승인하면 연결합니다."); return false;
    }
    if (status != "approved") { error.message = tr("기기 승인 응답을 확인해 주세요."); return false; }
    const auto key = stringField(root.get(), "accessKey");
    if (!key.empty()) {
        // Start from the file: settings chosen since this session began
        // (language, install checks) must survive the key being added.
        auto saved = loadState(path_);
        auto updated = saved.error || saved.value.deviceKey != state_.deviceKey ? state_ : saved.value; updated.accessKeys[server_.url] = key;
        if (!saveState(path_, updated, error)) { error.message = tr("발급받은 키를 저장하지 못했습니다. 상태 파일의 저장 위치를 확인해 주세요."); return false; }
        state_ = std::move(updated);
    }
    pairingPending_ = false; pairingInfo_ = {}; return true;
}
void Session::sendPairMessage() {
    // The status poll already knocks; the request goes out only to carry a
    // note the player typed, once, and again when it changes.
    const auto note = state_.pairMessages.find(server_.url);
    const auto body = pairRequestBody(state_.deviceKey, note == state_.pairMessages.end() ? "" : note->second);
    if (body.empty()) { pairMessageSent_.clear(); return; }
    if (body == pairMessageSent_) return;
    const auto url = server_.url + "/api/pair/request";
    auto headers = authHeaders(server_, state_, url);
    headers.push_back("Content-Type: application/json");
    PostOptions options;
    options.body = body;
    options.timeout = 15;
    const auto sent = post(url, headers, options, cancel_);
    // A failure is tried again on the next poll; approval works without it.
    if (sent.error.empty() && sent.status >= 200 && sent.status < 300) pairMessageSent_ = body;
    else logLine("pair message not sent: " + (sent.error.empty() ? "HTTP " + std::to_string(sent.status) : sent.error));
}
std::set<std::string> parseFeatures(const std::string& json) {
    std::set<std::string> out;
    Error ignored;
    auto root = parseJson(json, ignored);
    auto* list = root ? field(root.get(), "features") : nullptr;
    if (!json_object_is_type(list, json_type_array)) return out;
    for (size_t i = 0; i < json_object_array_length(list); ++i) {
        auto* value = json_object_array_get_idx(list, i);
        if (json_object_is_type(value, json_type_string)) out.insert(json_object_get_string(value));
    }
    return out;
}
bool Session::refresh(Error& error) {
    error = {};
    if (!pair(error)) return false;
    {
        // What the menu offers follows the server: asked on every refresh,
        // so content added on the server shows without a restart.
        const auto info = server_.url + "/api/shop/info";
        const auto response = get(info, authHeaders(server_, state_, info), "", cancel_);
        if (response.status == 200) features_ = parseFeatures(response.body);
        else if (response.status == 404) features_.clear();
    }
    const auto url = server_.url + "/api/shop/sections";
    // How long the first screen waits, in the log: the list's download,
    // then reading it.
    const auto asked = std::chrono::steady_clock::now();
    auto response = get(url, authHeaders(server_, state_, url), etag_, cancel_);
    const auto since = [](std::chrono::steady_clock::time_point from) {
        return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from).count()) + "ms";
    };
    if (response.status == 200)
        logLine("catalog download " + since(asked) + " " + std::to_string(response.body.size() >> 10) + "KB");
    if (!response.error.empty()) { error.message = response.error; return false; }
    unchanged_ = response.status == 304;
    if (unchanged_) return true;
    if (response.status != 200) { error.message = trf("서버 연결을 확인해 주세요. HTTP {0}", {std::to_string(response.status)}); return false; }
    // The catalog is several MB of JSON and its parsed tree several times
    // that: parse it once, drop the text right away, read every table from
    // the one tree, then let the tree go before anything else allocates.
    Json root = parseJson(response.body, error);
    std::string().swap(response.body);
    if (error) return false;
    auto parsed = parseSections(root.get()); if (parsed.error) { error = parsed.error; return false; }
    items_.clear(); items_.shrink_to_fit();
    items_ = std::move(parsed.value);
    featured_ = parseFeatured(root.get());
    genres_ = parseNameTable(root.get(), "genres");
    scoreLabels_ = parseNameTable(root.get(), "score_labels");
    logLine("catalog read " + since(asked) + " total, " + std::to_string(items_.size()) + " items");
    etag_ = response.etag;
    return true;
}
Parsed<Detail> Session::detail(const std::string& baseId) {
    Parsed<Detail> result;
    if (baseId.size() != 16 || baseId.find_first_not_of("0123456789ABCDEF") != std::string::npos) return result;
    // Genre names (category, categoryLabels) come back in the UI language.
    const auto url = server_.url + "/api/title/" + baseId + "?lang=" + languageCode();
    const auto response = get(url, authHeaders(server_, state_, url), "", cancel_);
    if (response.status == 404) return result;
    if (!response.error.empty() || response.status != 200) { result.error.message = tr("상세 정보를 불러오지 못했습니다."); return result; }
    return parseDetail(response.body);
}
Parsed<std::vector<UserPatch>> Session::patches() {
    Parsed<std::vector<UserPatch>> result;
    const auto url = server_.url + "/api/patches";
    const auto response = get(url, authHeaders(server_, state_, url), patchesEtag_, cancel_);
    if (response.status == 304) { result.value = patches_; return result; }
    patchesRetryAfter_ = 0;
    if (response.status == 404) return result; // a server without user patches
    if (response.status == 503) {
        // The server's first scan of a large patch folder; it says when to ask again.
        Error ignored;
        const auto body = parseJson(response.body, ignored);
        patchesRetryAfter_ = int(std::clamp<int64_t>(body ? integerField(body.get(), "retryAfter", 30) : 30, 5, 300));
        result.error.message = tr("서버가 패치를 읽고 있습니다. 잠시 뒤 다시 불러옵니다.");
        return result;
    }
    if (!response.error.empty() || response.status != 200) { result.error.message = tr("패치 목록을 불러오지 못했습니다."); return result; }
    patches_ = parsePatches(response.body);
    result.value = patches_;
    // Still scanning: the list so far, and when to ask for the rest. Its
    // ETag is not kept, so that ask gets the fuller list.
    Error ignored;
    const auto body = parseJson(response.body, ignored);
    auto* scanning = body ? field(body.get(), "scanning") : nullptr;
    if (scanning && json_object_get_boolean(scanning)) {
        // The wait comes in the Retry-After header.
        patchesRetryAfter_ = std::clamp(response.retryAfter ? response.retryAfter : 30, 5, 300);
        return result;
    }
    patchesEtag_ = response.etag;
    return result;
}
bool Session::downloadPatch(const UserPatch& patch, const std::string& dir, std::atomic<uint64_t>& received, Error& error) {
    const auto url = server_.url + "/api/patches/" + patch.id + "/download";
    // The same patch build (its hash, which the server also sends as the
    // ETag) continues from the last whole file an earlier try left in dir.
    PartInfo wanted;
    wanted.url = url;
    wanted.tag = patch.hash.empty() ? patch.version + "@" + std::to_string(patch.updatedAt) : patch.hash;
    wanted.size = patch.size;
    const auto record = dir + "/" + ResumeFile;
    const auto kept = parsePartInfo(readFile(record));
    const uint64_t from = samePart(kept, wanted) ? kept.offset : 0;
    std::error_code ec;
    if (!from) std::filesystem::remove_all(dir, ec);
    else logLine("patch " + patch.id + " resumes at " + std::to_string(from));
    const auto fetch = rangedTar(url, authHeaders(server_, state_, url), from, from ? kept.etag : "",
        [&](uint64_t at) {
            std::error_code ignored;
            if (!at) std::filesystem::remove_all(dir, ignored);
            makeDirectories(dir);
            return std::make_unique<TarWriter>(dir, [&patch](const std::string& path) { return patchPathAllowed(patch, path); });
        },
        [&](uint64_t boundary, const std::string& etag) {
            auto info = wanted;
            info.offset = boundary;
            info.etag = etag;
            std::string ignored;
            atomicWrite(record, serializePartInfo(info), ignored);
        },
        received, cancel_);
    // A drop or a cancel keeps what arrived for the next try; anything else
    // starts that try over.
    const auto fail = [&](const std::string& message, bool keep) {
        std::error_code ignored;
        if (!keep) std::filesystem::remove_all(dir, ignored);
        error.message = message;
        return false;
    };
    if (!fetch.tarError.empty()) return fail(fetch.tarError, false);
    if (!fetch.response.error.empty()) return fail(fetch.response.error, true);
    if (!rangedOk(fetch.response))
        return fail(trf("패치를 받지 못했습니다. HTTP {0}", {std::to_string(fetch.response.status)}), false);
    if (!fetch.complete) return fail(tr("패치를 받다가 끊겼습니다. 다시 받습니다."), false);
    std::filesystem::remove(record, ec);
    return true;
}
}
