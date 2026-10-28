#include "net/Socket.hpp"
#include "net/Segments.hpp"
#include "net/Tuner.hpp"
#include "core/Config.hpp"
#include "core/State.hpp"
#include "core/Log.hpp"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <curl/curl.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <utility>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::net {
namespace {
std::atomic<int> window{0};
std::atomic<uint64_t> received{0};
// What one server said about its downloads, kept apart from every other
// server's: a reply from one never touches another's token or address.
struct Link {
  std::string token;
  std::string direct; // where the server says the token goes; empty until it says
  bool rejected = false, signInRefused = false;
  std::function<long()> renewer;
  std::chrono::steady_clock::time_point lastAsk;
  bool askedOnce = false;
  std::string restartId;
  std::chrono::steady_clock::time_point restartUntil;
  bool restarting = false;
  uint64_t sessionRefusals = 0;
  int activeDownloads = 0, recommended = 0;
  // One ask for a token at a time, per server: the others wait for its
  // token, and a slow server holds up no other.
  std::shared_ptr<std::mutex> renewing = std::make_shared<std::mutex>();
};
std::mutex linkLock;
std::map<std::string, Link> links;      // by the server's own origin
std::map<std::string, std::string> lent; // a host off a server given its token, to that server
std::atomic<uint64_t> restarts{0};
std::set<std::string> revoked; // servers that took this console back, until taken; under linkLock
bool isConfigured(const std::string &origin) {
  const auto known = knownServers();
  return known && std::any_of(known->begin(), known->end(), [&](const std::string &s) { return originOf(s) == origin; });
}
// Under linkLock: the server `url` belongs to. A configured server's own
// address is always its own; else the server that named it as its direct
// address, or that gave a host there its token; else the address itself.
// Empty for a URL with no origin, which belongs to none.
std::string ownerOf(const std::string &url) {
  const auto origin = originOf(url);
  if (origin.empty() || isConfigured(origin)) return origin;
  for (const auto &[server, link] : links)
    if (link.direct == origin && server != origin) return server;
  const auto host = lent.find(origin);
  return host == lent.end() ? origin : host->second;
}
// A server's state: read (a server not heard from reads as one with
// nothing said) or changed.
template <typename Read>
auto peekLink(const std::string &url, Read read) {
  std::lock_guard<std::mutex> held(linkLock);
  const auto found = links.find(ownerOf(url));
  return found == links.end() ? read(Link{}) : read(found->second);
}
template <typename Change>
auto changeLink(const std::string &url, Change change) {
  std::lock_guard<std::mutex> held(linkLock);
  const auto owner = ownerOf(url);
  if (owner.empty()) {
    Link none;
    return change(none);
  }
  return change(links[owner]);
}
// A reply's header block, in the thread curl reads it on: whether it is a
// success so far and has no restart header.
thread_local bool replyOk = false;
size_t onRestartHeader(char *data, size_t size, size_t count, void *curl) {
  char *url = nullptr;
  curl_easy_getinfo(static_cast<CURL *>(curl), CURLINFO_EFFECTIVE_URL, &url);
  heardHeader(url ? url : "", data, size * count);
  return size * count;
}
} // namespace
std::string serverOf(const std::string &url) {
  std::lock_guard<std::mutex> held(linkLock);
  return ownerOf(url);
}
void heardHeader(const std::string &url, const char *data, size_t bytes) {
  if (originOf(url).empty()) return; // from no server
  std::string line(data, bytes);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
    line.pop_back();
  if (line.rfind("HTTP/", 0) == 0) {
    const auto space = line.find(' ');
    const long status = space == std::string::npos ? 0 : std::strtol(line.c_str() + space + 1, nullptr, 10);
    replyOk = status >= 200 && status < 300;
    return;
  }
  if (line.empty()) { // the block ended: a success with no restart header means it is back
    if (replyOk) changeLink(url, [](Link &link) { link.restarting = false; });
    replyOk = false;
    return;
  }
  const auto colon = line.find(':');
  if (colon == std::string::npos) return;
  std::string key = line.substr(0, colon);
  for (auto &c : key) c = char(std::tolower(static_cast<unsigned char>(c)));
  const auto trimmed = [&] {
    auto value = line.substr(colon + 1);
    const auto first = value.find_first_not_of(" \t");
    return first == std::string::npos ? std::string() : value.substr(first, value.find_last_not_of(" \t") - first + 1);
  };
  if (key == "x-cookingfoil-download-token") {
    const auto value = trimmed();
    if (!value.empty() && value.find_first_of(" \t\r\n") == std::string::npos)
      changeLink(url, [&](Link &link) {
        link.token = value;
        link.rejected = link.signInRefused = false;
      });
    return;
  }
  if (key == "x-cookingfoil-download-origin") {
    const auto value = trimmed();
    // Kept in the one form originOf gives ("https://Host:443" is
    // "https://host"): every check compares with originOf(url), and one
    // that missed would send the server's own keys there.
    const auto origin = value.rfind("https://", 0) == 0 && value.find_first_of(" \t\r\n/", 8) == std::string::npos
                            ? originOf(value)
                            : std::string();
    // Never another server's address, its own or the direct one it named:
    // that would take its replies and send it this server's token.
    if (!origin.empty()) {
      std::lock_guard<std::mutex> held(linkLock);
      const auto owner = ownerOf(url);
      const bool taken = (origin != owner && (isConfigured(origin) || links.count(origin))) ||
                         std::any_of(links.begin(), links.end(), [&](const auto &l) { return l.first != owner && l.second.direct == origin; });
      if (!owner.empty() && !taken) links[owner].direct = origin;
    }
    return;
  }
  // How many connections the server takes from a console now; the next
  // download opens that many (as /api/library/info's download.maxConnections).
  if (key == "x-cookingfoil-max-connections") {
    const long most = std::strtol(trimmed().c_str(), nullptr, 10);
    if (most > 0 && most <= 64) recommendConnections(url, int(most));
    return;
  }
  // Consoles downloading from the server now, this one included: the
  // connections are shared out among them.
  if (key == "x-cookingfoil-active-downloads") {
    heardActiveDownloads(url, int(std::strtol(trimmed().c_str(), nullptr, 10)));
    return;
  }
  if (key == "x-cookingfoil-session") { // the session token refused: expired | revoked | invalid
    const auto value = trimmed();
    const auto owner = serverOf(url);
    changeLink(url, [](Link &link) { ++link.sessionRefusals; });
    if (value == "revoked" && !owner.empty()) {
      std::lock_guard<std::mutex> held(linkLock);
      revoked.insert(owner);
    }
    logLine("session token " + (value == "expired" || value == "revoked" || value == "invalid" ? value : std::string("refused")));
    return;
  }
  if (key == "x-cookingfoil-token") { // a download refused: expired or revoked
    changeLink(url, [](Link &link) { link.rejected = true; });
    return;
  }
  if (key != "x-cookingfoil-restarting") return;
  replyOk = false;
  const auto value = line.substr(colon + 1);
  const long seconds = std::clamp<long>(std::strtol(value.c_str(), nullptr, 10), 1, 120);
  std::string id;
  if (const auto at = value.find("id="); at != std::string::npos) {
    id = value.substr(at + 3);
    id = id.substr(0, id.find_first_of(" ;,\t"));
  }
  changeLink(url, [&](Link &link) {
    // Down for at most the server's estimate and a minute more: a restart
    // that never ends does not keep a download waiting for good.
    link.restartUntil = std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 60);
    if (id != link.restartId) ++restarts;
    link.restartId = id;
    link.restarting = true;
  });
}
bool serverRestarting(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.restarting && std::chrono::steady_clock::now() < link.restartUntil; });
}
bool serverRestarting() {
  std::lock_guard<std::mutex> held(linkLock);
  const auto now = std::chrono::steady_clock::now();
  return std::any_of(links.begin(), links.end(), [&](const auto &l) { return l.second.restarting && now < l.second.restartUntil; });
}
uint64_t restartSerial() { return restarts; }
std::string downloadToken(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.token; });
}
bool downloadTokenRejected(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.rejected; });
}
std::string downloadTokenOrigin(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.direct; });
}
void forgetDownloadToken(const std::string &url) {
  changeLink(url, [](Link &link) {
    link.token.clear();
    link.direct.clear();
    link.rejected = false;
  });
}
void forgetLinks() {
  std::lock_guard<std::mutex> held(linkLock);
  links.clear();
  lent.clear();
  revoked.clear();
}
void lendToken(const std::string &serverUrl, const std::string &url) {
  const auto origin = originOf(url);
  std::lock_guard<std::mutex> held(linkLock);
  const auto server = ownerOf(serverUrl);
  if (!origin.empty() && !server.empty() && origin != server && !isConfigured(origin)) lent[origin] = server;
}
void setTokenRenewer(const std::string &serverUrl, std::function<long()> renew) {
  changeLink(serverUrl, [&](Link &link) {
    link.renewer = std::move(renew);
    link.askedOnce = false;
  });
}
bool renewDownloadToken(const std::string &url, const std::string &sent) {
  if (isSessionToken(sent)) return renewSession(sent);
  const auto renewing = changeLink(url, [](Link &link) { return link.renewing; });
  std::lock_guard<std::mutex> held(*renewing);
  // Another part already got a newer one.
  if (const auto now = downloadToken(url); !now.empty() && now != sent && !downloadTokenRejected(url)) return true;
  const auto at = std::chrono::steady_clock::now();
  const auto renew = changeLink(url, [&](Link &link) -> std::function<long()> {
    if (!link.renewer || (link.askedOnce && at - link.lastAsk < std::chrono::seconds(RenewGapSeconds))) return {};
    link.askedOnce = true;
    link.lastAsk = at;
    return link.renewer;
  });
  if (!renew) return false;
  const long status = renew();
  const auto now = downloadToken(url);
  const bool fresh = !now.empty() && now != sent && !downloadTokenRejected(url);
  if (!fresh && (status == 401 || status == 403)) changeLink(url, [](Link &link) { link.signInRefused = true; });
  if (!fresh) logLine("download token not renewed: status " + std::to_string(status));
  return fresh;
}
bool downloadSignInRefused(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.signInRefused; });
}
void recommendConnections(const std::string &url, int most) {
  changeLink(url, [&](Link &link) { link.recommended = most > 0 ? std::min(most, 8) : 0; });
}
int recommendedConnections(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.recommended; });
}
void heardActiveDownloads(const std::string &url, int count) {
  changeLink(url, [&](Link &link) { link.activeDownloads = std::clamp(count, 0, 64); });
}
int activeDownloads(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.activeDownloads; });
}
static const std::string BearerLead = "Authorization: Bearer ";
std::string bearerOf(const std::vector<std::string> &headers) {
  for (const auto &h : headers)
    if (h.rfind(BearerLead, 0) == 0) return h.substr(BearerLead.size());
  return {};
}
namespace {
std::string sessionAfter(const std::string &sent);
}
void freshenBearer(const std::string &url, std::vector<std::string> &headers) {
  const std::string &Lead = BearerLead;
  for (auto &h : headers) {
    if (h.rfind(Lead, 0) != 0) continue;
    const auto sent = h.substr(Lead.size());
    const auto now = isSessionToken(sent) ? sessionAfter(sent) : downloadToken(url);
    if (!now.empty()) h = Lead + now;
  }
}
void watchRestart(void *curl) {
  curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_HEADERFUNCTION, onRestartHeader);
  curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_HEADERDATA, curl);
}
void setReceiveWindow(int bytes) { window = bytes; }
void countReceived(size_t bytes) { received.fetch_add(bytes, std::memory_order_relaxed); }
uint64_t receivedBytes() { return received.load(std::memory_order_relaxed); }
bool linkDown(const std::string &url) {
  if (serverRestarting(url))
    return true;
#ifdef __SWITCH__
  NifmInternetConnectionType type{};
  NifmInternetConnectionStatus status{};
  u32 strength = 0;
  return R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status)) &&
         status != NifmInternetConnectionStatus_Connected;
#else
  return false;
#endif
}
int receiveWindow() { return window; }
namespace {
// A cache of open connections (with their TLS sessions and DNS answers)
// for each thread: its next request asks on a connection already open
// instead of a new TCP and TLS handshake through the edge. A folder app of
// thousands of small files was mostly handshakes; a game's next NCA, and
// the next download in the queue, go on the connections the last one used.
// One cache for every thread is what came before, and curl does not
// support connections shared by threads at once: two of them matching
// the same pool (the picture workers beside a download) read a connection
// the other had just closed, and the app crashed (Data Abort in
// url_match_destination).
std::atomic<bool> sharing{true};
struct ThreadShare {
  CURLSH *share = nullptr;
  ~ThreadShare() {
    // At the app's end curl itself is gone first: the main thread's is left.
    if (share && sharing) curl_share_cleanup(share);
  }
};
CURLSH *sharedConnections() {
  thread_local ThreadShare mine;
  if (!mine.share && sharing && (mine.share = curl_share_init())) {
    curl_share_setopt(mine.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(mine.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(mine.share, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
  }
  return mine.share;
}
} // namespace
void endConnections() { sharing = false; }
void shareConnections(void *curl) {
  if (auto *share = sharedConnections()) {
    curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_SHARE, share);
    // Room for a download's connections and the screens' requests at once;
    // curl's default of 5 would close warm download connections.
    curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_MAXCONNECTS, 16L);
  }
}
namespace {
thread_local const std::atomic<bool> *threadCancel = nullptr;
bool cancelled() { return threadCancel && threadCancel->load(); }
} // namespace
CancelScope::CancelScope(const std::atomic<bool> *cancel) : outer_(threadCancel) {
  if (cancel) threadCancel = cancel;
}
CancelScope::~CancelScope() { threadCancel = outer_; }
const std::atomic<bool> *scopedCancel() { return threadCancel; }
namespace {
struct SessionEntry {
  std::string credentials;
  std::function<SessionGrant()> ask;
  std::string token;
  std::set<std::string> spent; // tokens it held before, to tell a reply's
  std::chrono::steady_clock::time_point until;
  uint64_t refusalsSeen = 0; // its server's session refusals when it was asked
  bool refused = false; // the server gave none to this sign-in
  // Not asked again before this: its last answer was neither a token nor a
  // refusal (a 503, no answer at all).
  std::chrono::steady_clock::time_point retryAt;
  // Held while its server is asked: the other requests for it wait, those
  // for other servers do not.
  std::shared_ptr<std::mutex> asking = std::make_shared<std::mutex>();
};
std::mutex sessionLock; // the list; never held while a server is asked
std::map<std::string, SessionEntry> sessions; // by origin
std::chrono::milliseconds sessionRetryGap{std::chrono::seconds(RenewGapSeconds)};
// No token to be had now: refused for this sign-in, or waiting to ask again.
bool withheld(const SessionEntry &entry) { return entry.refused || std::chrono::steady_clock::now() < entry.retryAt; }
// Asks `origin`'s server for a token in place of the one held (`held` locks
// sessionLock, and is let go while the server answers). Nothing when another
// request got a new one, or a refusal, meanwhile.
void askSession(const std::string &origin, std::unique_lock<std::mutex> &held) {
  auto found = sessions.find(origin);
  if (found == sessions.end()) return;
  const auto gate = found->second.asking;
  const auto seen = found->second.token;
  held.unlock();
  // Another request asks: waited for, unless this one's work is stopped.
  while (!gate->try_lock()) {
    if (cancelled()) {
      held.lock();
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  std::lock_guard<std::mutex> one(*gate, std::adopt_lock);
  held.lock();
  found = sessions.find(origin);
  if (found == sessions.end() || found->second.asking != gate || withheld(found->second) || found->second.token != seen)
    return;
  auto &entry = found->second;
  entry.refusalsSeen = sessionRefusals(origin);
  if (!entry.token.empty()) entry.spent.insert(entry.token);
  entry.token.clear();
  const auto ask = entry.ask;
  const auto credentials = entry.credentials;
  held.unlock();
  const auto grant = ask ? ask() : SessionGrant{};
  held.lock();
  // Stopped while asking: no answer, not a refusal; the next one asks again.
  if (cancelled() && grant.token.empty()) return;
  found = sessions.find(origin);
  // Taken back, or another sign-in offered meanwhile: this answer is for
  // neither.
  if (found == sessions.end() || found->second.asking != gate || found->second.credentials != credentials) return;
  auto &now = found->second;
  if (grant.status == 200 && !grant.token.empty()) {
    now.token = grant.token;
    now.until = std::chrono::steady_clock::now() + std::chrono::seconds(grant.secondsLeft);
    return;
  }
  // 403: this server keeps the sign-in; 401: the sign-in itself is wrong
  // (the console taken back), and the request it was for says so. 429 is
  // the lock, not asked again. Anything else (a 5xx, no answer) is the
  // server for now: asked again after the gap.
  if (grant.status != 401 && grant.status != 403 && grant.status != 429) {
    now.retryAt = std::chrono::steady_clock::now() + sessionRetryGap;
    logLine("session token for " + origin + " not given: status " + std::to_string(grant.status) + "; asking again later");
    return;
  }
  if (const auto owner = serverOf(origin); grant.status == 401 && !owner.empty()) {
    std::lock_guard<std::mutex> held(linkLock);
    revoked.insert(owner);
  }
  now.refused = true;
  logLine("session token for " + origin + " not given: status " + std::to_string(grant.status) + "; signing in as before");
}
SessionEntry *owner(const std::string &sent) {
  for (auto &[origin, entry] : sessions)
    if (entry.token == sent || entry.spent.count(sent)) return &entry;
  return nullptr;
}
std::string sessionAfter(const std::string &sent) {
  std::lock_guard<std::mutex> held(sessionLock);
  const auto *entry = owner(sent);
  return entry ? entry->token : std::string();
}
} // namespace
void offerSession(const std::string &origin, const std::string &credentials, std::function<SessionGrant()> ask) {
  std::lock_guard<std::mutex> held(sessionLock);
  auto &entry = sessions[origin];
  if (entry.credentials != credentials) {
    if (!entry.token.empty()) entry.spent.insert(entry.token);
    entry.token.clear();
    entry.refused = false;
    entry.retryAt = {};
    entry.credentials = credentials;
  }
  entry.ask = std::move(ask);
}
void withdrawSession(const std::string &origin) {
  std::lock_guard<std::mutex> held(sessionLock);
  sessions.erase(origin);
}
std::string sessionToken(const std::string &origin) {
  std::unique_lock<std::mutex> held(sessionLock);
  auto found = sessions.find(origin);
  if (found == sessions.end() || withheld(found->second)) return {};
  const auto &entry = found->second;
  const bool ending = std::chrono::steady_clock::now() + std::chrono::seconds(RenewBeforeSeconds) >= entry.until;
  if (!entry.token.empty() && !ending && entry.refusalsSeen == sessionRefusals(origin)) return entry.token;
  askSession(origin, held);
  found = sessions.find(origin);
  return found == sessions.end() || withheld(found->second) ? std::string() : found->second.token;
}
bool renewSession(const std::string &sent) {
  std::unique_lock<std::mutex> held(sessionLock);
  std::string origin;
  for (const auto &[at, entry] : sessions)
    if (entry.token == sent || entry.spent.count(sent)) origin = at;
  auto found = sessions.find(origin);
  if (found == sessions.end() || withheld(found->second)) return false;
  // Another request already got a newer one.
  if (!found->second.token.empty() && found->second.token != sent) return true;
  askSession(origin, held);
  found = sessions.find(origin);
  return found != sessions.end() && !found->second.token.empty() && found->second.token != sent;
}
bool isSessionToken(const std::string &token) {
  if (token.empty()) return false;
  std::lock_guard<std::mutex> held(sessionLock);
  return owner(token) != nullptr;
}
uint64_t sessionRefusals(const std::string &url) {
  return peekLink(url, [](const Link &link) { return link.sessionRefusals; });
}
bool takeConsoleRevoked(const std::string &serverUrl) {
  std::lock_guard<std::mutex> held(linkLock);
  return revoked.erase(ownerOf(serverUrl)) > 0;
}
void forgetSessions() {
  std::lock_guard<std::mutex> held(sessionLock);
  sessions.clear();
}
void setSessionRetryGap(std::chrono::milliseconds gap) {
  std::lock_guard<std::mutex> held(sessionLock);
  sessionRetryGap = gap;
}
} // namespace foil::net
