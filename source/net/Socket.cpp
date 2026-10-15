#include "net/Socket.hpp"
#include "core/Log.hpp"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <curl/curl.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <sys/socket.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::net {
namespace {
std::atomic<int> window{0};
std::atomic<bool> logged{false};
std::atomic<uint64_t> received{0};
// The restart heard last: its id, until when it may last, whether it is on.
std::mutex restartLock;
std::string restartId;
std::chrono::steady_clock::time_point restartUntil;
bool restarting = false;
std::atomic<uint64_t> restarts{0};
std::mutex tokenLock;
std::string token;
std::atomic<bool> tokenRejected{false};
std::mutex renewLock; // one ask at a time; the others wait for its token
std::function<long()> renewer;
std::chrono::steady_clock::time_point lastAsk;
bool askedOnce = false;
std::atomic<bool> signInRefused{false};
std::atomic<uint64_t> sessionRefused{0};
// A reply's header block, in the thread curl reads it on: whether it is a
// success so far and has no restart header.
thread_local bool replyOk = false;
size_t onRestartHeader(char *data, size_t size, size_t count, void *) {
  heardHeader(data, size * count);
  return size * count;
}
} // namespace
void heardHeader(const char *data, size_t bytes) {
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
    if (replyOk) {
      std::lock_guard<std::mutex> held(restartLock);
      restarting = false;
    }
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
    if (!value.empty() && value.find_first_of(" \t\r\n") == std::string::npos) {
      std::lock_guard<std::mutex> held(tokenLock);
      token = value;
      tokenRejected = false;
      signInRefused = false;
    }
    return;
  }
  if (key == "x-cookingfoil-session") { // the session token refused: expired | revoked | invalid
    sessionRefused.fetch_add(1);
    const auto value = trimmed();
    logLine("session token " + (value == "expired" || value == "revoked" || value == "invalid" ? value : std::string("refused")));
    return;
  }
  if (key == "x-cookingfoil-token") { // a download refused: expired or revoked
    tokenRejected = true;
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
  std::lock_guard<std::mutex> held(restartLock);
  // Down for at most the server's estimate and a minute more: a restart
  // that never ends does not keep a download waiting for good.
  restartUntil = std::chrono::steady_clock::now() + std::chrono::seconds(seconds + 60);
  if (!restarting || id != restartId) {
    if (id != restartId) {
      ++restarts;
      logLine("server restarting (" + std::to_string(seconds) + " s, " + id + ")");
    }
    restartId = id;
  }
  restarting = true;
}
bool serverRestarting() {
  std::lock_guard<std::mutex> held(restartLock);
  return restarting && std::chrono::steady_clock::now() < restartUntil;
}
uint64_t restartSerial() { return restarts; }
std::string downloadToken() {
  std::lock_guard<std::mutex> held(tokenLock);
  return token;
}
bool downloadTokenRejected() { return tokenRejected; }
void setTokenRenewer(std::function<long()> renew) {
  std::lock_guard<std::mutex> held(renewLock);
  renewer = std::move(renew);
  askedOnce = false;
}
bool renewDownloadToken(const std::string &sent) {
  if (isSessionToken(sent)) return renewSession(sent);
  std::lock_guard<std::mutex> held(renewLock);
  // Another part already got a newer one.
  if (const auto now = downloadToken(); !now.empty() && now != sent && !tokenRejected) return true;
  const auto at = std::chrono::steady_clock::now();
  if (!renewer || (askedOnce && at - lastAsk < std::chrono::seconds(RenewGapSeconds))) return false;
  askedOnce = true;
  lastAsk = at;
  const long status = renewer();
  const auto now = downloadToken();
  const bool fresh = !now.empty() && now != sent && !tokenRejected;
  if (!fresh && (status == 401 || status == 403)) signInRefused = true;
  logLine(fresh ? "download token renewed" : "download token not renewed: status " + std::to_string(status));
  return fresh;
}
bool downloadSignInRefused() { return signInRefused; }
static const std::string BearerLead = "Authorization: Bearer ";
std::string bearerOf(const std::vector<std::string> &headers) {
  for (const auto &h : headers)
    if (h.rfind(BearerLead, 0) == 0) return h.substr(BearerLead.size());
  return {};
}
namespace {
std::string sessionAfter(const std::string &sent);
}
void freshenBearer(std::vector<std::string> &headers) {
  const std::string &Lead = BearerLead;
  for (auto &h : headers) {
    if (h.rfind(Lead, 0) != 0) continue;
    const auto sent = h.substr(Lead.size());
    const auto now = isSessionToken(sent) ? sessionAfter(sent) : downloadToken();
    if (!now.empty()) h = Lead + now;
  }
}
void watchRestart(void *curl) {
  curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_HEADERFUNCTION, onRestartHeader);
}
void setReceiveWindow(int bytes) { window = bytes; }
void countReceived(size_t bytes) { received.fetch_add(bytes, std::memory_order_relaxed); }
uint64_t receivedBytes() { return received.load(std::memory_order_relaxed); }
bool linkDown() {
  if (serverRestarting())
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
int tuneSocket(void *, int socket, int) {
  if (!logged.exchange(true)) {
    int start = 0;
    socklen_t size = sizeof(start);
    getsockopt(socket, SOL_SOCKET, SO_RCVBUF, &start, &size);
    logLine("socket receive window starts at " + std::to_string(start) + ", grows to " + std::to_string(window.load()));
  }
  return 0; // CURL_SOCKOPT_OK
}
namespace {
// One cache of open connections (with their TLS sessions and DNS answers)
// for every request: the next file asks on a connection already open
// instead of a new TCP and TLS handshake through the edge. A folder app of
// thousands of small files was mostly handshakes; a game's next NCA, and
// the next download in the queue, go on the connections the last one used.
std::mutex shareLocks[CURL_LOCK_DATA_LAST];
CURLSH *sharedConnections() {
  static CURLSH *share = [] {
    CURLSH *s = curl_share_init();
    if (!s)
      return s;
    curl_share_setopt(s, CURLSHOPT_LOCKFUNC,
                      +[](CURL *, curl_lock_data data, curl_lock_access, void *) { shareLocks[data].lock(); });
    curl_share_setopt(s, CURLSHOPT_UNLOCKFUNC, +[](CURL *, curl_lock_data data, void *) { shareLocks[data].unlock(); });
    curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
    curl_share_setopt(s, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    return s;
  }();
  return share;
}
} // namespace
void shareConnections(void *curl) {
  if (auto *share = sharedConnections()) {
    curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_SHARE, share);
    // Room for a download's connections and the screens' requests at once;
    // curl's default of 5 would close warm download connections.
    curl_easy_setopt(static_cast<CURL *>(curl), CURLOPT_MAXCONNECTS, 16L);
  }
}
namespace {
struct SessionEntry {
  std::string credentials;
  std::function<SessionGrant()> ask;
  std::string token;
  std::set<std::string> spent; // tokens it held before, to tell a reply's
  std::chrono::steady_clock::time_point until;
  uint64_t refusalsSeen = 0;
  bool refused = false; // the server gave none to this sign-in
};
std::mutex sessionLock; // held while one is asked for; the others wait for it
std::map<std::string, SessionEntry> sessions; // by origin
// Under sessionLock.
void askSession(const std::string &origin, SessionEntry &entry) {
  entry.refusalsSeen = sessionRefused.load();
  if (!entry.token.empty()) entry.spent.insert(entry.token);
  entry.token.clear();
  const auto grant = entry.ask ? entry.ask() : SessionGrant{};
  if (grant.status == 200 && !grant.token.empty()) {
    entry.token = grant.token;
    entry.until = std::chrono::steady_clock::now() + std::chrono::seconds(grant.secondsLeft);
    logLine("session token for " + origin + ": " + std::to_string(grant.secondsLeft / 60) + " min, renewed " +
            std::to_string(RenewBeforeSeconds / 60) + " min before it ends");
    return;
  }
  // 403: this server keeps the sign-in; 401: the sign-in itself is wrong,
  // and the request it was for says so. 429 is the lock, not asked again.
  entry.refused = true;
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
    entry.credentials = credentials;
  }
  entry.ask = std::move(ask);
}
void withdrawSession(const std::string &origin) {
  std::lock_guard<std::mutex> held(sessionLock);
  sessions.erase(origin);
}
std::string sessionToken(const std::string &origin) {
  std::lock_guard<std::mutex> held(sessionLock);
  const auto found = sessions.find(origin);
  if (found == sessions.end() || found->second.refused) return {};
  auto &entry = found->second;
  const bool ending = std::chrono::steady_clock::now() + std::chrono::seconds(RenewBeforeSeconds) >= entry.until;
  if (entry.token.empty() || ending || entry.refusalsSeen != sessionRefused.load()) askSession(origin, entry);
  return entry.token;
}
bool renewSession(const std::string &sent) {
  std::lock_guard<std::mutex> held(sessionLock);
  auto *entry = owner(sent);
  if (!entry || entry->refused) return false;
  // Another request already got a newer one.
  if (!entry->token.empty() && entry->token != sent) return true;
  for (auto &[origin, e] : sessions)
    if (&e == entry) askSession(origin, e);
  return !entry->token.empty() && entry->token != sent;
}
bool isSessionToken(const std::string &token) {
  if (token.empty()) return false;
  std::lock_guard<std::mutex> held(sessionLock);
  return owner(token) != nullptr;
}
uint64_t sessionRefusals() { return sessionRefused.load(); }
void forgetSessions() {
  std::lock_guard<std::mutex> held(sessionLock);
  sessions.clear();
}
} // namespace foil::net
