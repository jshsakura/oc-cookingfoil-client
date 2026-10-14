#include "net/Socket.hpp"
#include "core/Log.hpp"
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <curl/curl.h>
#include <mutex>
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
std::function<void()> renewer;
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
    }
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
void setTokenRenewer(std::function<void()> renew) {
  std::lock_guard<std::mutex> held(renewLock);
  renewer = std::move(renew);
}
bool renewDownloadToken() {
  {
    std::lock_guard<std::mutex> held(renewLock);
    if (tokenRejected && renewer) {
      logLine("download token renewed");
      renewer();
    }
  }
  return !tokenRejected && !downloadToken().empty();
}
void freshenBearer(std::vector<std::string> &headers) {
  static const std::string Lead = "Authorization: Bearer ";
  const auto now = downloadToken();
  for (auto &h : headers)
    if (h.rfind(Lead, 0) == 0 && !now.empty()) h = Lead + now;
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
} // namespace foil::net
