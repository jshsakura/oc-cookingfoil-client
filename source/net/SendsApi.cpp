#include "net/SendsApi.hpp"
#include "net/Api.hpp"
namespace foil::net {
SendsReply deviceSends(const Server &server, const State &state, const std::string &etag,
                       const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/device/sends";
  const auto response = get(url, authHeaders(server, state, url), etag, cancel);
  SendsReply reply;
  if (response.status == 304 && response.error.empty()) {
    reply.etag = etag;
    return reply;
  }
  // An older server without the route has nothing waiting.
  if (response.status == 404 && response.error.empty()) {
    reply.feed = sends::Feed{};
    return reply;
  }
  if (!response.error.empty() || response.status != 200) {
    reply.failed = true;
    return reply;
  }
  // Not a feed (an error page answered 200): failed, not an empty list,
  // so nothing it "no longer lists" is taken as answered.
  bool shaped = false;
  auto feed = sends::parse(response.body, &shaped);
  if (!shaped) {
    reply.failed = true;
    return reply;
  }
  reply.feed = std::move(feed);
  reply.etag = response.etag;
  return reply;
}
bool ackSends(const Server &server, const State &state, const std::vector<std::string> &ids, bool accepted,
              const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/device/sends/ack";
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: application/json");
  PostOptions options;
  options.timeout = 15;
  options.body = sends::ackBody(ids, accepted);
  const auto response = post(url, headers, options, cancel);
  return response.error.empty() && response.status == 200;
}
} // namespace foil::net
