#include "Test.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/Resume.hpp"
#include <algorithm>
#include <iterator>
#include "core/State.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include "net/Socket.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
// The session token (feature "session-token"): asked for once with the
// sign-in, then sent to the server in place of Basic and the access key. A
// 401 that names it gets one new token and one more try; a token about to
// end is renewed before the next request; a download goes on from the same
// byte with the new one. A server that does not offer it is never asked.
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
const std::string *first(const std::vector<std::string> &requests, const std::string &text) {
  for (const auto &r : requests)
    if (has(r, text)) return &r;
  return nullptr;
}
std::string grant(const std::string &token, int64_t seconds) {
  const auto at = (int64_t(std::time(nullptr)) + seconds) * 1000;
  return "{\"token\":\"" + token + "\",\"expiresAt\":" + std::to_string(at) + "}";
}
} // namespace
int runSessionTokenTests() {
  int failures = 0;
  {
    // A server slow to give its token holds up only its own requests: another
    // server's token comes at once meanwhile, and a second request to the slow
    // one waits for the first ask rather than asking again.
    using namespace std::chrono_literals;
    const std::string slow = "https://slow.example.com", quick = "https://quick.example.com";
    std::atomic<bool> letGo{false};
    std::atomic<int> slowAsks{0};
    foil::net::offerSession(slow, "a", [&] {
      ++slowAsks;
      while (!letGo) std::this_thread::sleep_for(5ms);
      return foil::net::SessionGrant{200, "slow-token", 3600};
    });
    foil::net::offerSession(quick, "b", [] { return foil::net::SessionGrant{200, "quick-token", 3600}; });
    std::string first, second;
    std::thread asking([&] { first = foil::net::sessionToken(slow); });
    while (slowAsks == 0) std::this_thread::sleep_for(1ms);
    std::thread waiting([&] { second = foil::net::sessionToken(slow); });
    std::thread release([&] { // so a build that holds one lock for all ends too
      std::this_thread::sleep_for(1s);
      letGo = true;
    });
    const auto started = std::chrono::steady_clock::now();
    const auto other = foil::net::sessionToken(quick);
    const bool prompt = std::chrono::steady_clock::now() - started < 500ms;
    CHECK(other == "quick-token" && prompt);
    letGo = true;
    asking.join();
    waiting.join();
    release.join();
    CHECK(first == "slow-token" && second == "slow-token" && slowAsks == 1);
    CHECK(foil::net::isSessionToken("slow-token") && foil::net::isSessionToken("quick-token"));
    foil::net::withdrawSession(slow);
    foil::net::withdrawSession(quick);
  }
  Mock mock;
  foil::Server server;
  server.url = "http://127.0.0.1:" + std::to_string(mock.port);
  server.username = "user";
  server.password = "pass";
  foil::State state;
  state.deviceKey = std::string(64, 'a');
  state.accessKeys[server.url] = "acc";
  bool offers = false;
  int issued = 0;
  int64_t lifetime = 12 * 3600;
  int sessionStatus = 200;
  // Requests carrying these tokens are refused as expired.
  std::vector<std::string> dead;
  mock.answer = [&](const std::string &request) -> Reply {
    if (request.rfind("GET /api/shop/info", 0) == 0)
      return {200, "", offers ? R"({"features":["session-token"]})" : R"({"features":[]})"};
    if (request.rfind("GET /api/session", 0) == 0) {
      if (sessionStatus != 200) return {sessionStatus, "", "{}"};
      return {200, "Cache-Control: no-store\r\n", grant("cfs_t" + std::to_string(++issued), lifetime)};
    }
    for (const auto &token : dead)
      if (has(request, "Bearer " + token + "\r\n")) return {401, "X-CookingFoil-Session: expired\r\n", "{}"};
    if (request.rfind("GET /api/pair/status", 0) == 0) return {200, "", R"({"status":"approved"})"};
    if (request.rfind("GET /api/shop/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
    if (request.rfind("GET /file", 0) == 0) return {206, "Content-Range: bytes 4-9/10\r\nETag: \"f\"\r\n", "456789"};
    return {200, "", "{}"};
  };
  foil::Error error;
  const auto list = server.url + "/api/shop/sections";
  {
    // No "session-token": never asked, the sign-in as before.
    foil::net::forgetSessions();
    foil::net::Session session(server, state, "");
    session.refresh(error);
    const auto sent = mock.take();
    CHECK(count(sent, "/api/session") == 0);
    const auto *sections = first(sent, "GET /api/shop/sections");
    CHECK(sections && has(*sections, "Authorization: Basic ") && has(*sections, "X-Access-Key: acc") && !has(*sections, "Bearer"));
  }
  {
    // Offered: asked once with Basic, the device key and the access key;
    // every later request to the server carries the token and the device
    // key, and neither Basic nor the access key. The other headers stay.
    offers = true;
    foil::net::forgetSessions();
    foil::net::Session session(server, state, "");
    session.refresh(error);
    session.refresh(error);
    const auto sent = mock.take();
    CHECK(count(sent, "GET /api/session") == 1);
    const auto *asked = first(sent, "GET /api/session");
    CHECK(asked && has(*asked, "Authorization: Basic ") && has(*asked, "X-Access-Key: acc") && has(*asked, "X-Device-Key: "));
    CHECK(count(sent, "GET /api/shop/sections") == 2);
    for (const auto &r : sent)
      if (r.rfind("GET /api/shop/sections", 0) == 0)
        CHECK(has(r, "Authorization: Bearer cfs_t1\r\n") && !has(r, "Basic") && !has(r, "X-Access-Key") &&
              has(r, "X-Device-Key: ") && has(r, "X-CookingFoil-Features: "));
    // A picture on another host still gets nothing.
    const auto picture = foil::net::authHeaders(server, state, "https://images.example/icon.jpg");
    for (const auto &h : picture) CHECK(!has(h, "Authorization") && !has(h, "X-Device-Key"));
  }
  {
    // 401 naming the session token: one new token, the request once more.
    dead = {"cfs_t1"};
    const auto reply = foil::net::get(list, foil::net::authHeaders(server, state, list));
    CHECK(reply.status == 200);
    const auto sent = mock.take();
    CHECK(count(sent, "GET /api/session") == 1 && count(sent, "GET /api/shop/sections") == 2);
    if (sent.size() == 3) CHECK(has(sent[2], "Bearer cfs_t2\r\n") && !has(sent[2], "Basic"));
    // Refused again and the new ask refused too (401): no more tries, the
    // 401 goes to the caller (the login refused), and the requests after it
    // sign in as before.
    dead = {"cfs_t2"};
    sessionStatus = 401;
    const auto refused = foil::net::get(list, foil::net::authHeaders(server, state, list));
    CHECK(refused.status == 401);
    const auto again = mock.take();
    CHECK(count(again, "GET /api/session") == 1 && count(again, "GET /api/shop/sections") == 1);
    const auto after = foil::net::authHeaders(server, state, list);
    CHECK(std::any_of(after.begin(), after.end(), [](const std::string &h) { return has(h, "Basic"); }));
    CHECK(mock.take().empty()); // not asked again for this sign-in
  }
  {
    // A token that ends within ten minutes is renewed, with the sign-in,
    // before the next request goes.
    dead.clear();
    sessionStatus = 200;
    issued = 0;
    lifetime = 5 * 60;
    foil::net::forgetSessions();
    foil::net::offerSessionFor(server, state);
    const auto one = foil::net::authHeaders(server, state, list);
    lifetime = 12 * 3600;
    const auto two = foil::net::authHeaders(server, state, list);
    const auto three = foil::net::authHeaders(server, state, list);
    const auto sent = mock.take();
    CHECK(count(sent, "GET /api/session") == 2);
    for (const auto &r : sent) CHECK(has(r, "Authorization: Basic ") && !has(r, "Bearer"));
    CHECK(foil::net::bearerOf(one) == "cfs_t1" && foil::net::bearerOf(two) == "cfs_t2" && foil::net::bearerOf(three) == "cfs_t2");
  }
  {
    // A download refused halfway (401 with X-CookingFoil-Session): the token
    // alone changes and it goes on from the same byte.
    char temporary[] = "/tmp/cookingfoil-session-token-XXXXXX";
    const char *root = mkdtemp(temporary);
    CHECK(root);
    if (root) {
      const auto url = server.url + "/file";
      const std::string part = std::string(root) + "/file.part";
      foil::PartInfo wanted;
      wanted.url = url;
      wanted.size = 10;
      wanted.tag = "t";
      { std::ofstream(part, std::ios::binary) << "0123"; }
      std::string written;
      auto kept = wanted;
      kept.etag = "\"f\"";
      CHECK(foil::atomicWrite(foil::partInfoPath(part), foil::serializePartInfo(kept), written));
      dead = {"cfs_t2"};
      std::atomic<uint64_t> received{0};
      const auto got = foil::net::fetchPart(url, foil::net::downloadHeaders(server, state, url), part, wanted, false, received, nullptr);
      CHECK(got.response.status == 206 && got.size == 10);
      const auto sent = mock.take();
      CHECK(count(sent, "GET /api/session") == 1 && count(sent, "GET /file") == 2);
      if (sent.size() == 3) {
        CHECK(has(sent[0], "Bearer cfs_t2\r\n") && has(sent[0], "Range: bytes=4-"));
        CHECK(has(sent[2], "Bearer cfs_t3\r\n") && has(sent[2], "Range: bytes=4-") && !has(sent[2], "Basic"));
      }
      std::ifstream in(part, std::ios::binary);
      const std::string whole((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      CHECK(whole == "0123456789");
      std::filesystem::remove_all(root);
    }
  }
  {
    // A connection that never opened says so (curl 7): nothing was sent, so
    // a one-time request (Dropbox's consume) may go again.
    foil::net::PostOptions none;
    none.body = "{}";
    const auto refused = foil::net::post("http://127.0.0.1:9/api/x", {}, none);
    CHECK(refused.curl == 7 && refused.status == 0 && !refused.error.empty());
    CHECK(foil::net::get(server.url + "/api/shop/info", {}).curl == 0);
  }
  {
    // The token never reaches the log.
    CHECK(foil::withoutAddresses("{\"token\":\"cfs_abc-DEF.1\"} sent cfs_x9") == "{\"token\":\"***\"} sent ***");
  }
  foil::net::forgetSessions();
  return failures;
}
