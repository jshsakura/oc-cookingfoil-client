#pragma once
#include <set>
#include <atomic>
#include <map>
#include "core/State.hpp"
#include "core/Patches.hpp"
#include "core/I18n.hpp"
#include <functional>
struct json_object;
namespace foil::net {
struct Response { long status = 0; std::string body, etag, error; };
std::vector<std::string> authHeaders(const Server& server, const State& state, const std::string& url);
bool internetConnected();
const char* offlineMessage();
std::string connectionError(int curlCode, const std::string& url);
Response get(const std::string& url, const std::vector<std::string>& headers = {}, const std::string& etag = "", const std::atomic<bool>* cancel = nullptr);
// Streams a body to onData as it arrives, only for a 200 response. onData
// returning false aborts. No overall timeout, only a stall timeout.
Response stream(const std::string& url, const std::vector<std::string>& headers,
                const std::function<bool(const char*, size_t)>& onData, const std::atomic<bool>* cancel = nullptr);
// A POST carrying `body`, or with `uploadFile` that file's bytes. With
// `saveTo` a 2xx body is written to that file (removed again on failure)
// and only an error body is kept in Response::body. Redirects are not
// followed, as in get().
struct PostOptions {
    std::string body, uploadFile, saveTo;
    long timeout = 60; // seconds; 0 for no limit, only a 30 second stall
};
Response post(const std::string& url, const std::vector<std::string>& headers, const PostOptions& options,
              const std::atomic<bool>* cancel = nullptr);
// `patches` of GET /api/patches or of a title's detail.
std::vector<UserPatch> parsePatches(json_object* array);
std::vector<UserPatch> parsePatches(const std::string& json);
// Readable title from a file URL: decoded, without [tags] or extension.
std::string fileTitle(const std::string& url);
// Admin-picked shelf: base title ids in the admin's order (feature: featured).
struct Featured {
    std::string id, title, bannerUrl;
    std::vector<std::string> titleIds;
};
std::vector<Featured> parseFeatured(const std::string& json);
Parsed<std::vector<Item>> parseSections(const std::string& json);
Parsed<std::vector<Item>> parseSections(json_object* root);
std::vector<Featured> parseFeatured(json_object* root);
// Top-level name tables (`genres`, `score_labels`): key -> {en, ko, ja, zh};
// returns key -> name in `lang`.
std::map<std::string, std::string> parseNames(const std::string& json, const char* table, const std::string& lang = "ko");
// The same table for every language at once: key -> lang -> name.
using NameTable = std::map<std::string, std::map<std::string, std::string>>;
NameTable parseNameTable(const std::string& json, const char* table);
NameTable parseNameTable(json_object* root, const char* table);
// key -> name in lang; a key without that language falls back to English.
std::map<std::string, std::string> namesIn(const NameTable& table, const std::string& lang);
inline std::map<std::string, std::string> parseGenres(const std::string& json, const std::string& lang = "ko") {
    return parseNames(json, "genres", lang);
}
struct Detail {
    std::string name, publisher, description, category, banner;
    std::vector<std::string> categoryLabels; // genre names in the UI language
    int releaseDate = 0, players = 0;
    std::vector<std::string> screenshots;
    // eShop price as the store formats it ("64,800원"); empty when unknown.
    std::string priceRegular, priceDiscount, priceCountry;
    std::string requiredFirmware, ratingContent;
    int dlcTotal = 0;
    int score = 0, scoreCount = 0; // review score 0-100 and its vote count
    std::string scoreLabel;
    std::vector<UserPatch> patches;
};
std::string tidyDescription(const std::string& text);
Parsed<Detail> parseDetail(const std::string& json);
struct PairingInfo {
    std::string url, code;
    int64_t expiresAt = 0;
};
Parsed<PairingInfo> parsePairingInfo(const std::string& json, const Server& server);
// /api/shop/info's "features": what this server offers beyond the catalog
// (retro, homebrew, ...). Empty for a reply without them.
std::set<std::string> parseFeatures(const std::string& json);
class Session {
public:
    Session(Server server, State state, std::string statePath, const std::atomic<bool>* cancel = nullptr);
    bool refresh(Error& error);
    Parsed<Detail> detail(const std::string& baseId);
    // Every user patch on the server; unchanged lists come back from cache.
    Parsed<std::vector<UserPatch>> patches();
    // Seconds the server asked to wait after a 503 from patches() (it is
    // still reading its patch folder); 0 otherwise.
    int patchesRetryAfter() const { return patchesRetryAfter_; }
    // Unpacks a patch's tar into dir. `received` counts bytes for progress.
    bool downloadPatch(const UserPatch& patch, const std::string& dir, std::atomic<uint64_t>& received, Error& error);
    const std::vector<Item>& items() const { return items_; }
    // Hands the catalog over instead of copying it: the app keeps the only
    // copy (thousands of items). True after a 304, when nothing changed.
    std::vector<Item> takeItems() { return std::move(items_); }
    bool unchanged() const { return unchanged_; }
    const std::set<std::string>& features() const { return features_; }
    const std::vector<Featured>& featured() const { return featured_; }
    // Names in the current interface language.
    std::map<std::string, std::string> genres() const { return namesIn(genres_, languageCode()); }
    std::map<std::string, std::string> scoreLabels() const { return namesIn(scoreLabels_, languageCode()); }
    const State& state() const { return state_; }
    const Server& server() const { return server_; }
    bool pairingPending() const { return pairingPending_; }
    const PairingInfo& pairingInfo() const { return pairingInfo_; }
private:
    bool pair(Error& error);
    Server server_;
    State state_;
    std::string path_, etag_, patchesEtag_;
    int patchesRetryAfter_ = 0;
    std::vector<UserPatch> patches_;
    std::vector<Item> items_;
    bool unchanged_ = false;
    std::set<std::string> features_;
    std::vector<Featured> featured_;
    NameTable genres_, scoreLabels_;
    bool pairingPending_ = false;
    PairingInfo pairingInfo_;
    const std::atomic<bool>* cancel_ = nullptr;
};
}
