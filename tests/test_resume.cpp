#include "Test.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include "core/Resume.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include "net/HomebrewApi.hpp"
#include "core/Homebrew.hpp"
#include "core/Sha256.hpp"
#include "platform/Storage.hpp"
#include "install/Installer.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
namespace fs = std::filesystem;
namespace {
constexpr const char *Tid = "0100F2C0115B6000";
constexpr uint64_t FiveGiB = 5ull << 30;
std::string inGame(const std::string &name) { return std::string("atmosphere/contents/") + Tid + "/romfs/" + name; }

// A ustar header; `size` goes into the size field as octal, or base-256 when
// `base256` is set.
std::string header(const std::string &path, uint64_t size, char type = '0', bool base256 = false) {
  std::string b(512, '\0');
  std::memcpy(&b[0], path.data(), std::min<size_t>(path.size(), 100));
  std::snprintf(&b[100], 8, "%07o", 0644);
  if (base256) {
    b[124] = char(0x80);
    for (int i = 11; i >= 1; --i, size >>= 8)
      b[124 + i] = char(size & 0xFF);
  } else {
    std::snprintf(&b[124], 12, "%011llo", static_cast<unsigned long long>(size & 077777777777ull));
  }
  b[156] = type;
  std::memcpy(&b[257], "ustar\0" "00", 8);
  std::memset(&b[148], ' ', 8);
  unsigned sum = 0;
  for (unsigned char c : b)
    sum += c;
  std::snprintf(&b[148], 8, "%06o", sum);
  return b;
}
std::string padded(const std::string &body) { return body + std::string((512 - body.size() % 512) % 512, '\0'); }
std::string paxRecord(const std::string &key, const std::string &value) {
  const auto record = " " + key + "=" + value + "\n";
  size_t length = record.size() + 1;
  while (std::to_string(length).size() + record.size() != length)
    ++length;
  return std::to_string(length) + record;
}
std::string body(size_t size, char seed) {
  std::string out(size, '\0');
  for (size_t i = 0; i < size; ++i)
    out[i] = char(seed + i * 7 % 251);
  return out;
}
struct File {
  std::string path, body;
};
std::string archive(const std::vector<File> &files) {
  std::string out;
  for (const auto &f : files)
    out += header(f.path, f.body.size()) + padded(f.body);
  return out + std::string(1024, '\0');
}
bool feedAll(foil::TarWriter &writer, const std::string &data, size_t from, size_t to) {
  for (size_t at = from; at < to; at += 333)
    if (!writer.feed(data.data() + at, std::min<size_t>(333, to - at)))
      return false;
  return true;
}

// A one-file HTTP server for the end-to-end tests: honours Range and
// If-Range against a fixed ETag, and cuts a reply short when `drop` says so.
struct Mock {
  std::string body, etag = "\"abc123\"";
  bool ranges = true;                          // false: Range is ignored (200)
  std::function<size_t(int index)> drop;       // bytes to send before closing; npos: all
  std::vector<std::string> requests;
  // When set, a body for each path (by the request line); any other path is 404.
  std::map<std::string, std::string> paths;
  int listener = -1;
  std::string url;
  std::thread thread;
  void start(int connections) {
    listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
    listen(listener, 5);
    socklen_t length = sizeof(address);
    getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
    url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
    thread = std::thread([this, connections] {
      for (int i = 0; i < connections; ++i) {
        pollfd ready{listener, POLLIN, 0};
        if (::poll(&ready, 1, 10000) <= 0)
          return;
        const int client = accept(listener, nullptr, nullptr);
        if (client < 0)
          return;
        char buffer[4096];
        std::string request;
        while (request.find("\r\n\r\n") == std::string::npos) {
          const auto size = recv(client, buffer, sizeof(buffer), 0);
          if (size <= 0)
            break;
          request.append(buffer, size);
        }
        requests.push_back(request);
        std::string served = body;
        if (!paths.empty()) {
          const auto start = request.find(' ') + 1;
          const auto path = request.substr(start, request.find(' ', start) - start);
          const auto found = paths.find(path);
          if (found == paths.end()) {
            const std::string missing = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            send(client, missing.data(), missing.size(), MSG_NOSIGNAL);
            close(client);
            continue;
          }
          served = found->second;
        }
        const std::string &body = served;
        uint64_t from = 0;
        const auto range = request.find("Range: bytes=");
        const auto ifRange = request.find("If-Range: ");
        const bool validatorOk =
            ifRange == std::string::npos || request.compare(ifRange + 10, etag.size(), etag) == 0;
        if (ranges && range != std::string::npos && validatorOk)
          from = std::strtoull(request.c_str() + range + 13, nullptr, 10);
        std::string reply;
        if (from >= body.size() && from) {
          reply = "HTTP/1.1 416 Range Not Satisfiable\r\nConnection: close\r\nContent-Range: bytes */" +
                  std::to_string(body.size()) + "\r\nContent-Length: 0\r\n\r\n";
        } else {
          const auto rest = body.substr(from);
          reply = std::string(from ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n") +
                  "Connection: close\r\nETag: " + etag + "\r\nAccept-Ranges: bytes\r\n" +
                  (from ? "Content-Range: bytes " + std::to_string(from) + "-" + std::to_string(body.size() - 1) + "/" +
                              std::to_string(body.size()) + "\r\n"
                        : "") +
                  "Content-Length: " + std::to_string(rest.size()) + "\r\n\r\n";
          const auto cut = drop ? drop(i) : std::string::npos;
          reply += rest.substr(0, std::min(cut, rest.size()));
        }
        send(client, reply.data(), reply.size(), MSG_NOSIGNAL);
        close(client);
      }
    });
  }
  ~Mock() {
    if (thread.joinable())
      thread.join();
    if (listener >= 0)
      close(listener);
  }
};
std::string rangeOf(const std::string &request) {
  const auto at = request.find("Range: bytes=");
  return at == std::string::npos ? "" : request.substr(at + 13, request.find("\r\n", at) - at - 13);
}
} // namespace

int runResumeTests() {
  int failures = 0;
  using foil::RangeReply;
  {
    // What a resumed request asks for, past 4 GiB too.
    CHECK(foil::rangeHeaders(0, "\"h\"").empty());
    const auto headers = foil::rangeHeaders(FiveGiB, "\"abc123\"");
    CHECK(headers.size() == 2 && headers[0] == "Range: bytes=5368709120-" && headers[1] == "If-Range: \"abc123\"");
    CHECK(foil::rangeHeaders(100, "W/\"weak\"").size() == 1);
    CHECK(foil::rangeHeaders(100, "").size() == 1);
  }
  {
    // What each reply means.
    CHECK(foil::judgeRange(100, 206, "bytes 100-199/200") == RangeReply::Continue);
    CHECK(foil::judgeRange(100, 206, "bytes 0-199/200") == RangeReply::Reject);
    CHECK(foil::judgeRange(100, 206, "") == RangeReply::Reject);
    CHECK(foil::judgeRange(100, 200, "") == RangeReply::Restart);
    CHECK(foil::judgeRange(100, 416, "bytes */50") == RangeReply::Reject);
    CHECK(foil::judgeRange(0, 200, "") == RangeReply::Continue);
    CHECK(foil::judgeRange(0, 416, "") == RangeReply::Fail);
    CHECK(foil::judgeRange(100, 404, "") == RangeReply::Fail);
    CHECK(foil::judgeRange(FiveGiB, 206, "bytes 5368709120-6442450943/6442450944") == RangeReply::Continue);
    const auto range = foil::parseContentRange("bytes 5368709120-6442450943/*");
    CHECK(range && range->first == FiveGiB && range->total == 0);
    CHECK(!foil::parseContentRange("bytes 9-1/20") && !foil::parseContentRange("items 0-1/2"));
    CHECK(foil::declaredSize(206, "bytes 5368709120-6442450943/6442450944", 1) == 6442450944ull);
    CHECK(foil::declaredSize(200, "", 1234) == 1234);
  }
  {
    // The sidecar: a round trip with 64-bit numbers, and when a part resumes.
    foil::PartInfo wanted{"https://h/a.nro", "sha", "", 6ull << 30, 0};
    auto kept = wanted;
    kept.etag = "\"sha\"";
    kept.offset = FiveGiB;
    const auto back = foil::parsePartInfo(foil::serializePartInfo(kept));
    CHECK(back.url == kept.url && back.tag == "sha" && back.etag == "\"sha\"" && back.size == (6ull << 30) &&
          back.offset == FiveGiB);
    CHECK(foil::parsePartInfo("url=x\nsize=12a\n").url.empty());
    CHECK(foil::partResumeAt(kept, wanted, FiveGiB) == FiveGiB);
    auto changed = wanted;
    changed.tag = "other";
    CHECK(foil::partResumeAt(kept, changed, FiveGiB) == 0);
    CHECK(foil::partResumeAt(kept, wanted, (6ull << 30) + 1) == 0);
    CHECK(foil::partResumeAt(foil::PartInfo{}, wanted, 10) == 0);
    // Nothing to tell a changed file by: never resumed.
    foil::PartInfo bare{"https://h/a.nro", "", "", 0, 0};
    CHECK(foil::partResumeAt(bare, bare, 10) == 0);
    auto validated = bare;
    validated.etag = "\"e\"";
    CHECK(foil::partResumeAt(validated, bare, 10) == 10);
  }
  {
    // 4 GiB and up is made as a split file; the decision, at its edge.
    CHECK(!foil::platform::needsBigFile(0xFFFFFFFFull));
    CHECK(foil::platform::needsBigFile(1ull << 32));
    CHECK(foil::platform::needsBigFile(49ull << 30));
  }
  const auto root = fs::temp_directory_path() / ("cf-resume-" + std::to_string(getpid()));
  fs::remove_all(root);
  {
    // Sizes: base-256 and a PAX record, both read for the entry that follows.
    char field[12] = {};
    field[0] = char(0x80);
    const uint64_t big = 10ull << 30;
    for (int i = 11; i >= 1; --i)
      field[i] = char((big >> (8 * (11 - i))) & 0xFF);
    CHECK(foil::tarSize(field) == big);
    field[0] = char(0xFF);
    CHECK(foil::tarSize(field) == UINT64_MAX);
    const auto dir = (root / "sizes").string();
    const auto six = body(600, 'a');
    const auto data = header(inGame("b256.bin"), 600, '0', true) + padded(six) +
                      header("pax", paxRecord("size", "600").size(), 'x') + padded(paxRecord("size", "600")) +
                      header(inGame("pax.bin"), 0) + padded(six) + header(inGame("after.bin"), 3) + padded("end") +
                      std::string(1024, '\0');
    foil::TarWriter writer(dir, Tid);
    CHECK(feedAll(writer, data, 0, data.size()) && writer.finish());
    CHECK(foil::readFile(dir + "/" + inGame("b256.bin")) == six);
    CHECK(foil::readFile(dir + "/" + inGame("pax.bin")) == six);
    CHECK(foil::readFile(dir + "/" + inGame("after.bin")) == "end");
    // Past 8 GiB from a PAX record: parsed only, no payload written.
    const auto huge = paxRecord("size", std::to_string(big));
    const auto start = header("pax", huge.size(), 'x') + padded(huge) + header(inGame("huge.bin"), 0);
    foil::TarWriter giant(dir, Tid);
    CHECK(giant.feed(start.data(), start.size()) && giant.entryLeft() == big && giant.boundary() == 0);
    giant.abandon();
    CHECK(!fs::exists(dir + "/" + inGame("huge.bin")));
  }
  {
    // Cut mid-entry, resumed at the recorded boundary: the same files.
    const std::vector<File> files{{inGame("one.bin"), body(700, 'x')},
                                  {inGame("two.bin"), body(1500, 'y')},
                                  {inGame("three.bin"), body(300, 'z')}};
    const auto data = archive(files);
    const auto whole = (root / "whole").string(), cut = (root / "cut").string();
    foil::TarWriter all(whole, Tid);
    CHECK(feedAll(all, data, 0, data.size()) && all.finish());
    const size_t firstEnd = 512 + 1024, cutAt = firstEnd + 512 + 800;
    foil::PartInfo saved;
    {
      foil::TarWriter first(cut, Tid);
      CHECK(feedAll(first, data, 0, cutAt));
      CHECK(first.boundary() == firstEnd);
      saved.offset = first.boundary();
      first.abandon();
      CHECK(!fs::exists(cut + "/" + inGame("two.bin")));
    }
    const auto record = foil::parsePartInfo(foil::serializePartInfo(saved));
    foil::TarWriter second(cut, Tid);
    second.startAt(record.offset);
    CHECK(feedAll(second, data, size_t(record.offset), data.size()) && second.finish());
    for (const auto &f : files)
      CHECK(foil::readFile(cut + "/" + f.path) == foil::readFile(whole + "/" + f.path));
  }
  {
    // End to end: a file cut off mid-transfer continues with a Range ask.
    Mock server;
    server.body = body(300000, 'f');
    server.drop = [](int index) { return index == 0 ? size_t(100000) : std::string::npos; };
    server.start(2);
    const auto part = (root / "file.part").string();
    std::atomic<uint64_t> received{0};
    foil::PartInfo wanted{server.url + "/f", "", "", server.body.size(), 0};
    const auto got = foil::net::fetchPart(server.url + "/f", {}, part, wanted, true, received, nullptr);
    server.thread.join();
    CHECK(foil::net::rangedOk(got.response) && got.size == server.body.size());
    CHECK(foil::readFile(part) == server.body && received == server.body.size());
    CHECK(server.requests.size() == 2 && rangeOf(server.requests[1]) == "100000-");
    CHECK(server.requests.size() == 2 && server.requests[1].find("If-Range: \"abc123\"") != std::string::npos);
    foil::net::dropPart(part);
  }
  {
    // A part kept from an earlier launch: only the rest is asked for, and
    // progress starts at what is already there.
    Mock server;
    server.body = body(50000, 'g');
    server.start(1);
    const auto part = (root / "kept.part").string();
    std::string ignored;
    foil::atomicWrite(part, server.body.substr(0, 20000), ignored);
    foil::PartInfo wanted{server.url + "/g", "", "", server.body.size(), 0};
    auto kept = wanted;
    kept.etag = server.etag;
    foil::atomicWrite(foil::partInfoPath(part), foil::serializePartInfo(kept), ignored);
    std::atomic<uint64_t> received{0};
    const auto got = foil::net::fetchPart(server.url + "/g", {}, part, wanted, false, received, nullptr);
    server.thread.join();
    CHECK(foil::net::rangedOk(got.response) && foil::readFile(part) == server.body);
    CHECK(server.requests.size() == 1 && rangeOf(server.requests[0]) == "20000-");
    CHECK(received == server.body.size());
    // The server's file changed (another tag): the part is dropped, all asked again.
    Mock fresh;
    fresh.body = body(40000, 'h');
    fresh.start(1);
    foil::atomicWrite(part, "stale", ignored);
    kept.url = wanted.url = fresh.url + "/g";
    kept.tag = "old";
    wanted.size = kept.size = fresh.body.size();
    foil::atomicWrite(foil::partInfoPath(part), foil::serializePartInfo(kept), ignored);
    const auto again = foil::net::fetchPart(fresh.url + "/g", {}, part, wanted, false, received, nullptr);
    fresh.thread.join();
    CHECK(foil::net::rangedOk(again.response) && foil::readFile(part) == fresh.body);
    CHECK(fresh.requests.size() == 1 && rangeOf(fresh.requests[0]).empty());
    foil::net::dropPart(part);
  }
  {
    // A server that ignores Range sends 200: the part restarts cleanly.
    Mock server;
    server.body = body(30000, 'i');
    server.ranges = false;
    server.drop = [](int index) { return index == 0 ? size_t(10000) : std::string::npos; };
    server.start(2);
    const auto part = (root / "norange.part").string();
    std::atomic<uint64_t> received{0};
    foil::PartInfo wanted{server.url + "/i", "", "", server.body.size(), 0};
    const auto got = foil::net::fetchPart(server.url + "/i", {}, part, wanted, true, received, nullptr);
    server.thread.join();
    CHECK(foil::net::rangedOk(got.response) && foil::readFile(part) == server.body);
    CHECK(received == server.body.size());
    foil::net::dropPart(part);
  }
  {
    // A patch: dropped mid-entry, continued from the entry boundary with the
    // hash as If-Range; then a second launch picks up a saved boundary.
    const std::vector<File> files{{inGame("a.bin"), body(5000, 'a')},
                                  {inGame("b.bin"), body(9000, 'b')},
                                  {inGame("c.bin"), body(700, 'c')}};
    const auto data = archive(files);
    const size_t firstEnd = 512 + 5120;
    Mock server;
    server.body = data;
    server.drop = [&](int index) { return index == 0 ? firstEnd + 512 + 3000 : std::string::npos; };
    server.start(2);
    foil::Server shop;
    shop.url = server.url;
    foil::net::Session session(shop, foil::State{}, (root / "state.json").string());
    foil::UserPatch patch;
    patch.id = "p1";
    patch.titleId = Tid;
    patch.hash = "abc123";
    patch.size = data.size();
    const auto dir = (root / "p1.part").string();
    std::atomic<uint64_t> received{0};
    foil::Error error;
    CHECK(session.downloadPatch(patch, dir, received, error));
    server.thread.join();
    CHECK(server.requests.size() == 2 && rangeOf(server.requests[1]) == std::to_string(firstEnd) + "-");
    for (const auto &f : files)
      CHECK(foil::readFile(dir + "/" + f.path) == f.body);
    CHECK(!fs::exists(dir + "/" + foil::ResumeFile) && received == data.size());

    Mock later;
    later.body = data;
    later.start(1);
    shop.url = later.url;
    foil::net::Session next(shop, foil::State{}, (root / "state.json").string());
    const auto second = (root / "p2.part").string();
    std::string ignored;
    foil::atomicWrite(second + "/" + files[0].path, files[0].body, ignored);
    foil::atomicWrite(second + "/" + files[1].path, "half written", ignored);
    foil::PartInfo saved{later.url + "/api/patches/p1/download", "abc123", later.etag, data.size(), firstEnd};
    foil::atomicWrite(second + "/" + foil::ResumeFile, foil::serializePartInfo(saved), ignored);
    received = 0;
    CHECK(next.downloadPatch(patch, second, received, error));
    later.thread.join();
    CHECK(later.requests.size() == 1 && rangeOf(later.requests[0]) == std::to_string(firstEnd) + "-");
    for (const auto &f : files)
      CHECK(foil::readFile(second + "/" + f.path) == f.body);
    CHECK(received == data.size());
  }
  {
    // A folder homebrew app as one tar (bundle_url): cut after its first
    // file, the next request asks from that file's end, and every file lands
    // where the list puts it ('+', spaces and Hangul in names too).
    const auto sd = fs::temp_directory_path() / ("cf-bundle-" + std::to_string(getpid()));
    fs::remove_all(sd);
    fs::create_directories(sd);
    const std::vector<File> files{{"switch/pk/pk.nro", body(70000, 'a')},
                                  {"switch/pk/Graphics/Scratch + Shadow Claw.png", body(5000, 'b')},
                                  {"switch/pk/data/설치-스위치.md", body(123, 'c')}};
    Mock mock;
    // As the server sends it: the folders first, then the files in list order.
    const std::string folders = header("switch/pk/", 0, '5') + header("switch/pk/Graphics/", 0, '5') +
                                header("switch/pk/data/", 0, '5');
    mock.body = folders + archive(files);
    mock.drop = [](int i) { return i == 0 ? size_t(1536 + 72000) : std::string::npos; }; // inside the second file
    mock.start(2);
    foil::homebrew::App app;
    app.id = "pk";
    app.bundleUrl = mock.url + "/api/homebrew/pk/bundle";
    for (const auto &f : files) {
      foil::homebrew::File file;
      file.path = f.path;
      file.size = f.body.size();
      file.sha256 = std::string(64, 'a');
      app.files.push_back(file);
    }
    foil::Server server;
    server.url = mock.url;
    foil::State state;
    std::atomic<uint64_t> received{0};
    foil::Error error;
    CHECK(foil::net::homebrewDownload(server, state, app, sd.string(), received, error, nullptr));
    for (const auto &f : files)
      CHECK(foil::readFile((sd / f.path).string()) == f.body);
    CHECK(mock.requests.size() == 2 && rangeOf(mock.requests[1]) == std::to_string(1536 + 512 + 70144) + "-");
    CHECK(!fs::exists(sd / "switch/cookingfoil/resume/homebrew-pk.json"));
    // The progress total is the bundle's size, give or take a folder header
    // (which parent folders a server lists is its own choice).
    const auto estimate = foil::homebrew::bundleSize(app);
    CHECK(estimate + 4096 >= mock.body.size() && estimate <= mock.body.size() + 4096);
    fs::remove_all(sd);
  }
  {
    // A server whose bundle address is gone (404): the files come one by one.
    const auto sd = fs::temp_directory_path() / ("cf-bundle-gone-" + std::to_string(getpid()));
    fs::remove_all(sd);
    fs::create_directories(sd);
    Mock mock;
    const std::string one = body(3000, 'q'), two = body(10, 'r');
    mock.paths = {{"/f/1", one}, {"/f/2", two}};
    mock.start(3);
    foil::homebrew::App app;
    app.id = "gone";
    app.bundleUrl = mock.url + "/bundle";
    const auto hash = [](const std::string &bytes) {
      foil::Sha256 h;
      h.update(bytes.data(), bytes.size());
      return h.hex();
    };
    app.files.push_back({"switch/gone/one.nro", hash(one), mock.url + "/f/1", one.size()});
    app.files.push_back({"switch/gone/설치-스위치.md", hash(two), mock.url + "/f/2", two.size()});
    foil::Server server;
    server.url = mock.url;
    std::atomic<uint64_t> received{0};
    foil::Error error;
    CHECK(foil::net::homebrewDownload(server, foil::State{}, app, sd.string(), received, error, nullptr));
    CHECK(foil::readFile((sd / "switch/gone/one.nro").string()) == one);
    CHECK(foil::readFile((sd / "switch/gone/설치-스위치.md").string()) == two);
    fs::remove_all(sd);
  }
  {
    // A bundle holding a path the list does not name is refused, unwritten.
    const auto sd = fs::temp_directory_path() / ("cf-bundle-bad-" + std::to_string(getpid()));
    fs::remove_all(sd);
    fs::create_directories(sd);
    Mock mock;
    mock.body = archive({{"switch/pk/pk.nro", body(100, 'a')}, {"switch/other/evil.nro", body(100, 'e')}});
    mock.start(1);
    foil::homebrew::App app;
    app.id = "pk";
    app.bundleUrl = mock.url + "/bundle";
    foil::homebrew::File file;
    file.path = "switch/pk/pk.nro";
    file.size = 100;
    file.sha256 = std::string(64, 'a');
    app.files.push_back(file);
    foil::Server server;
    server.url = mock.url;
    std::atomic<uint64_t> received{0};
    foil::Error error;
    CHECK(!foil::net::homebrewDownload(server, foil::State{}, app, sd.string(), received, error, nullptr) && error);
    CHECK(!fs::exists(sd / "switch/other/evil.nro"));
    fs::remove_all(sd);
  }
  fs::remove_all(root);
  {
    // The preview downloads a game for real on the console's engine and
    // throws the bytes away; a server that refuses it fails the install.
    Mock server;
    server.body = std::string(3u << 20, 'g');
    server.start(1);
    foil::Item item;
    item.url = server.url + "/game.nsp";
    item.name = "Game";
    bool threw = false;
    try {
      foil::install::nativeInstall(item, "sd", {});
    } catch (...) {
      threw = true;
    }
    CHECK(!threw);
    CHECK(server.requests.size() == 1);
  }
  {
    Mock server;
    server.body = "x";
    server.paths["/other"] = "x";
    server.start(1);
    foil::Item item;
    item.url = server.url + "/game.nsp";
    bool threw = false;
    try {
      foil::install::nativeInstall(item, "sd", {});
    } catch (...) {
      threw = true;
    }
    CHECK(threw);
  }
  return failures;
}
