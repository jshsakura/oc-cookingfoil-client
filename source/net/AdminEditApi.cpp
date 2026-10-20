#include "net/AdminEditApi.hpp"
#include "core/I18n.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
namespace foil::net {
namespace {
std::string fieldsUrl(const Server &server, const std::string &kind, const std::string &id) {
  return server.url + "/api/admin/items/" + dropbox::percentEncode(kind) + "/" + dropbox::percentEncode(id) + "/fields";
}
Parsed<std::vector<admin::Field>> answer(const Response &response) {
  if (!response.error.empty()) return {{}, {response.error}};
  if (response.status == 403) return {{}, {tr("This console is not an admin console. Turn it on in the admin dashboard.")}};
  if (response.status == 404) return {{}, {tr("This server cannot edit from the console yet.")}};
  if (response.status == 400) return {{}, {tr("The server did not take that change.")}};
  if (response.status != 200) return {{}, {trf("Could not reach the server. HTTP {0}", {std::to_string(response.status)})}};
  return {admin::parseFields(response.body), {}};
}
} // namespace
Parsed<std::vector<admin::Field>> adminFields(const Server &server, const State &state, const std::string &kind,
                                              const std::string &id, const std::atomic<bool> *cancel) {
  const auto url = fieldsUrl(server, kind, id);
  return answer(get(url, authHeaders(server, state, url), "", cancel));
}
Parsed<std::vector<admin::Field>> setAdminField(const Server &server, const State &state, const std::string &kind,
                                                const std::string &id, const std::string &body,
                                                const std::atomic<bool> *cancel) {
  const auto url = fieldsUrl(server, kind, id);
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: application/json");
  PostOptions options;
  options.timeout = 20;
  options.method = "PATCH";
  options.body = body;
  return answer(post(url, headers, options, cancel));
}
} // namespace foil::net
