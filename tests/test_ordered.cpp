#include "Test.hpp"
#include "MockServer.hpp"
#include "net/Ordered.hpp"
#include "net/Direct.hpp"
#include "net/Socket.hpp"
#include <atomic>
#include <string>
// An NCZ's bytes come down on several connections and reach the
// decompressor in order, whole, whatever order the pieces finish in; a busy
// server is asked again, a missing file is not.
namespace {
std::string file(size_t size) {
  std::string bytes(size, '\0');
  for (size_t i = 0; i < size; ++i) bytes[i] = char((i * 131 + i / 977) & 0xff);
  return bytes;
}
// The range a request asks for, as [from, end).
bool rangeOf(const std::string &request, size_t &from, size_t &end) {
  const auto at = request.find("Range: bytes=");
  if (at == std::string::npos) return false;
  const auto dash = request.find('-', at);
  from = std::stoul(request.substr(at + 13, dash - at - 13));
  end = std::stoul(request.substr(dash + 1)) + 1;
  return true;
}
Reply part(const std::string &bytes, const std::string &request) {
  size_t from = 0, end = 0;
  if (!rangeOf(request, from, end) || end > bytes.size()) return {416, "", ""};
  return {206, "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(end - 1) + "/" +
                   std::to_string(bytes.size()) + "\r\n",
          bytes.substr(from, end - from)};
}
} // namespace
int runOrderedTests() {
  int failures = 0;
  using namespace foil::net;
  forgetDirect();
  forgetLinks();
  const auto bytes = file(3u << 20);
  {
    // Several connections: every byte, in order, each piece asked once.
    Mock server;
    server.answer = [&](const std::string &request) { return part(bytes, request); };
    OrderedOptions options;
    options.connections = 3;
    options.chunk = 256u << 10;
    options.ahead = 2;
    std::string got;
    const auto url = "http://127.0.0.1:" + std::to_string(server.port) + "/game.nsz";
    CHECK(orderedStream(url, 4096, bytes.size() - 4096, [&](const uint8_t *data, size_t size) {
      got.append(reinterpret_cast<const char *>(data), size);
      return size;
    }, options));
    CHECK(got == bytes.substr(4096));
    CHECK(server.take().size() == (bytes.size() - 4096 + options.chunk - 1) / options.chunk);
  }
  {
    // A busy reply (503) is not the piece's bytes: asked again after its
    // Retry-After, and the stream is still whole.
    Mock server;
    std::atomic<int> asked{0};
    server.answer = [&](const std::string &request) -> Reply {
      if (++asked == 2) return {503, "Retry-After: 1\r\n", "busy"};
      return part(bytes, request);
    };
    OrderedOptions options;
    options.connections = 2;
    options.chunk = 512u << 10;
    std::string got;
    const auto url = "http://127.0.0.1:" + std::to_string(server.port) + "/game.nsz";
    CHECK(orderedStream(url, 0, bytes.size(), [&](const uint8_t *data, size_t size) {
      got.append(reinterpret_cast<const char *>(data), size);
      return size;
    }, options));
    CHECK(got == bytes);
  }
  {
    // One connection, a header read: exactly the range asked for.
    Mock server;
    server.answer = [&](const std::string &request) { return part(bytes, request); };
    std::string header(1000, '\0');
    const auto url = "http://127.0.0.1:" + std::to_string(server.port) + "/game.nsz";
    CHECK(orderedRead(url, 70000, header.data(), header.size(), {}));
    CHECK(header == bytes.substr(70000, 1000));
    const auto sent = server.take();
    CHECK(sent.size() == 1 && sent[0].find("Range: bytes=70000-70999") != std::string::npos);
  }
  {
    // A file the server does not have stops at once, on one connection or several.
    Mock server;
    server.answer = [](const std::string &) { return Reply{404, "", "no"}; };
    const auto url = "http://127.0.0.1:" + std::to_string(server.port) + "/gone.nsz";
    OrderedOptions options;
    options.connections = 2;
    options.chunk = 256u << 10;
    const auto ignore = [](const uint8_t *, size_t size) { return size; };
    CHECK(!orderedStream(url, 0, bytes.size(), ignore, options));
    char one[10];
    CHECK(!orderedRead(url, 0, one, sizeof one, {}));
  }
  {
    // The decompressor refusing its bytes ends the download, not a retry.
    Mock server;
    server.answer = [&](const std::string &request) { return part(bytes, request); };
    const auto url = "http://127.0.0.1:" + std::to_string(server.port) + "/game.nsz";
    CHECK(!orderedStream(url, 0, 1000, [](const uint8_t *, size_t) -> size_t { return 0; }, {}));
    CHECK(server.take().size() == 1);
    // Cancelled before it starts: nothing is asked for.
    OrderedOptions cancelled;
    cancelled.cancelled = [] { return true; };
    CHECK(!orderedStream(url, 0, 1000, [](const uint8_t *, size_t size) { return size; }, cancelled));
    CHECK(server.take().empty());
  }
  return failures;
}
