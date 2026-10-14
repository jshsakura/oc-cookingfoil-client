#include "net/Socket.hpp"
#include "core/Log.hpp"
#include <atomic>
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
} // namespace
void setReceiveWindow(int bytes) { window = bytes; }
void countReceived(size_t bytes) { received.fetch_add(bytes, std::memory_order_relaxed); }
uint64_t receivedBytes() { return received.load(std::memory_order_relaxed); }
bool linkDown() {
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
