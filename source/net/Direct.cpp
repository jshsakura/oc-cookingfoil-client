#include "net/Direct.hpp"
#include "core/Config.hpp"
#include "net/Socket.hpp"
#include <curl/curl.h>
#include <mutex>
namespace foil::net {
namespace {
std::mutex lock;
std::string shop;      // the server's own origin
std::string renewedTo; // the token a 401 on the direct address was renewed to
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
    if (shop.empty() || shop == direct) return url;
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
DirectStep afterDirectFailure(const std::string &requestUrl, long status, const std::string &sent) {
  if (status != 401 || !isDirect(requestUrl)) return DirectStep::Other;
  const auto now = downloadToken();
  {
    std::lock_guard<std::mutex> held(lock);
    // Another request already holds a newer token: again with it.
    if (!now.empty() && now != sent && sent != renewedTo) return DirectStep::Again;
    // Refused with the fresh one too: the caller's own handling.
    if (!sent.empty() && sent == renewedTo) return DirectStep::Other;
  }
  if (!renewDownloadToken(sent)) return DirectStep::Other;
  std::lock_guard<std::mutex> held(lock);
  renewedTo = downloadToken();
  return DirectStep::Again;
}
std::string routeName(const std::string &url) { return isDirect(routeDownload(url)) ? "direct" : "server"; }
void forgetDirect() {
  std::lock_guard<std::mutex> held(lock);
  shop.clear();
  renewedTo.clear();
}
} // namespace foil::net
