#include "Test.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
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
  int enrollStatus = 200;
  mock.answer = [&](const std::string &request) -> Reply {
    if (request.rfind("POST /api/pair/enroll", 0) == 0) {
      if (enrollStatus == 401) return {401, "X-CookingFoil-Enroll: invalid\r\n", R"({"error":"x"})"};
      if (enrollStatus != 200) return {enrollStatus, "", R"({"error":"x"})"};
      return {200, "", R"({"status":"approved","accessKey":"enrolled-key","shopUrl":null})"};
    }
    if (request.rfind("GET /api/shop/info", 0) == 0) return {200, "", R"({"features":["enroll"]})"};
    if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":"approved"})"};
    if (request.rfind("GET /api/shop/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
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
  }
  const auto withoutKey = [&] {
    auto state = loadState(db).value;
    state.accessKeys.clear();
    CHECK(saveState(db, state, error));
    writeConfig();
    net::Session::forgetEnrollAttempts();
  };
  {
    // 401 (expired, spent, unknown alike): says to download again, keeps
    // the token, and is not asked again this run.
    withoutKey();
    enrollStatus = 401;
    net::Session session(server(), loadState(db).value, db);
    session.setConfigPath(configPath);
    Error refused;
    CHECK(!session.refresh(refused));
    CHECK(refused.message == tr("The link key has expired or is not registered. Download the client again from the website."));
    CHECK(server().enrollToken == token && loadState(db).value.accessKeys.empty());
    CHECK(count(mock.take(), "/api/pair/enroll") == 1);
    Error again;
    CHECK(!session.refresh(again) && again.message == refused.message);
    CHECK(count(mock.take(), "/api/pair/enroll") == 0);
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
