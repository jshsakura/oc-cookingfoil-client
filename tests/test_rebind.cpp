#include "Test.hpp"
#include "core/State.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
// The device key made from the console replaces the stored one per server:
// POST /api/pair/rebind goes before anything carries the new key, a server
// that took it gets the new key from then on, one that refused keeps the
// old key and is asked again next run, and one that does not offer
// "device-rebind" is never asked.
namespace {
struct Mock {
  int listener = -1;
  int port = 0;
  std::atomic<bool> stop{false};
  std::mutex lock;
  std::vector<std::string> requests; // head and body
  bool offers = true;
  int rebindStatus = 200;
  std::string rebindBody = R"({"status":"moved"})";
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
      int status = 200;
      std::string body = "{}";
      if (request.rfind("GET /api/library/info", 0) == 0)
        body = offers ? R"({"features":["device-rebind"]})" : R"({"features":[]})";
      else if (request.rfind("POST /api/pair/rebind", 0) == 0) {
        status = rebindStatus;
        body = rebindBody;
      } else if (request.rfind("GET /api/pair/status", 0) == 0)
        body = R"({"status":"approved"})";
      else if (request.rfind("GET /api/library/sections", 0) == 0)
        body = R"({"sections":[]})";
      {
        std::lock_guard<std::mutex> held(lock);
        requests.push_back(request);
      }
      const auto reply = "HTTP/1.1 " + std::to_string(status) + " X\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body;
      send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
      close(client);
    }
  }
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> held(lock);
    return std::exchange(requests, {});
  }
};
bool has(const std::string &request, const std::string &text) { return request.find(text) != std::string::npos; }
int firstWith(const std::vector<std::string> &requests, const std::string &text) {
  for (size_t i = 0; i < requests.size(); ++i)
    if (has(requests[i], text)) return int(i);
  return -1;
}
} // namespace
int runRebindTests() {
  int failures = 0;
  char temporary[] = "/tmp/cookingfoil-rebind-XXXXXX";
  const char *made = mkdtemp(temporary);
  CHECK(made);
  if (!made) return failures;
  const std::filesystem::path root(made);
  const auto db = (root / "cookingfoil.db").string();
  const auto console = foil::deriveIdentity(0x1122334455667788ull);
  foil::setDeviceIdentity(console);
  Mock mock;
  foil::Server server;
  server.url = "http://127.0.0.1:" + std::to_string(mock.port);
  server.username = "user";
  server.password = "pass";
  foil::setKnownServers(std::vector<std::string>{server.url});
  // A console from before: a random stored key, an access key under it.
  const std::string old(64, 'a');
  std::string written;
  CHECK(foil::atomicWrite((root / "state.json").string(),
                          "{\"deviceKey\":\"" + old + "\",\"accessKeys\":{\"" + server.url + "\":\"acc\"}}", written));
  const auto loaded = foil::loadState(db).value;
  CHECK(loaded.deviceKey == old && loaded.derivedKey == console.deviceKey && foil::needsRebind(loaded, server.url));
  foil::Error error;
  {
    // No "device-rebind": never asked, the old key goes on.
    mock.offers = false;
    foil::net::forgetRebindAttempts();
    foil::net::Session session(server, foil::loadState(db).value, db);
    session.refresh(error);
    const auto sent = mock.take();
    CHECK(firstWith(sent, "/api/pair/rebind") < 0);
    CHECK(firstWith(sent, "X-Device-Key: " + console.deviceKey) < 0);
    CHECK(firstWith(sent, "GET /api/pair/status") >= 0 && has(sent[size_t(firstWith(sent, "GET /api/pair/status"))], "X-Device-Key: " + old));
  }
  {
    // Refused (409): the old key goes on, not asked again this run, and
    // nothing changes on the card.
    mock.offers = true;
    mock.rebindStatus = 409;
    mock.rebindBody = R"({"error":"conflict"})";
    foil::net::forgetRebindAttempts();
    foil::net::Session session(server, foil::loadState(db).value, db);
    session.refresh(error);
    auto sent = mock.take();
    const int rebind = firstWith(sent, "POST /api/pair/rebind");
    CHECK(rebind >= 0);
    if (rebind >= 0) {
      const auto &request = sent[size_t(rebind)];
      CHECK(has(request, "X-Device-Key: " + console.deviceKey) && has(request, "X-Access-Key: acc"));
      CHECK(has(request, "Authorization: Basic ") && has(request, "{\"from\":\"" + old + "\"}"));
    }
    CHECK(firstWith(sent, "GET /api/pair/status") > rebind);
    CHECK(has(sent[size_t(firstWith(sent, "GET /api/pair/status"))], "X-Device-Key: " + old));
    session.refresh(error);
    sent = mock.take();
    CHECK(firstWith(sent, "/api/pair/rebind") < 0);
    const auto after = foil::loadState(db).value;
    CHECK(after.deviceKey == old && after.rebound.empty() && after.accessKeys.at(server.url) == "acc");
  }
  {
    // The next run, taken (200 "moved"): asked before anything carries the
    // new key, which every request uses from then on with the same access
    // key; every known server took it, so it is the stored key now.
    mock.rebindStatus = 200;
    mock.rebindBody = R"({"status":"moved"})";
    foil::net::forgetRebindAttempts();
    foil::net::Session session(server, foil::loadState(db).value, db);
    session.refresh(error);
    const auto sent = mock.take();
    const int rebind = firstWith(sent, "POST /api/pair/rebind");
    CHECK(rebind >= 0 && firstWith(sent, "X-Device-Key: " + console.deviceKey) == rebind);
    for (int i = 0; i < rebind; ++i) CHECK(!has(sent[size_t(i)], console.deviceKey));
    const int status = firstWith(sent, "GET /api/pair/status");
    CHECK(status > rebind && has(sent[size_t(status)], "X-Device-Key: " + console.deviceKey) && has(sent[size_t(status)], "X-Access-Key: acc"));
    const auto after = foil::loadState(db).value;
    CHECK(after.deviceKey == console.deviceKey && after.rebound.empty() && after.accessKeys.at(server.url) == "acc");
    // Never asked again.
    foil::net::forgetRebindAttempts();
    foil::net::Session next(server, after, db);
    next.refresh(error);
    const auto again = mock.take();
    CHECK(firstWith(again, "/api/pair/rebind") < 0 && firstWith(again, "X-Device-Key: " + old) < 0);
  }
  {
    // Two servers, one moved: that one gets the new key, the other the old,
    // and the stored key stays until both took it. "kept" counts as taken.
    foil::State state = foil::loadState(db).value;
    state.deviceKey = old;
    CHECK(foil::saveState(db, state, error));
    foil::setKnownServers(std::vector<std::string>{server.url, "https://other.example.com"});
    mock.rebindBody = R"({"status":"kept"})";
    foil::net::forgetRebindAttempts();
    foil::net::Session session(server, foil::loadState(db).value, db);
    session.refresh(error);
    mock.take();
    const auto after = foil::loadState(db).value;
    CHECK(after.deviceKey == old && after.rebound.count(server.url));
    CHECK(foil::deviceKeyFor(after, server.url) == console.deviceKey);
    CHECK(foil::deviceKeyFor(after, "https://other.example.com") == old);
    // The card in another console: the record of who took which key goes.
    foil::setDeviceIdentity(foil::deriveIdentity(0x99));
    CHECK(foil::loadState(db).value.rebound.empty());
    foil::takeRebound();
  }
  foil::setKnownServers(std::nullopt);
  foil::setDeviceIdentity(foil::deriveIdentity(0x756e697474657374ull));
  foil::net::forgetRebindAttempts();
  std::filesystem::remove_all(root);
  return failures;
}
