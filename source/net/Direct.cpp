#include "net/Direct.hpp"
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "net/Socket.hpp"
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <mutex>
namespace foil::net {
namespace {
using Clock = std::chrono::steady_clock;
std::mutex lock;
std::string shop;               // the server's own origin
std::string renewedTo;          // the token a 401 on the direct address was renewed to
Clock::time_point restUntil;    // the direct address rests until then
bool resting = false;
std::atomic<unsigned> leaves{0};
// The path and query of `url` ("/a/b?x=1"), as sent.
std::string pathOf(const std::string &url) {
  auto *h = curl_url();
  if (!h || curl_url_set(h, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
    if (h) curl_url_cleanup(h);
    return {};
  }
  std::string result;
  char *part = nullptr;
  if (curl_url_get(h, CURLUPART_PATH, &part, 0) == CURLUE_OK && part) result = part;
  curl_free(part);
  part = nullptr;
  if (curl_url_get(h, CURLUPART_QUERY, &part, 0) == CURLUE_OK && part && *part) result += std::string("?") + part;
  curl_free(part);
  curl_url_cleanup(h);
  return result.empty() ? "/" : result;
}
bool restingNow() {
  if (resting && Clock::now() >= restUntil) resting = false;
  return resting;
}
// No connection, a TLS failure or a timeout: the direct address is not
// there for this console.
bool unreachable(long code) {
  switch (code) {
  case CURLE_COULDNT_RESOLVE_HOST:
  case CURLE_COULDNT_CONNECT:
  case CURLE_OPERATION_TIMEDOUT:
  case CURLE_SSL_CONNECT_ERROR:
  case CURLE_PEER_FAILED_VERIFICATION:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CIPHER:
  case CURLE_SSL_CACERT_BADFILE:
  case CURLE_SSL_ISSUER_ERROR:
  case CURLE_GOT_NOTHING:
    return true;
  default:
    return false;
  }
}
} // namespace
void setShopOrigin(const std::string &origin) {
  std::lock_guard<std::mutex> held(lock);
  shop = origin.empty() ? std::string() : originOf(origin);
}
bool isDirect(const std::string &url) {
  const auto direct = downloadTokenOrigin();
  return !direct.empty() && originOf(url) == direct;
}
std::string routeDownload(const std::string &url) {
  const auto direct = downloadTokenOrigin();
  if (direct.empty() || downloadToken().empty()) return url;
  {
    std::lock_guard<std::mutex> held(lock);
    if (shop.empty() || shop == direct || restingNow()) return url;
    if (originOf(url) != shop) return url;
  }
  const auto path = pathOf(url);
  return path.empty() ? url : direct + path;
}
std::vector<std::string> headersFor(const std::string &requestUrl, const std::vector<std::string> &headers) {
  if (!isDirect(requestUrl)) return headers;
  const auto token = downloadToken();
  if (token.empty()) return {};
  return {"Authorization: Bearer " + token};
}
DirectStep afterDirectFailure(const std::string &requestUrl, long status, long curlCode, const std::string &sent) {
  if (!isDirect(requestUrl)) return DirectStep::Other;
  std::string why;
  if (status == 401) {
    // Another request already holds a newer token: again with it.
    const auto now = downloadToken();
    bool again = false;
    {
      std::lock_guard<std::mutex> held(lock);
      again = !now.empty() && now != sent && sent != renewedTo;
    }
    if (again) return DirectStep::Again;
    bool renewedBefore = false;
    {
      std::lock_guard<std::mutex> held(lock);
      renewedBefore = !sent.empty() && sent == renewedTo;
    }
    // Once a fresh one: refused again, or none to be had, it is left.
    if (!renewedBefore && renewDownloadToken(sent)) {
      std::lock_guard<std::mutex> held(lock);
      renewedTo = downloadToken();
      return DirectStep::Again;
    }
    why = "401";
  } else if (status == 403 || status >= 500) {
    why = std::to_string(status);
  } else if ((status == 0 || status == 200 || status == 206) && unreachable(curlCode)) {
    why = std::string("curl ") + std::to_string(curlCode);
  } else {
    return DirectStep::Other;
  }
  bool first = false;
  {
    std::lock_guard<std::mutex> held(lock);
    first = !restingNow();
    resting = true;
    restUntil = Clock::now() + std::chrono::minutes(DirectRestMinutes);
  }
  if (first) {
    ++leaves;
    logLine("direct download address failed (" + why + "): the server's own address for " +
            std::to_string(DirectRestMinutes) + " min");
  }
  return DirectStep::Leave;
}
unsigned directLeaves() { return leaves.load(); }
std::string routeName(const std::string &url) { return isDirect(routeDownload(url)) ? "direct" : "server"; }
void forgetDirect() {
  std::lock_guard<std::mutex> held(lock);
  shop.clear();
  renewedTo.clear();
  resting = false;
}
} // namespace foil::net
