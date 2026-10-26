#pragma once
#include <set>
#include <atomic>
#include <map>
#include "core/State.hpp"
#include "net/Socket.hpp"
#include "core/Patches.hpp"
#include "core/I18n.hpp"
#include <functional>
struct json_object;
namespace foil::net {
struct Response {
    std::string url; // what was asked
    long status = 0;
    std::string body, etag, error;
    int retryAfter = 0; // the Retry-After header in seconds; 0 when absent or a date
    std::string contentRange; // a 206's Content-Range header
    uint64_t contentLength = 0; // 0 when absent
    std::string session; // X-CookingFoil-Session: why the session token was refused
    std::string enroll;  // X-CookingFoil-Enroll: "invalid" when an enroll token was refused
    std::string auth;    // X-CookingFoil-Auth: why a 401 refused (no-access-key, access-key-wrong, ...)
    int curl = 0;        // curl's result; 7 (CURLE_COULDNT_CONNECT): nothing was sent
    int64_t date = 0;    // the Date header, Unix seconds; 0 when absent
    bool directNamed = false; // X-CookingFoil-Download-Origin came with it
};
// The server's keys go to the server only; anything else (a picture on
// another host) gets no credentials at all.
std::vector<std::string> authHeaders(const Server& server, const State& state, const std::string& url);
// After the server's info or list answered (`url`): a success over https
// without X-CookingFoil-Download-Origin means the server turned its direct
// address off, so the address and token held from before are dropped.
void followDirectNaming(const std::string& url, const Response& response);
// A file download: as authHeaders, and on the server's download host (https)
// its download token too. Only downloads take it, so no other host sees it.
std::vector<std::string> downloadHeaders(const Server& server, const State& state, const std::string& url);
// GET /api/session with the sign-in, and handing that ask to the requests
// to this server (net/Socket's session token).
SessionGrant askSessionToken(const Server& server, const State& state);
void offerSessionFor(const Server& server, const State& state);
bool internetConnected();
// The console's link: -1 offline, else signal bars 0-3 (wired reads as 3).
int signalStrength();
const char* offlineMessage();
std::string connectionError(int curlCode, const std::string& url);
// A secure connection failed since the last call: the console clock may be
// off (ui/ClockFix compares it with internet time).
bool takeCertificateFailure();
// Each server is asked to take the console's key once a run (Session); this
// starts a new run's worth (tests).
void forgetRebindAttempts();
// Asks the server for a fresh download token (it comes with any answer).
long renewDownloadToken(const Server& server, const State& state); // the reply's status
Response get(const std::string& url, const std::vector<std::string>& headers = {}, const std::string& etag = "", const std::atomic<bool>* cancel = nullptr);
// Streams a body to onData as it arrives, only for a 200 response. onData
// returning false aborts. No overall timeout, only a stall timeout.
Response stream(const std::string& url, const std::vector<std::string>& headers,
                const std::function<bool(const char*, size_t)>& onData, const std::atomic<bool>* cancel = nullptr);
// What a streamed reply's body is for, decided once its status and headers
// are in: Take hands it to onData, Drop reads past it (an error page), Stop
// ends the transfer with no error of its own.
enum class Start { Take, Drop, Stop };
Response stream(const std::string& url, const std::vector<std::string>& headers,
                const std::function<Start(const Response&)>& start,
                const std::function<bool(const char*, size_t)>& onData, const std::atomic<bool>* cancel = nullptr);
// A POST carrying `body`, or with `uploadFile` that file's bytes. With
// `saveTo` a 2xx body is written to that file (removed again on failure)
// and only an error body is kept in Response::body. Redirects are not
// followed, as in get().
struct PostOptions {
    std::string body, uploadFile, saveTo;
    std::string method; // "PUT" or "DELETE" in place of POST; empty for POST
    long timeout = 60; // seconds; 0 for no limit, only a 30 second stall
};
Response post(const std::string& url, const std::vector<std::string>& headers, const PostOptions& options,
              const std::atomic<bool>* cancel = nullptr);
// `patches` of GET /api/patches or of a title's detail.
std::vector<UserPatch> parsePatches(json_object* array);
std::vector<UserPatch> parsePatches(const std::string& json);
// One patch as parsePatches() reads it back, for a queued patch kept in 다운로드.
std::string serializePatch(const UserPatch& patch);
// Readable title from a file URL: decoded, without [tags] or extension.
std::string fileTitle(const std::string& url);
// Admin-picked shelf: base title ids in the admin's order (feature: featured).
struct Featured {
    std::string id, title, bannerUrl;
    std::string icon; // a name from the client's icon set; empty for the default
    std::vector<std::string> titleIds;
};
std::vector<Featured> parseFeatured(const std::string& json);
Parsed<std::vector<Item>> parseSections(const std::string& json);
std::vector<Featured> parseFeatured(json_object* root);
// Top-level name tables (`genres`, `score_labels`): key -> {en, ko, ja, zh};
// returns key -> name in `lang`.
std::map<std::string, std::string> parseNames(const std::string& json, const char* table, const std::string& lang = "ko");
// The same table for every language at once: key -> lang -> name.
using NameTable = std::map<std::string, std::map<std::string, std::string>>;
// The whole /api/shop/sections answer, read one game at a time.
struct Catalog {
    std::vector<Item> items;
    std::vector<Featured> featured;
    NameTable genres, scoreLabels;
    labels::Book labels; // the cards' label dictionary
};
Parsed<Catalog> parseCatalog(const std::string& json);
NameTable parseNameTable(const std::string& json, const char* table);
NameTable parseNameTable(json_object* root, const char* table);
// key -> name in lang; a key without that language falls back to English.
std::map<std::string, std::string> namesIn(const NameTable& table, const std::string& lang);
inline std::map<std::string, std::string> parseGenres(const std::string& json, const std::string& lang = "ko") {
    return parseNames(json, "genres", lang);
}
// One file of a title the server's check flagged (detail.integrity[]): its
// name, "modified" or "damaged" and why, whether an install surely fails,
// and the label and help the server words in the language asked. The help
// body is paragraphs split by blank lines.
struct FileCheck {
    std::string file, verdict, reason, label, title, body;
    bool certain = false;
    // 1 modified, 2 maybe damaged, 3 damaged, as Item::integrity.
    int level() const { return verdict == "modified" ? 1 : certain ? 3 : 2; }
};
struct Detail {
    std::string name, publisher, description, category, banner;
    std::string notes; // the operator's own notes: what else to install, sizes, which build
    std::vector<std::string> categoryLabels; // genre names in the UI language
    int releaseDate = 0, players = 0;
    std::vector<std::string> screenshots;
    // eShop price as the store formats it ("64,800원"); empty when unknown.
    std::string priceRegular, priceDiscount, priceCountry;
    std::string requiredFirmware, keyFirmware, ratingContent;
    int dlcTotal = 0;
    int score = 0, scoreCount = 0; // review score 0-100 and its vote count
    std::string scoreLabel;
    std::vector<UserPatch> patches;
    std::vector<FileCheck> integrity;
};
std::string tidyDescription(const std::string& text);
Parsed<Detail> parseDetail(const std::string& json);
struct PairingInfo {
    std::string url, code;
    int64_t expiresAt = 0;
    // Why the download's token did not connect this console (X-CookingFoil-Enroll:
    // "used", "expired", "revoked", "limit": as many consoles by download this
    // hour as the account may; "other-account" from its 409); empty when there
    // was none or it was unknown.
    std::string refusal;
    // The account holds as many consoles as it may (limit: "device"): waiting
    // approves nothing until one is removed.
    bool deviceLimit = false;
    int deviceLimitCount = 0; // its deviceLimit: how many; 0 when not said
};
Parsed<PairingInfo> parsePairingInfo(const std::string& json, const Server& server);
// The note sent with an approval request, read as the server reads it: line
// breaks and control characters become spaces, runs of spaces one, trimmed,
// at most PairMessageLimit characters (code points). Empty for none.
constexpr size_t PairMessageLimit = 100;
std::string cleanPairMessage(const std::string& text);
// POST /api/pair/request's JSON body, {"deviceKey": ..., "message": ...,
// "pairSecret": ...}, each of the last two only when there is one; empty
// when there is neither, and then nothing is sent.
// enrollToken: a download's token not yet spent, so the server can approve
// the request under that download's account. label: the console's own
// nickname, which names it in the server's approval queue; sent only with
// one of the others.
std::string pairRequestBody(const std::string& deviceKey, const std::string& message, const std::string& secret = "",
                            const std::string& enrollToken = "", const std::string& label = "");
// The console's nickname (System Settings), sent as `label` when it asks a
// server to take it; set once at start, before any session.
void setConsoleName(std::string name);
// /api/shop/info's "features": what this server offers beyond the catalog
// (retro, homebrew, ...). Empty for a reply without them.
std::set<std::string> parseFeatures(const std::string& json);
// Its client_strings.languages: every language the server's dictionary has,
// [{"code","name","base"}], the ones the app ships too. A row without a code
// or a name is left out, and so is one whose name the console cannot draw.
std::vector<ExtraLanguage> parseClientLanguages(const std::string& json);
// Its download.maxConnections: how many connections the server would have a
// console open (0 when it does not say).
int parseMaxConnections(const std::string& json);
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
    const std::vector<ExtraLanguage>& clientLanguages() const { return clientLanguages_; }
    const std::vector<Featured>& featured() const { return featured_; }
    // Names in the current interface language.
    std::map<std::string, std::string> genres() const { return namesIn(genres_, languageCode()); }
    std::map<std::string, std::string> scoreLabels() const { return namesIn(scoreLabels_, languageCode()); }
    const labels::Book &labels() const { return labels_; }
    const State& state() const { return state_; }
    const Server& server() const { return server_; }
    // The server's own id (/api/shop/info server_id): two addresses of one
    // server give the same. Empty until it answers, or from a server without one.
    const std::string& serverId() const { return serverId_; }
    // The newest firmware whose content the server can re-encrypt (nca-rewrap); empty: not said.
    const std::string& rewrapMaxFirmware() const { return rewrapMaxFirmware_; }
    bool pairingPending() const { return pairingPending_; }
    const PairingInfo& pairingInfo() const { return pairingInfo_; }
    // The note for this server's approval request, from the app; a changed
    // one goes out with the next refresh while approval is pending.
    void setPairMessage(const std::string& message);
    // config.json, from which a spent enroll token is taken out.
    void setConfigPath(std::string path) { configPath_ = std::move(path); }
    // Tests: enroll tokens refused earlier this run may be tried again.
    static void forgetEnrollAttempts();
private:
    // The enroll token from the website's download, spent for this server's
    // access key before anything else; false (with the reason) when refused.
    // replacing: spent even with a key kept, which the server no longer approves.
    bool enroll(Error& error, bool replacing = false);
    bool keepEnrolledKey(const std::string& accessKey, const std::string& device, Error& error);
    // The file's state (settings chosen since this session began stay), with
    // the key this run holds but has not written yet. Under stateFile.
    State stateFromFile();
    // An issued key, written with a few tries (another write may hold the
    // file a while). None taking, it is held for this run (keyUnsaved_) and
    // written at the next refresh: the console connects all the same, since
    // the server gives the key once. True when written. Under stateFile.
    bool saveKey(const std::function<void(State&)>& change, int tries);
    // The held key reached the file: a spent enroll token leaves config.json.
    void keyWritten();
    bool pair(Error& error);
    void rebind();
    void sendPairMessage();
    // The kept access key and pair secret for this server go (the console
    // was taken back), and its session token with them.
    void forgetAccessKey();
    void forgetPasswordOnceKeyed();
    // A list refused with 401: approval again when its reason says the key
    // is no good, a new sign-in when it was the session token. True when the
    // list should be asked once more; error set when approval is pending or
    // failed.
    bool answerRefusal(const Response& reply, Error& error);
    bool pairPasswordOnly_ = false; // a /api/pair/* reply said passwordOnly
    // This server's pair secret, made and saved first when the console has no
    // key from it yet; empty when it has one or the secret could not be saved.
    std::string pairSecret();
    Server server_;
    State state_;
    std::string path_, etag_, patchesEtag_, configPath_, serverId_, rewrapMaxFirmware_;
    int patchesRetryAfter_ = 0;
    std::vector<UserPatch> patches_;
    std::vector<Item> items_;
    bool unchanged_ = false;
    std::set<std::string> features_;
    std::vector<ExtraLanguage> clientLanguages_;
    std::vector<Featured> featured_;
    NameTable genres_, scoreLabels_;
    labels::Book labels_;
    bool pairingPending_ = false;
    PairingInfo pairingInfo_;
    std::string enrollRefusal_; // PairingInfo::refusal, kept across polls
    std::string pairMessageSent_; // the body the server last took
    // The server holds another secret for this console's request (409): an
    // admin must clear that request before a QR code means anything.
    bool pairMismatch_ = false;
    bool keyUnsaved_ = false;
    bool enrollUnsaved_ = false; // the key came from the enroll token, kept until written
    const std::atomic<bool>* cancel_ = nullptr;
};
}
