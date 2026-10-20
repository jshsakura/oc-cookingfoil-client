#include "net/Direct.hpp"
#include "core/Config.hpp"
#include "net/Socket.hpp"
#include <curl/curl.h>
#include <map>
#include <mutex>
namespace foil::net {
namespace {
std::mutex lock;
std::map<std::string, std::string> renewedTo; // by server: the token a 401 on its direct address was renewed to
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
} // namespace
bool isDirect(const std::string &url) {
  const auto to = downloadTokenOrigin(url);
  return !to.empty() && originOf(url) == to;
}
// Named, the direct address is the only way, token or not yet: without one
// the request goes with no key, and its 401 asks for one (afterDirectFailure).
std::string routeDownload(const std::string &url) {
  // Only a server's own address goes direct: a mirror it lists has no
  // direct address of its own.
  const auto to = downloadTokenOrigin(url);
  if (to.empty() || originOf(url) == to) return url;
  const auto path = pathOf(url);
  return path.empty() ? url : to + path;
}
std::vector<std::string> headersFor(const std::string &requestUrl, const std::vector<std::string> &headers) {
  if (!isDirect(requestUrl)) return headers;
  const auto token = downloadToken(requestUrl);
  if (token.empty()) return {};
  return {"Authorization: Bearer " + token};
}
DirectStep afterDirectFailure(const std::string &requestUrl, long status, const std::string &sent) {
  // Sent with no key (no token had come yet), the proxy in front of the
  // direct address ends it with a 404 before the app sees it: as a 401.
  const bool refused = status == 401 || (status == 404 && sent.empty());
  if (!refused || !isDirect(requestUrl)) return DirectStep::Other;
  const auto now = downloadToken(requestUrl);
  const auto server = serverOf(requestUrl);
  {
    std::lock_guard<std::mutex> held(lock);
    const auto &renewed = renewedTo[server];
    // Another request already holds a newer token: again with it.
    if (!now.empty() && now != sent && sent != renewed) return DirectStep::Again;
    // Refused with the fresh one too: the caller's own handling.
    if (!sent.empty() && sent == renewed) return DirectStep::Other;
  }
  if (!renewDownloadToken(requestUrl, sent)) return DirectStep::Other;
  std::lock_guard<std::mutex> held(lock);
  renewedTo[server] = downloadToken(requestUrl);
  return DirectStep::Again;
}
std::string routeName(const std::string &url) { return isDirect(routeDownload(url)) ? "direct" : "server"; }
void forgetDirect() {
  std::lock_guard<std::mutex> held(lock);
  renewedTo.clear();
}
} // namespace foil::net
