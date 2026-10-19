#include "net/AchievementsApi.hpp"
#include "core/I18n.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
namespace foil::net {
namespace {
constexpr const char *AccountPath = "/api/retro/achievements/account";
Error accountError(const Response &response) {
  if (!response.error.empty()) return {response.error};
  if (response.status == 404) return {tr("RetroAchievements has no account with that id.")};
  if (response.status == 503) return {tr("This server has no RetroAchievements key set up.")};
  if (response.status == 400) return {tr("Check the RetroAchievements id.")};
  return {trf("Could not reach the server's achievements. HTTP {0}", {std::to_string(response.status)})};
}
} // namespace
Parsed<achievements::Account> achievementsAccount(const Server &server, const State &state) {
  const auto url = server.url + AccountPath;
  const auto response = get(url, authHeaders(server, state, url));
  // An older server without achievements: nothing to show, no error.
  if (response.status == 404) return {{}, {}};
  if (!response.error.empty() || response.status != 200) return {{}, accountError(response)};
  return {achievements::parseAccount(response.body), {}};
}
Parsed<achievements::Account> setAchievementsAccount(const Server &server, const State &state,
                                                     const std::string &username) {
  const auto url = server.url + AccountPath;
  auto headers = authHeaders(server, state, url);
  PostOptions options;
  options.timeout = 20;
  if (username.empty()) {
    options.method = "DELETE";
  } else {
    options.method = "PUT";
    options.body = achievements::accountBody(username);
    headers.push_back("Content-Type: application/json");
  }
  const auto response = post(url, headers, options);
  if (!response.error.empty() || response.status < 200 || response.status >= 300) return {{}, accountError(response)};
  // DELETE may answer with no body: the id is gone, the key as before.
  auto account = achievements::parseAccount(response.body);
  if (username.empty()) account.username.clear();
  return {account, {}};
}
Parsed<achievements::Game> gameAchievements(const Server &server, const State &state, const std::string &gameId,
                                            bool &missing, const std::atomic<bool> *cancel) {
  missing = false;
  const auto url = server.url + "/api/retro/games/" + dropbox::percentEncode(gameId) + "/achievements";
  const auto response = get(url, authHeaders(server, state, url), "", cancel);
  if (response.status == 404) {
    missing = true;
    return {{}, {tr("RetroAchievements has no achievements for this game.")}};
  }
  if (response.status == 502) return {{}, {tr("RetroAchievements did not answer. Try again shortly.")}};
  if (!response.error.empty() || response.status != 200) return {{}, accountError(response)};
  return {achievements::parseGame(response.body, server.url), {}};
}
Parsed<achievements::Login> signInRetroAchievements(const std::string &user, const std::string &password) {
  PostOptions options;
  options.timeout = 20;
  options.body = "r=login2&u=" + dropbox::percentEncode(user) + "&p=" + dropbox::percentEncode(password);
  const auto response = post("https://retroachievements.org/dorequest.php",
                             {"Content-Type: application/x-www-form-urlencoded"}, options);
  if (!response.error.empty()) return {{}, {response.error}};
  auto login = achievements::parseLogin(response.body);
  if (login.token.empty()) {
    // RetroAchievements answers a wrong password with 401 and its reason.
    if (login.error.empty()) login.error = trf("RetroAchievements did not sign in. HTTP {0}", {std::to_string(response.status)});
    return {{}, {login.error}};
  }
  return {login, {}};
}
} // namespace foil::net
