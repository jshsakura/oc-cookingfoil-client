#include "net/VotesApi.hpp"
#include "core/I18n.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
namespace foil::net {
Parsed<votes::Votes> castVote(const Server &server, const State &state, const std::string &kind, const std::string &id,
                              const std::string &body, const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/votes/" + dropbox::percentEncode(kind) + "/" + dropbox::percentEncode(id);
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: application/json");
  PostOptions options;
  options.timeout = 15;
  options.body = body;
  const auto response = post(url, headers, options, cancel);
  if (!response.error.empty()) return {{}, {response.error}};
  if (response.status == 404) return {{}, {tr("The server no longer has this.")}};
  if (response.status != 200) return {{}, {trf("Could not reach the server. HTTP {0}", {std::to_string(response.status)})}};
  return {votes::parseAnswer(response.body), {}};
}
} // namespace foil::net
