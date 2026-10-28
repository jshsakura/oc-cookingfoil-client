#include "Test.hpp"
#include "core/Config.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "net/Socket.hpp"
#include "MockServer.hpp"
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
// A console whose account is turned off or deleted is suspended: the server
// answers 403 with X-CookingFoil-Session: suspended (the key and the session
// token) or X-CookingFoil-Token: suspended (a download), and keeps its key.
// The console keeps its access key and its enroll token, asks for no new
// approval, says why, and comes back with the same key when the account does.
namespace {
bool has(const std::string &request, const std::string &text) { return request.find(text) != std::string::npos; }
int count(const std::vector<std::string> &requests, const std::string &text) {
  int n = 0;
  for (const auto &r : requests) n += has(r, text);
  return n;
}
} // namespace
int runSuspendedTests() {
  int failures = 0;
  using namespace foil;
  Mock mock;
  std::string reason; // empty: the account is on
  bool sessions = false; // the server offers session tokens
  mock.answer = [&](const std::string &request) -> Reply {
    if (!reason.empty() && request.rfind("GET /api/library/info", 0) != 0)
      return {403, "X-CookingFoil-Session: suspended\r\n", R"({"error":"suspended","reason":")" + reason + R"("})"};
    if (request.rfind("GET /api/library/info", 0) == 0)
      return {200, "", sessions ? R"({"features":["session-token"]})" : R"({"features":[]})"};
    if (request.rfind("GET /api/session", 0) == 0) return {200, "", R"({"token":"cfs_back","expiresAt":0})"};
    if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":"approved"})"};
    if (request.rfind("GET /api/library/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
    return {200, "", "{}"};
  };
  char temporary[] = "/tmp/cookingfoil-suspended-XXXXXX";
  const char *made = mkdtemp(temporary);
  CHECK(made);
  if (!made) return failures;
  const std::filesystem::path root(made);
  const auto db = (root / "cookingfoil.db").string();
  const auto configPath = (root / "config.json").string();
  const std::string url = "http://127.0.0.1:" + std::to_string(mock.port);
  const std::string enrollToken = "cfe_" + std::string(43, 'b');
  std::string written;
  CHECK(atomicWrite(configPath,
                    R"({"servers":[{"title":"Main","url":")" + url + R"(","username":"","password":"","enrollToken":")" +
                        enrollToken + R"(","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"en","installTarget":"sd"})",
                    written));
  {
    auto state = loadState(db).value;
    state.accessKeys[url] = "held-key";
    Error error;
    CHECK(saveState(db, state, error));
  }
  const auto server = [&] {
    auto config = loadConfig(configPath);
    return config.value.servers.empty() ? Server{} : config.value.servers.front();
  };
  // Enrolling is not what this is about: the server ignores the token.
  net::Session::forgetEnrollAttempts();
  Error error;
  {
    // Turned off: the reason on the screen, the key and the enroll token
    // kept, no approval asked for.
    reason = "account-off";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(!session.refresh(error));
    CHECK(error.message == tr("This console's account is turned off. Contact the server's admin."));
    CHECK(!session.pairingPending());
    CHECK(loadState(db).value.accessKeys.at(url) == "held-key");
    CHECK(readFile(configPath).find(enrollToken) != std::string::npos);
    CHECK(net::consoleSuspension(url) == "account-off");
    CHECK(count(mock.take(), "POST /api/pair/request") == 0);
  }
  {
    // Deleted: its own reason.
    reason = "account-gone";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(!session.refresh(error));
    CHECK(error.message == tr("This console's account was deleted. Contact the server's admin."));
    CHECK(loadState(db).value.accessKeys.at(url) == "held-key");
    mock.take();
  }
  {
    // A session token refused while suspended is asked again once the
    // account is back, with the same key: not a refusal for the whole run.
    using namespace std::chrono_literals;
    net::setSessionRetryGap(0ms);
    sessions = true;
    reason = "account-off";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(!session.refresh(error));
    mock.take();
    reason.clear();
    CHECK(session.refresh(error));
    CHECK(!error);
    const auto sent = mock.take();
    CHECK(count(sent, "GET /api/session") >= 1 && count(sent, "X-Access-Key: held-key") >= 1);
    CHECK(count(sent, "Authorization: Bearer cfs_back") >= 1);
    CHECK(net::consoleSuspension(url).empty());
    net::setSessionRetryGap(std::chrono::seconds(net::RenewGapSeconds));
    net::forgetSessions();
    sessions = false;
  }
  {
    // A download refused as suspended is not a spent download token: none
    // is asked for again, and the console counts as suspended until a reply
    // goes through.
    const std::string file = url + "/api/files/x.nsp";
    const std::string status = "HTTP/1.1 403 X\r\n", header = "X-CookingFoil-Token: suspended\r\n", end = "\r\n";
    net::heardHeader(file, status.data(), status.size());
    net::heardHeader(file, header.data(), header.size());
    net::heardHeader(file, end.data(), end.size());
    CHECK(!net::downloadTokenRejected(url));
    CHECK(!net::consoleSuspension(url).empty());
    const std::string ok = "HTTP/1.1 200 X\r\n";
    net::heardHeader(file, ok.data(), ok.size());
    net::heardHeader(file, end.data(), end.size());
    CHECK(net::consoleSuspension(url).empty());
  }
  std::filesystem::remove_all(root);
  return failures;
}
