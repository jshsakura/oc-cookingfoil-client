#include "Test.hpp"
#include "core/Config.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "net/ClientUpdateApi.hpp"
#include "net/Socket.hpp"
#include "MockServer.hpp"
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>
// A console whose account is turned off or deleted is suspended: the server
// answers 403 with X-CookingFoil-Session: suspended (the key and the session
// token) or X-CookingFoil-Token: suspended (a download), and keeps its key.
// The console keeps its access key and its enroll token, shows a re-pair QR
// code (asked with only the server address and its pair secret) so another
// account can take it, comes back with the same key when the account does,
// and still gets app updates.
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
  std::string repairKey; // what approving the re-pair QR code hands over
  const std::string sha(64, 'c');
  mock.answer = [&](const std::string &request) -> Reply {
    const bool keyed = has(request, "X-Access-Key:");
    // Open to anyone with the address, refused with a suspended key.
    if (request.rfind("GET /api/client/latest", 0) == 0 && !keyed && !has(request, "Authorization:"))
      return {200, "", R"({"version":"9.9.9","size":25,"sha256":")" + sha + R"(","url":"/api/client/cookingfoil.nro"})"};
    // Without the key: the re-pair QR code, or the new key once approved.
    if (!reason.empty() && request.rfind("GET /api/pair/status", 0) == 0 && !keyed && has(request, "X-Pair-Secret:"))
      return {200, "", repairKey.empty() ? R"({"status":"pending","repair":true,"pairUrl":"https://example.com/p/1","pairCode":"ABCD1234"})"
                                         : R"({"status":"approved","accessKey":")" + repairKey + R"("})"};
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
    // Turned off: a re-pair QR code, asked without the key; the key and
    // the enroll token kept.
    reason = "account-off";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(!session.refresh(error));
    CHECK(session.pairingPending());
    CHECK(session.pairingInfo().refusal == "suspended");
    CHECK(session.pairingInfo().code == "ABCD1234");
    CHECK(loadState(db).value.accessKeys.at(url) == "held-key");
    CHECK(readFile(configPath).find(enrollToken) != std::string::npos);
    const auto sent = mock.take();
    int keyless = 0;
    for (const auto &r : sent)
      keyless += r.rfind("GET /api/pair/status", 0) == 0 && !has(r, "X-Access-Key:") && has(r, "X-Pair-Secret:");
    CHECK(keyless >= 1);
    // The update is still there, asked without the key.
    net::ClientUpdateReply reply;
    const auto release = net::latestClient(server(), loadState(db).value, reply, nullptr);
    CHECK(release && release->version == "9.9.9");
    mock.take();
  }
  {
    // Deleted, and another account approves the QR code: its key replaces
    // the old one.
    reason = "account-gone";
    repairKey = "new-key";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    session.refresh(error);
    CHECK(!session.pairingPending());
    CHECK(loadState(db).value.accessKeys.at(url) == "new-key");
    repairKey.clear();
    mock.take();
    // Put back for what follows.
    auto state = loadState(db).value;
    state.accessKeys[url] = "held-key";
    Error saved;
    CHECK(saveState(db, state, saved));
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
