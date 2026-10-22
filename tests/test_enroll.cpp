#include "Test.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "net/Socket.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
// A client downloaded from the website carries a one-time enroll token in
// config.json: spent once (POST /api/pair/enroll) for this console's access
// key before anything else, then taken out of the file. Refused, it says
// why and stays; a console with a key never spends it; a server that does
// not know enroll ignores it.
namespace {
struct Reply {
  int status = 200;
  std::string headers, body;
};
struct Mock {
  int listener = -1;
  int port = 0;
  std::atomic<bool> stop{false};
  std::mutex lock;
  std::vector<std::string> requests;
  std::function<Reply(const std::string &)> answer;
  std::thread thread;
  Mock() {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr *>(&at), sizeof(at));
    listen(listener, 8);
    socklen_t size = sizeof(at);
    getsockname(listener, reinterpret_cast<sockaddr *>(&at), &size);
    port = ntohs(at.sin_port);
    thread = std::thread([this] { serve(); });
  }
  ~Mock() {
    stop = true;
    thread.join();
    close(listener);
  }
  void serve() {
    while (!stop) {
      pollfd ready{listener, POLLIN, 0};
      if (poll(&ready, 1, 50) <= 0) continue;
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0) continue;
      std::string request;
      char buffer[4096];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const auto got = recv(client, buffer, sizeof buffer, 0);
        if (got <= 0) break;
        request.append(buffer, size_t(got));
      }
      const auto end = request.find("\r\n\r\n");
      const auto length = request.find("Content-Length: ");
      const size_t want = length != std::string::npos && length < end ? std::stoul(request.substr(length + 16)) : 0;
      while (end != std::string::npos && request.size() < end + 4 + want) {
        const auto got = recv(client, buffer, sizeof buffer, 0);
        if (got <= 0) break;
        request.append(buffer, size_t(got));
      }
      Reply reply;
      {
        std::lock_guard<std::mutex> held(lock);
        requests.push_back(request);
        reply = answer(request);
      }
      const auto text = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nConnection: close\r\n" + reply.headers +
                        "Content-Length: " + std::to_string(reply.body.size()) + "\r\n\r\n" + reply.body;
      send(client, text.data(), text.size(), MSG_NOSIGNAL);
      close(client);
    }
  }
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> held(lock);
    return std::exchange(requests, {});
  }
};
bool has(const std::string &request, const std::string &text) { return request.find(text) != std::string::npos; }
int count(const std::vector<std::string> &requests, const std::string &text) {
  int n = 0;
  for (const auto &r : requests) n += has(r, text);
  return n;
}
} // namespace
int runEnrollTests() {
  int failures = 0;
  using namespace foil;
  Mock mock;
  int enrollStatus = 200, sectionsStatus = 200;
  std::string pairStatus = "approved";
  mock.answer = [&](const std::string &request) -> Reply {
    if (request.rfind("POST /api/pair/enroll", 0) == 0) {
      if (enrollStatus == 401) return {401, "X-CookingFoil-Enroll: invalid\r\n", R"({"error":"x"})"};
      if (enrollStatus != 200) return {enrollStatus, "", R"({"error":"x"})"};
      return {200, "", R"({"status":"approved","accessKey":"enrolled-key","shopUrl":null})"};
    }
    if (request.rfind("GET /api/shop/info", 0) == 0) return {200, "", R"({"features":["enroll"]})"};
    if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":")" + pairStatus + R"("})"};
    if (request.rfind("GET /api/shop/sections", 0) == 0)
      return sectionsStatus == 200 ? Reply{200, "", R"({"sections":[]})"} : Reply{sectionsStatus, "", R"({"error":"denied"})"};
    return {200, "", "{}"};
  };
  char temporary[] = "/tmp/cookingfoil-enroll-XXXXXX";
  const char *made = mkdtemp(temporary);
  CHECK(made);
  if (!made) return failures;
  const std::filesystem::path root(made);
  const auto db = (root / "cookingfoil.db").string();
  const auto configPath = (root / "config.json").string();
  const std::string url = "http://127.0.0.1:" + std::to_string(mock.port);
  const std::string token = "cfe_" + std::string(43, 'a');
  const auto writeConfig = [&] {
    std::string written;
    CHECK(atomicWrite(configPath,
                      R"({"servers":[{"title":"Shop","url":")" + url + R"(","username":"","password":"","enrollToken":")" + token +
                          R"(","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"ko","installTarget":"sd"})",
                      written));
  };
  const auto server = [&] {
    auto config = loadConfig(configPath);
    CHECK(!config.error && config.value.servers.size() == 1);
    return config.value.servers.empty() ? Server{} : config.value.servers.front();
  };
  Error error;
  {
    // Token, no key: spent once, first; the key saved; the token out of
    // config.json; after it, no Basic (no username) and the access key.
    writeConfig();
    CHECK(server().enrollToken == token);
    net::Session::forgetEnrollAttempts();
    const auto state = loadState(db).value;
    net::Session session(server(), state, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error));
    const auto sent = mock.take();
    CHECK(!sent.empty() && sent.front().rfind("POST /api/pair/enroll", 0) == 0);
    CHECK(count(sent, "POST /api/pair/enroll") == 1);
    if (!sent.empty()) {
      CHECK(has(sent.front(), "X-Device-Key: " + state.derivedKey) && has(sent.front(), "\"token\":\"" + token + "\""));
      CHECK(has(sent.front(), "Content-Type: application/json"));
    }
    CHECK(loadState(db).value.accessKeys.at(url) == "enrolled-key");
    CHECK(server().enrollToken.empty() && readFile(configPath).find(token) == std::string::npos);
    for (size_t i = 1; i < sent.size(); ++i)
      CHECK(!has(sent[i], "Authorization: Basic") && has(sent[i], "X-Access-Key: enrolled-key"));
    // With the key, never again (even with the token back in the file).
    writeConfig();
    net::Session again(server(), loadState(db).value, db);
    again.setConfigPath(configPath);
    CHECK(again.refresh(error));
    CHECK(count(mock.take(), "/api/pair/enroll") == 0);
    // A list refused with none of the server's own headers: the screen names
    // the host it went to, the log how it signed in and what came back.
    const auto logged = (root / "log.txt").string();
    openLog(logged);
    sectionsStatus = 401;
    net::Session refused(server(), loadState(db).value, db);
    refused.setConfigPath(configPath);
    CHECK(!refused.refresh(error));
    CHECK(error.message.find("401") != std::string::npos && error.message.find("127.0.0.1") != std::string::npos);
    flushLog();
    const auto line = readFile(logged);
    CHECK(line.find("list refused: HTTP 401, signed in by") != std::string::npos);
    CHECK(line.find("session header none, enroll header none, auth header none, error denied") != std::string::npos);
    sectionsStatus = 200;
    mock.take();
  }
  {
    // One server under two addresses (its LAN one and a tunnel): the second,
    // with no key, sends the first's session token once; answered with the
    // first's server id it takes the first's key and asks for no approval
    // of its own. Another id, or none, and nothing is lent. A key the
    // server calls wrong goes, and approval is asked again.
    const auto kept = mock.answer;
    const std::string other = "http://localhost:" + std::to_string(mock.port);
    std::string probeId = "server-one", wrong;
    mock.answer = [&](const std::string &request) -> Reply {
      if (request.rfind("GET /api/session", 0) == 0)
        return has(request, "X-Access-Key: shared-key") ? Reply{200, "", R"({"token":"cfs_lent","expiresAt":0})"}
                                                        : Reply{401, "", "{}"};
      if (request.rfind("GET /api/shop/info", 0) == 0 && has(request, "Authorization: Bearer cfs_lent"))
        return probeId.empty() ? Reply{401, "X-CookingFoil-Auth: session-invalid\r\n", "{}"}
                               : Reply{200, "X-CookingFoil-Server-Id: " + probeId + "\r\n", "{}"};
      if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":")" + pairStatus + R"("})"};
      if (request.rfind("GET /api/shop/sections", 0) == 0) {
        if (!wrong.empty() && has(request, "X-Access-Key: " + wrong))
          return {401, "X-CookingFoil-Server-Id: server-one\r\nX-CookingFoil-Auth: access-key-wrong\r\n", "no"};
        if (has(request, "X-Access-Key: shared-key")) return {200, "", R"({"sections":[]})"};
        return {401, "", "no access key"};
      }
      return {200, "", "{}"};
    };
    const auto lender = [&](const std::string &id) {
      auto state = loadState(db).value;
      state.accessKeys = {{url, "shared-key"}};
      state.serverIds = {{url, id}};
      state.pairSecrets.clear();
      CHECK(saveState(db, state, error));
      net::Session::forgetKeyProbes();
    };
    auto second = server();
    second.url = other;
    second.enrollToken.clear();
    {
      lender("server-one");
      net::Session session(second, loadState(db).value, db);
      CHECK(session.refresh(error) && !session.pairingPending());
      const auto sent = mock.take();
      CHECK(count(sent, "Authorization: Bearer cfs_lent") == 1);
      CHECK(count(sent, "/api/shop/sections") == 1 && count(sent, "/api/pair/request") == 0);
      const auto saved = loadState(db).value;
      CHECK(saved.accessKeys.count(other) && saved.accessKeys.at(other) == "shared-key");
      CHECK(saved.serverIds.count(other) && saved.serverIds.at(other) == "server-one");
      CHECK(!saved.pairSecrets.count(other));
    }
    {
      // A 200 alone proves nothing (anything can answer 200): the id must match.
      lender("server-two");
      net::Session session(second, loadState(db).value, db);
      CHECK(!session.refresh(error) && error.message.find("401") != std::string::npos);
      const auto sent = mock.take();
      CHECK(count(sent, "Authorization: Bearer cfs_lent") == 1);
      CHECK(!loadState(db).value.accessKeys.count(other));
      // Tried once a run: the next refresh sends the token nowhere.
      net::Session again(second, loadState(db).value, db);
      CHECK(!again.refresh(error));
      CHECK(count(mock.take(), "Authorization: Bearer cfs_lent") == 0);
    }
    {
      // Another server (its own 401, no id): nothing is lent.
      lender("server-one");
      probeId.clear();
      net::Session session(second, loadState(db).value, db);
      CHECK(!session.refresh(error));
      CHECK(!loadState(db).value.accessKeys.count(other));
      mock.take();
      probeId = "server-one";
    }
    {
      // access-key-wrong: the key goes and approval is asked again (the
      // QR code), not "try again"; the same key held by the other address
      // is never borrowed back.
      auto again = loadState(db).value;
      again.accessKeys = {{url, "stale-key"}, {other, "stale-key"}};
      again.serverIds = {{url, "server-one"}, {other, "server-one"}};
      CHECK(saveState(db, again, error));
      net::Session::forgetKeyProbes();
      wrong = "stale-key";
      pairStatus = "pending";
      net::Session session(second, loadState(db).value, db);
      Error waiting;
      CHECK(!session.refresh(waiting) && session.pairingPending());
      const auto saved = loadState(db).value;
      CHECK(!saved.accessKeys.count(other) && saved.accessKeys.size() == 1);
      const auto sent = mock.take();
      CHECK(count(sent, "POST /api/pair/request") == 1 && count(sent, "Authorization: Bearer") == 0);
      pairStatus = "approved";
      wrong.clear();
    }
    auto restored = loadState(db).value;
    restored.accessKeys = {{url, "enrolled-key"}};
    restored.serverIds.clear();
    restored.pairSecrets.clear();
    CHECK(saveState(db, restored, error));
    net::Session::forgetKeyProbes();
    mock.answer = kept;
  }
  {
    // A key kept from before that the server no longer approves (the console
    // was removed): the token approves it again in place of a new request.
    auto state = loadState(db).value;
    state.accessKeys[url] = "removed-key";
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
    pairStatus = "pending";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    CHECK(session.refresh(error));
    const auto sent = mock.take();
    CHECK(count(sent, "POST /api/pair/enroll") == 1 && count(sent, "/api/pair/request") == 0);
    CHECK(count(sent, "/api/shop/sections") == 1 && !session.pairingPending());
    CHECK(loadState(db).value.accessKeys.at(url) == "enrolled-key");
    CHECK(server().enrollToken.empty());
    pairStatus = "approved";
  }
  const auto withoutKey = [&] {
    auto state = loadState(db).value;
    state.accessKeys.clear();
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
  };
  {
    // 401 (expired, spent, unknown alike): the token is out of config.json
    // and the console asks for approval (the QR code) without it.
    withoutKey();
    enrollStatus = 401;
    pairStatus = "pending";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error waiting;
    CHECK(!session.refresh(waiting) && session.pairingPending());
    CHECK(server().enrollToken.empty() && loadState(db).value.accessKeys.empty());
    const auto sent = mock.take();
    CHECK(count(sent, "/api/pair/enroll") == 1 && count(sent, "POST /api/pair/request") == 1);
    for (const auto &request : sent)
      if (request.rfind("POST /api/pair/enroll", 0) != 0) CHECK(!has(request, token));
    CHECK(!session.refresh(waiting) && count(mock.take(), "/api/pair/enroll") == 0);
    pairStatus = "approved";
  }
  {
    // 409: another account's console.
    withoutKey();
    enrollStatus = 409;
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error refused;
    CHECK(!session.refresh(refused) && refused.message == tr("This console is linked to another account."));
    CHECK(server().enrollToken == token);
    mock.take();
  }
  {
    // A server that does not know enroll: the token is ignored and the
    // console goes on as before.
    withoutKey();
    enrollStatus = 404;
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error none;
    CHECK(session.refresh(none));
    const auto sent = mock.take();
    CHECK(count(sent, "/api/pair/enroll") == 1 && count(sent, "/api/shop/sections") == 1);
    CHECK(server().enrollToken == token);
    // Still pending there: the request carries the token, so the server
    // can approve it under the download's account.
    pairStatus = "pending";
    Error waiting;
    CHECK(!session.refresh(waiting) && session.pairingPending());
    bool carried = false;
    for (const auto &request : mock.take())
      if (request.rfind("POST /api/pair/request", 0) == 0) carried = has(request, "\"enrollToken\":\"" + token + "\"");
    CHECK(carried);
    pairStatus = "approved";
  }
  {
    // A console the server took back (its account off, its password
    // changed), with no enroll token: the kept key goes, and approval is
    // asked again with a new pair secret, so the key the server issues next
    // goes only to this console. Then the QR code shows.
    std::string written;
    CHECK(atomicWrite(configPath,
                      R"({"servers":[{"title":"Shop","url":")" + url +
                          R"(","username":"","password":"","enrollToken":"","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"ko","installTarget":"sd"})",
                      written));
    auto state = loadState(db).value;
    state.accessKeys[url] = "revoked-key";
    state.pairSecrets[url] = std::string(64, 'c'); // one the server no longer holds
    CHECK(saveState(db, state, error));
    pairStatus = "pending";
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error waiting;
    CHECK(!session.refresh(waiting) && session.pairingPending());
    const auto kept = loadState(db).value;
    CHECK(!kept.accessKeys.count(url));
    const auto secret = kept.pairSecrets.count(url) ? kept.pairSecrets.at(url) : std::string();
    CHECK(secret.size() == 64 && secret != std::string(64, 'c'));
    const auto sent = mock.take();
    int asked = 0;
    for (const auto &request : sent)
      if (request.rfind("GET /api/pair/status", 0) == 0 && has(request, "X-Pair-Secret: " + secret)) {
        ++asked;
        CHECK(!has(request, "revoked-key"));
      }
    CHECK(asked == 1);
    CHECK(count(sent, "POST /api/pair/request") == 1);
    pairStatus = "approved";
  }
  {
    // An ID and password are for approval on a server that says
    // pair-password-only: sent to /api/pair/* alone, and once a key came,
    // taken out of config.json, so they no longer cross the network or sit
    // on the card. A server without the mark gets them as before.
    const auto withPassword = [&](bool only) {
      std::string written;
      CHECK(atomicWrite(configPath,
                        R"({"servers":[{"title":"Shop","url":")" + url +
                            R"(","username":"alice","password":"secret-pass","enrollToken":"","cfClientId":"","cfClientSecret":"","enabled":true}],"language":"ko","installTarget":"sd"})",
                        written));
      auto state = loadState(db).value;
      state.accessKeys.clear();
      state.pairSecrets.clear();
      CHECK(saveState(db, state, error));
      mock.answer = [only](const std::string &request) -> Reply {
        if (request.rfind("GET /api/pair/status", 0) == 0)
          return only ? Reply{200, "", R"({"status":"approved","accessKey":"password-key","passwordOnly":true})"}
                      : Reply{404, "", "{}"};
        if (request.rfind("GET /api/shop/info", 0) == 0)
          return {200, "", only ? R"({"features":["pair-password-only"]})" : R"({"features":[]})"};
        if (request.rfind("GET /api/shop/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
        return {200, "", "{}"};
      };
    };
    const auto basic = std::string("Authorization: Basic ");
    withPassword(true);
    {
      net::Session session(server(), loadState(db).value, db);
      session.setConfigPath(configPath);
      CHECK(session.refresh(error));
      const auto sent = mock.take();
      // The approval says passwordOnly itself: nothing after it carries
      // the password, /api/shop/info included.
      for (const auto &request : sent) {
        const bool pairing = request.find(" /api/pair/") != std::string::npos;
        CHECK(has(request, basic) == pairing);
      }
      CHECK(count(sent, "GET /api/pair/status") >= 1);
      CHECK(loadState(db).value.accessKeys.at(url) == "password-key");
      CHECK(server().password.empty() && readFile(configPath).find("secret-pass") == std::string::npos);
      CHECK(server().username == "alice"); // kept: the form shows whose it was
      CHECK(session.server().password.empty());
      // Next time no password goes anywhere, the pairing paths included.
      CHECK(session.refresh(error));
      for (const auto &request : mock.take()) CHECK(!has(request, basic));
    }
    withPassword(false);
    {
      net::Session session(server(), loadState(db).value, db);
      session.setConfigPath(configPath);
      CHECK(session.refresh(error));
      mock.take();
      CHECK(session.refresh(error));
      CHECK(count(mock.take(), basic) >= 3); // a server with no pairing: Basic everywhere, as before
      CHECK(server().password == "secret-pass");
    }
  }
  {
    // The server says so on any reply: once, for the screen to refresh.
    mock.answer = [](const std::string &) -> Reply {
      return {401, "X-CookingFoil-Session: revoked\r\n", R"({"error":"session revoked"})"};
    };
    net::takeConsoleRevoked(url); // what tests before this one left
    net::get(url + "/api/shop/sections", {}, "", nullptr);
    CHECK(net::takeConsoleRevoked(url) && !net::takeConsoleRevoked(url));
    mock.answer = [](const std::string &) -> Reply {
      return {401, "X-CookingFoil-Session: expired\r\n", "{}"};
    };
    net::get(url + "/api/shop/sections", {}, "", nullptr);
    CHECK(!net::takeConsoleRevoked(url)); // expired is renewed, not a take-back
    mock.take();
  }
  {
    // A username still signs in with Basic, as before; none sends none.
    Server shop;
    shop.url = url;
    State state;
    state.deviceKey = std::string(64, 'a');
    const auto basic = [&](const Server &s) {
      for (const auto &h : net::authHeaders(s, state, url + "/api/shop/sections"))
        if (h.rfind("Authorization: Basic", 0) == 0) return true;
      return false;
    };
    shop.password = "only-a-password";
    CHECK(!basic(shop));
    shop.username = "user";
    CHECK(basic(shop));
  }
  std::filesystem::remove_all(root);
  return failures;
}
