#include "Test.hpp"
#include "net/Direct.hpp"
#include "net/Ranged.hpp"
#include "net/Segmented.hpp"
#include "net/Socket.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>
// A download from the server's own address goes to its direct address
// (X-CookingFoil-Download-Origin) with the download token alone, and only
// there: the direct address failing fails the download; it never goes to
// the public address instead (that one may be metered).
namespace {
// Serves one file with ranges (206) over plain HTTP, keeping each request.
struct Files {
  int listener = -1, port = 0;
  std::string body;
  std::atomic<bool> stop{false};
  std::mutex lock;
  std::vector<std::string> requests;
  std::thread thread;
  Files() {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr *>(&at), sizeof(at));
    listen(listener, 16);
    socklen_t size = sizeof(at);
    getsockname(listener, reinterpret_cast<sockaddr *>(&at), &size);
    port = ntohs(at.sin_port);
    thread = std::thread([this] { serve(); });
  }
  ~Files() {
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
      {
        std::lock_guard<std::mutex> held(lock);
        requests.push_back(request);
      }
      size_t from = 0, to = body.size() - 1;
      const auto range = request.find("Range: bytes=");
      if (range != std::string::npos) {
        unsigned long long a = 0, b = 0;
        const int n = std::sscanf(request.c_str() + range + 13, "%llu-%llu", &a, &b);
        from = size_t(a);
        if (n == 2 && b < body.size()) to = size_t(b);
      }
      const auto part = body.substr(from, to - from + 1);
      const auto head = "HTTP/1.1 206 Partial\r\nConnection: close\r\nContent-Range: bytes " + std::to_string(from) + "-" +
                        std::to_string(to) + "/" + std::to_string(body.size()) +
                        "\r\nContent-Length: " + std::to_string(part.size()) + "\r\n\r\n";
      const auto text = head + part;
      size_t sent = 0;
      while (sent < text.size()) {
        const auto n = send(client, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += size_t(n);
      }
      close(client);
    }
  }
  std::vector<std::string> take() {
    std::lock_guard<std::mutex> held(lock);
    return std::exchange(requests, {});
  }
};
void hear(const std::string &line) { foil::net::heardHeader(line.data(), line.size()); }
bool has(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }
} // namespace
int runDirectTests() {
  int failures = 0;
  using namespace foil::net;
  forgetDirect();
  forgetDownloadToken();
  const std::string shop = "http://127.0.0.1:38000";
  const std::string game = shop + "/api/get/Game%20One.nsz?v=3";
  {
    // Nothing named: the server's own address, its keys as they are.
    setShopOrigin(shop);
    CHECK(routeDownload(game) == game);
    CHECK(routeName(game) == "server");
    hear("X-CookingFoil-Download-Origin: https://dl.example:8443\r\n");
    CHECK(routeDownload(game) == game); // no token yet
    hear("X-CookingFoil-Download-Token: first\r\n");
    // Both: the same path and query on the direct address.
    CHECK(routeDownload(game) == "https://dl.example:8443/api/get/Game%20One.nsz?v=3");
    CHECK(routeName(game) == "direct");
    CHECK(routeDownload(shop + "/api/patches/7/files") == "https://dl.example:8443/api/patches/7/files");
    // Another host's file (a mirror) stays where it is.
    CHECK(routeDownload("https://mirror.example/x.nsp") == "https://mirror.example/x.nsp");
    // The direct address gets the token alone; anywhere else the keys stay.
    const std::vector<std::string> keys{"Authorization: Basic dXNlcjpwYXNz", "X-Access-Key: k"};
    const auto direct = routeDownload(game);
    CHECK(headersFor(direct, keys) == std::vector<std::string>{"Authorization: Bearer first"});
    CHECK(headersFor(game, keys) == keys);
    // Failures other than a 401 are the caller's, as always; the route stays.
    CHECK(afterDirectFailure(direct, 404, "first") == DirectStep::Other);
    CHECK(afterDirectFailure(direct, 502, "first") == DirectStep::Other);
    CHECK(afterDirectFailure(direct, 0, "first") == DirectStep::Other);
    CHECK(afterDirectFailure(game, 401, "first") == DirectStep::Other);
    CHECK(routeName(game) == "direct");
  }
  {
    // 401: a fresh token once; refused with it too, the caller's (a failure).
    forgetDirect();
    setShopOrigin(shop);
    int asked = 0;
    setTokenRenewer([&] {
      ++asked;
      hear("X-CookingFoil-Download-Token: second\r\n");
      return 200L;
    });
    const auto direct = routeDownload(game);
    CHECK(afterDirectFailure(direct, 401, "first") == DirectStep::Again);
    CHECK(asked == 1 && downloadToken() == "second");
    CHECK(afterDirectFailure(direct, 401, "first") == DirectStep::Again); // a part sent with the old one
    CHECK(asked == 1);
    CHECK(afterDirectFailure(direct, 401, "second") == DirectStep::Other);
    CHECK(routeDownload(game) == direct); // still the only way
    setTokenRenewer(nullptr);
  }
  Files files;
  files.body.resize(3 << 20);
  for (size_t i = 0; i < files.body.size(); ++i) files.body[i] = char('a' + (i * 7) % 26);
  const std::string url = "http://127.0.0.1:" + std::to_string(files.port) + "/api/get/game.nsp?k=1";
  const std::vector<std::string> keys{"X-Access-Key: shop-key"};
  {
    // The direct address refuses the connection: it is asked again there
    // (a dead one for minutes, as any server) until it fails or is
    // cancelled, and nothing reaches the public address.
    forgetDirect();
    setShopOrigin("http://127.0.0.1:" + std::to_string(files.port));
    hear("X-CookingFoil-Download-Origin: https://127.0.0.1:1\r\n");
    CHECK(routeName(url) == "direct");
    SegmentedOptions options;
    options.connections = 2;
    options.headers = keys;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    options.cancelled = [until] { return std::chrono::steady_clock::now() > until; };
    const auto result = segmentedFetch(
        url, [](uint64_t) { return true; }, [](uint64_t, const char *, size_t) { return true; }, options);
    CHECK(!result.ok && result.cancelled);
    RangedSink sink;
    std::string got;
    sink.kept = [&] { return uint64_t(got.size()); };
    sink.begin = [&](uint64_t from, const Response &) {
      got.resize(size_t(from));
      return true;
    };
    sink.data = [&](const char *data, size_t size) {
      got.append(data, size);
      return true;
    };
    CHECK(!rangedOk(rangedStream(url, keys, "", sink, nullptr)));
    CHECK(files.take().empty());
  }
  {
    // No direct address named: the server's own, with its keys.
    forgetDirect();
    forgetDownloadToken();
    setShopOrigin("http://127.0.0.1:" + std::to_string(files.port));
    std::string got(files.body.size(), '\0');
    SegmentedOptions options;
    options.connections = 3;
    options.headers = keys;
    const auto result = segmentedFetch(
        url, [](uint64_t) { return true; },
        [&](uint64_t offset, const char *data, size_t size) {
          if (offset + size > got.size()) return false;
          std::memcpy(&got[size_t(offset)], data, size);
          return true;
        },
        options);
    CHECK(result.ok && got == files.body);
    const auto sent = files.take();
    CHECK(!sent.empty());
    for (const auto &request : sent) CHECK(has(request, "X-Access-Key: shop-key") && !has(request, "Bearer"));
  }
  forgetDirect();
  forgetDownloadToken();
  return failures;
}
