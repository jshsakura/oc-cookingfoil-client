#include "net/ProdKeysApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
namespace foil::net {
namespace {
Error refusal(const Response &response) {
  if (!response.error.empty()) return {response.error};
  if (response.status == 403) return {tr("This console is not an admin console. Turn it on in the admin dashboard.")};
  if (response.status == 410) return {tr("The server's one-time key ran out. Send it again.")};
  if (response.status == 400) {
    Error ignored;
    const auto root = parseJson(response.body, ignored);
    const auto reason = root ? stringField(root.get(), "reason") : std::string();
    if (reason == "no-header-key") return {tr("This prod.keys has no header_key.")};
    if (reason == "too-large") return {tr("This prod.keys is too large.")};
    if (reason == "format") return {tr("This is not a prod.keys file.")};
    return {tr("The server could not open what was sent. Send it again.")};
  }
  return {trf("Could not reach the server. HTTP {0}", {std::to_string(response.status)})};
}
Parsed<prodkeys::Status> status(const Response &response) {
  if (response.status != 200 || !response.error.empty()) return {{}, refusal(response)};
  return {prodkeys::parseStatus(response.body), {}};
}
} // namespace
Parsed<prodkeys::Status> keysStatus(const Server &server, const State &state, const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/admin/keys";
  return status(get(url, authHeaders(server, state, url), "", cancel));
}
Parsed<prodkeys::Status> sendKeys(const Server &server, const State &state, const std::string &keysText,
                                  const std::atomic<bool> *cancel) {
  const auto sealUrl = server.url + "/api/admin/keys/seal";
  auto headers = authHeaders(server, state, sealUrl);
  headers.push_back("Content-Type: application/json");
  PostOptions ask;
  ask.timeout = 20;
  ask.body = "{}";
  const auto offered = post(sealUrl, headers, ask, cancel);
  if (offered.status != 200 || !offered.error.empty()) return {{}, refusal(offered)};
  const auto body = prodkeys::sealBody(prodkeys::parseOffer(offered.body), keysText);
  if (body.error) return {{}, body.error};
  const auto url = server.url + "/api/admin/keys";
  auto putHeaders = authHeaders(server, state, url);
  putHeaders.push_back("Content-Type: application/json");
  PostOptions put;
  put.timeout = 30;
  put.method = "PUT";
  put.body = body.value;
  return status(post(url, putHeaders, put, cancel));
}
} // namespace foil::net
