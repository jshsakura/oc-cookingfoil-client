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
                                                     const std::string &username, const std::atomic<bool> *cancel) {
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
  const auto response = post(url, headers, options, cancel);
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
Parsed<achievements::Login> signInRetroAchievements(const std::string &user, const std::string &password,
                                                    const std::atomic<bool> *cancel) {
  PostOptions options;
  options.timeout = 20;
  options.body = "r=login2&u=" + dropbox::percentEncode(user) + "&p=" + dropbox::percentEncode(password);
  const auto response = post("https://retroachievements.org/dorequest.php",
                             {"Content-Type: application/x-www-form-urlencoded"}, options, cancel);
  std::fill(options.body.begin(), options.body.end(), '\0');
  if (!response.error.empty()) return {{}, {response.error}};
  auto login = achievements::parseLogin(response.body);
  if (!login.token.empty()) return {login, {}};
  // Refused with RetroAchievements' own reason (a wrong password answers
  // 401 with one); anything else, such as 503 or an error page, is RetroAchievements
  // being unreachable, not the password.
  if (!login.error.empty()) return {login, {login.error}};
  return {{}, {trf("RetroAchievements did not sign in. HTTP {0}", {std::to_string(response.status)})}};
}
namespace {
constexpr const char *LinkPath = "/api/retro/achievements/link";
Error linkError(const Response &response) {
  if (!response.error.empty()) return {response.error};
  if (response.status == 410) return {tr("The QR code expired. Open a new one.")};
  return {trf("Could not reach the server's phone sign-in. HTTP {0}", {std::to_string(response.status)})};
}
} // namespace
Parsed<achievements::LinkStart> startAchievementsLink(const Server &server, const State &state,
                                                      const std::string &publicKey, bool &old) {
  old = false;
  const auto url = server.url + LinkPath;
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: application/json");
  PostOptions options;
  options.timeout = 20;
  options.body = "{\"publicKey\":\"" + publicKey + "\"}"; // base64url: nothing to escape
  const auto response = post(url, headers, options);
  if (response.status == 404) {
    old = true;
    return {{}, {tr("This server cannot take the password from a phone yet.")}};
  }
  if (!response.error.empty() || (response.status != 200 && response.status != 201)) return {{}, linkError(response)};
  auto start = achievements::parseLinkStart(response.body);
  if (start.code.empty()) return {{}, {tr("The server's answer could not be read.")}};
  return {start, {}};
}
Parsed<SealedWait> waitSealed(const Server &server, const State &state, const std::string &code,
                              const std::atomic<bool> *cancel) {
  const auto url = server.url + LinkPath + "/" + dropbox::percentEncode(code) + "/sealed";
  const auto response = get(url, authHeaders(server, state, url), "", cancel);
  SealedWait wait;
  if (response.status == 204) wait.waiting = true;
  else if (response.status == 410) wait.gone = true;
  else if (!response.error.empty() || response.status != 200) return {{}, linkError(response)};
  else wait.sealed = achievements::parseSealed(response.body);
  return {wait, {}};
}
Parsed<std::string> sendLinkResult(const Server &server, const State &state, const std::string &code, bool ok,
                                   const std::string &username, const std::string &reason,
                                   const std::atomic<bool> *cancel) {
  const auto url = server.url + LinkPath + "/" + dropbox::percentEncode(code) + "/result";
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: application/json");
  PostOptions options;
  options.timeout = 20;
  options.body = achievements::linkResultBody(ok, username, reason);
  const auto response = post(url, headers, options, cancel);
  if (!response.error.empty() || response.status != 200) return {{}, linkError(response)};
  return {achievements::parseLinkState(response.body), {}};
}
} // namespace foil::net
