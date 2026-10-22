#include "Test.hpp"
#include "net/Segmented.hpp"
#include "net/Segments.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
// When the limit drops while parts are being read (a 429, the tuner stepping
// back), the connections over it go at once: waiting for their parts to end
// would keep them reading for half the file.
namespace {
using Clock = std::chrono::steady_clock;
// Serves a file of `size` bytes with ranges, slowly and on many connections
// at once, counting those open; the third request is answered 429.
struct SlowFiles {
  uint64_t size;
  int listener = -1, port = 0;
  std::atomic<bool> stop{false};
  std::atomic<int> open{0}, asked{0};
  std::atomic<bool> limited{false};
  std::atomic<long long> limitedAt{0}; // ms since start, once limited
  Clock::time_point started = Clock::now();
  std::mutex lock;
  std::vector<std::thread> clients;
  std::thread thread;
  explicit SlowFiles(uint64_t bytes) : size(bytes) {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr *>(&at), sizeof(at));
    listen(listener, 16);
    socklen_t length = sizeof(at);
    getsockname(listener, reinterpret_cast<sockaddr *>(&at), &length);
    port = ntohs(at.sin_port);
    thread = std::thread([this] { serve(); });
  }
  ~SlowFiles() {
    stop = true;
    thread.join();
    for (auto &client : clients) client.join();
    close(listener);
  }
  void serve() {
    while (!stop) {
      pollfd ready{listener, POLLIN, 0};
      if (poll(&ready, 1, 50) <= 0) continue;
      const int client = accept(listener, nullptr, nullptr);
      if (client < 0) continue;
      std::lock_guard<std::mutex> held(lock);
      clients.emplace_back([this, client] { answer(client); });
    }
  }
  // The client went away (shed, or done): a read says so at once.
  static bool gone(int client) {
    char byte;
    const auto got = recv(client, &byte, 1, MSG_DONTWAIT);
    return got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK);
  }
  void answer(int client) {
    ++open;
    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos) {
      const auto got = recv(client, buffer, sizeof buffer, 0);
      if (got <= 0) break;
      request.append(buffer, size_t(got));
    }
    if (++asked == 3) {
      const std::string busy = "HTTP/1.1 429 Too Many\r\nRetry-After: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      send(client, busy.data(), busy.size(), MSG_NOSIGNAL);
      limitedAt = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
      limited = true; // within the first millisecond limitedAt is 0
      --open;
      close(client);
      return;
    }
    unsigned long long from = 0, to = size - 1;
    const auto range = request.find("Range: bytes=");
    if (range != std::string::npos) {
      unsigned long long a = 0, b = 0;
      const int n = std::sscanf(request.c_str() + range + 13, "%llu-%llu", &a, &b);
      from = a;
      if (n == 2 && b < size) to = b;
    }
    const auto head = "HTTP/1.1 206 Partial\r\nConnection: close\r\nContent-Range: bytes " + std::to_string(from) + "-" +
                      std::to_string(to) + "/" + std::to_string(size) +
                      "\r\nContent-Length: " + std::to_string(to - from + 1) + "\r\n\r\n";
    send(client, head.data(), head.size(), MSG_NOSIGNAL);
    std::vector<char> chunk(64 << 10, 'x');
    for (uint64_t at = from; at <= to && !stop && !gone(client);) {
      const auto n = std::min<uint64_t>(chunk.size(), to - at + 1);
      if (send(client, chunk.data(), size_t(n), MSG_NOSIGNAL) <= 0) break;
      at += n;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    --open;
    close(client);
  }
};
// Answers each connection with the next reply in `replies` (the last one
// again once they run out), counting the requests.
struct Scripted {
  std::vector<std::string> replies;
  int listener = -1, port = 0;
  std::atomic<bool> stop{false};
  std::atomic<int> asked{0};
  std::thread thread;
  explicit Scripted(std::vector<std::string> said) : replies(std::move(said)) {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr *>(&at), sizeof(at));
    listen(listener, 16);
    socklen_t length = sizeof(at);
    getsockname(listener, reinterpret_cast<sockaddr *>(&at), &length);
    port = ntohs(at.sin_port);
    thread = std::thread([this] {
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
        const auto &reply = replies[std::min<size_t>(size_t(asked++), replies.size() - 1)];
        send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        close(client);
      }
    });
  }
  ~Scripted() {
    stop = true;
    thread.join();
    close(listener);
  }
};
std::string whole(const std::string &body) {
  return "HTTP/1.1 206 Partial\r\nConnection: close\r\nETag: \"v1\"\r\nContent-Range: bytes 0-" +
         std::to_string(body.size() - 1) + "/" + std::to_string(body.size()) +
         "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}
// The file as segmentedFetch wrote it, asked from its start with no size.
struct Fetched {
  foil::net::SegmentedResult result;
  std::string data;
};
Fetched fetchFrom(int port) {
  Fetched out;
  foil::net::SegmentedOptions options;
  options.connections = 1;
  const auto start = Clock::now();
  options.cancelled = [start] { return Clock::now() - start > std::chrono::seconds(10); };
  out.result = foil::net::segmentedFetch(
      "http://127.0.0.1:" + std::to_string(port) + "/file", [](uint64_t) { return true; },
      [&out](uint64_t offset, const char *data, size_t size) {
        if (out.data.size() < offset + size) out.data.resize(offset + size);
        out.data.replace(size_t(offset), size, data, size);
        return true;
      },
      options);
  return out;
}
} // namespace
int runSegmentedTests() {
  int failures = 0;
  using namespace foil::net;
  {
    // Four reading, a 429: down to two at once, not when the parts end.
    SlowFiles files(ParallelFloor + (16ull << 20));
    SegmentedOptions options;
    options.connections = 4;
    int most = 0;
    std::atomic<bool> seen{false};
    options.cancelled = [&] {
      if (!files.limited) return false;
      const auto limited = files.limitedAt.load();
      const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - files.started).count();
      if (now - limited < 1500) return false; // the shed ones are gone by then
      most = std::max(most, files.open.load());
      if (now - limited < 2500) return false;
      seen = true;
      return true;
    };
    const auto result = segmentedFetch(
        "http://127.0.0.1:" + std::to_string(files.port) + "/big.nsp", [](uint64_t) { return true; },
        [](uint64_t, const char *, size_t) { return true; }, options);
    CHECK(seen && result.cancelled);
    if (!seen) std::printf("  ended: %s, status %ld, %d asked\n", result.error.c_str(), result.status, files.asked.load());
    CHECK(most > 0 && most <= 2);
    if (most > 2) std::printf("  %d connections open after a 429 halved four\n", most);
  }
  {
    // A small file that comes whole on the first reply is written, not lost
    // while the parts are planned.
    const std::string body(3000, 'g');
    Scripted server({whole(body)});
    const auto got = fetchFrom(server.port);
    CHECK(got.result.ok && got.data == body);
    CHECK(server.asked == 1);
  }
  for (const char *busy : {"429 Too Many", "503 Unavailable", "524 Timeout"}) {
    // The first request turned away before it told the size: asked again
    // after Retry-After, not left waiting.
    const std::string body(3000, 'g');
    Scripted server({std::string("HTTP/1.1 ") + busy + "\r\nRetry-After: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                     whole(body)});
    const auto got = fetchFrom(server.port);
    CHECK(got.result.ok && !got.result.cancelled && got.data == body);
    CHECK(server.asked == 2);
    if (!got.result.ok) std::printf("  first %s: %s, %d asked\n", busy, got.result.error.c_str(), server.asked.load());
  }
  return failures;
}
