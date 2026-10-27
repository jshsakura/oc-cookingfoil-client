#include "Test.hpp"
#include "core/ClientUpdate.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include "core/Resume.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include "net/Socket.hpp"
#include "net/HomebrewApi.hpp"
#include "core/Homebrew.hpp"
#include "core/Sha256.hpp"
#include "platform/Storage.hpp"
#include "install/Installer.hpp"
#include "install/Installer.hpp"
#include "core/ZipStream.hpp"
#include <zlib.h>
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>
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
  // When set and not empty for a connection, the whole reply sent instead.
  std::function<std::string(int index)> answer;
  int idleMs = 10000; // how long it waits for the next connection before it stops
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
        if (::poll(&ready, 1, idleMs) <= 0)
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
        if (const auto said = answer ? answer(i) : std::string(); !said.empty()) {
          send(client, said.data(), said.size(), MSG_NOSIGNAL);
          close(client);
          continue;
        }
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
// A zip as tools write it: each entry stored or deflated, some with a data
// descriptor after their bytes, then the central directory and end record.
struct ZipFile {
  std::string name, data;
  bool deflate = false, descriptor = false;
};
std::string le(uint64_t value, int bytes) {
  std::string out;
  for (int i = 0; i < bytes; ++i)
    out += char((value >> (8 * i)) & 0xFF);
  return out;
}
std::string deflated(const std::string &data) {
  z_stream z{};
  deflateInit2(&z, 6, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::string out(compressBound(uLong(data.size())) + 64, '\0');
  z.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.data()));
  z.avail_in = uInt(data.size());
  z.next_out = reinterpret_cast<Bytef *>(&out[0]);
  z.avail_out = uInt(out.size());
  deflate(&z, Z_FINISH);
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}
std::string zipOf(const std::vector<ZipFile> &files, std::vector<uint64_t> *dataEnds = nullptr) {
  std::string zip, directory;
  for (const auto &f : files) {
    const auto packed = f.deflate ? deflated(f.data) : f.data;
    const uint32_t crc = uint32_t(crc32(0, reinterpret_cast<const Bytef *>(f.data.data()), uInt(f.data.size())));
    const int flags = f.descriptor ? 8 : 0, method = f.deflate ? 8 : 0;
    const uint64_t offset = zip.size();
    zip += le(0x04034b50, 4) + le(20, 2) + le(flags, 2) + le(method, 2) + le(0, 4) +
           le(f.descriptor ? 0 : crc, 4) + le(f.descriptor ? 0 : packed.size(), 4) +
           le(f.descriptor ? 0 : f.data.size(), 4) + le(f.name.size(), 2) + le(0, 2) + f.name + packed;
    if (dataEnds)
      dataEnds->push_back(zip.size());
    if (f.descriptor)
      zip += le(0x08074b50, 4) + le(crc, 4) + le(packed.size(), 4) + le(f.data.size(), 4);
    directory += le(0x02014b50, 4) + le(20, 2) + le(20, 2) + le(flags, 2) + le(method, 2) + le(0, 4) + le(crc, 4) +
                 le(packed.size(), 4) + le(f.data.size(), 4) + le(f.name.size(), 2) + le(0, 2) + le(0, 2) +
                 le(0, 2) + le(0, 2) + le(0, 4) + le(offset, 4) + f.name;
  }
  const uint64_t at = zip.size();
  return zip + directory + le(0x06054b50, 4) + le(0, 4) + le(files.size(), 2) + le(files.size(), 2) +
         le(directory.size(), 4) + le(at, 4) + le(0, 2);
}
std::string varied(size_t size, int seed) {
  std::string out(size, ' ');
  for (size_t i = 0; i < size; ++i)
    out[i] = char('a' + (i * 7 + i / 13 + size_t(seed)) % 26);
  return out;
}
std::string rangeOf(const std::string &request) {
  const auto at = request.find("Range: bytes=");
  return at == std::string::npos ? "" : request.substr(at + 13, request.find("\r\n", at) - at - 13);
}
} // namespace

namespace foil::install {
void setHostInstall(std::function<void(const Item &)> run);
}
namespace foil::platform {
void setHostSpace(uint64_t bytes);
}
int runLegacyPatchResumeTests() {
  int failures = 0;
  const auto root = fs::temp_directory_path() / ("cf-legacy-resume-" + std::to_string(getpid()));
  fs::remove_all(root);
  for (bool current : {false, true}) {
    Mock server;
    const std::vector<File> files{{inGame("a.bin"), "first"}, {inGame("b.bin"), "second"}};
    server.body = archive(files);
    server.start(1);
    foil::Server upstream;
    upstream.url = server.url;
    foil::net::Session session(upstream, foil::State{}, (root / "cookingfoil.db").string());
    foil::UserPatch patch;
    patch.id = current ? "both" : "old-only";
    patch.titleId = Tid;
    patch.hash = "legacy";
    patch.size = server.body.size();
    foil::PatchStore store((root / "sd").string(), (root / "store").string());
    const auto dir = store.staging(patch.id);
    std::string error;
    CHECK(foil::atomicWrite(dir + "/" + files[0].path, files[0].body, error));
    const foil::PartInfo saved{server.url + "/api/patches/" + patch.id + "/download",
                              patch.hash, server.etag, patch.size, 1024, ""};
    CHECK(foil::atomicWrite(dir + "/.resume.old", foil::serializePartInfo(saved), error));
    if (current) CHECK(foil::atomicWrite(dir + "/.resume", foil::serializePartInfo(saved), error));
    std::atomic<uint64_t> received{0};
    foil::Error problem;
    CHECK(session.downloadPatch(patch, dir, received, problem));
    server.thread.join();
    CHECK(server.requests.size() == 1 && rangeOf(server.requests[0]) == "1024-");
    CHECK(!fs::exists(dir + "/.resume.old") && !fs::exists(dir + "/.resume"));
    CHECK(store.commit(patch, problem));
    CHECK(foil::readFile((root / "store" / patch.id / files[1].path).string()) == files[1].body);
  }
  fs::remove_all(root);
  return failures;
}
int runResumeTests() {
  int failures = runLegacyPatchResumeTests();
  using foil::RangeReply;
  {
    // An update over files already on the card: a file is replaced only once
    // it is whole and checked. Cut short, or refused, the old one stays and
    // nothing is left beside it.
    const auto root = fs::temp_directory_path() / ("cf-replace-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "switch/app");
    const std::string path = "switch/app/app.nro", fresh = body(5000, 'n');
    const auto onCard = (root / path).string();
    const auto leftovers = [&] {
      int n = 0;
      for (const auto &entry : fs::directory_iterator(root / "switch/app"))
        n += entry.path().filename() != "app.nro";
      return n;
    };
    const auto allow = [](const std::string &) { return true; };
    const std::string data = archive({{path, fresh}});
    std::string ignored;
    CHECK(foil::atomicWrite(onCard, "old build", ignored));
    {
      foil::TarWriter cut(root.string(), allow);
      CHECK(feedAll(cut, data, 0, 512 + 2000));
      cut.abandon();
    }
    CHECK(foil::readFile(onCard) == "old build" && leftovers() == 0);
    {
      foil::TarWriter refused(root.string(), allow);
      refused.check([](const std::string &, uint64_t, const std::string &) { return false; });
      CHECK(!feedAll(refused, data, 0, data.size()));
    }
    CHECK(foil::readFile(onCard) == "old build" && leftovers() == 0);
    {
      foil::TarWriter whole(root.string(), allow);
      CHECK(feedAll(whole, data, 0, data.size()) && whole.finish());
    }
    CHECK(foil::readFile(onCard) == fresh && leftovers() == 0);
    // replaceFile: the new file in the old one's place, nothing beside it.
    CHECK(foil::atomicWrite(onCard + ".new", "next", ignored));
    CHECK(foil::replaceFile(onCard + ".new", onCard, ignored));
    CHECK(foil::readFile(onCard) == "next" && leftovers() == 0);
    CHECK(!foil::replaceFile(onCard + ".missing", onCard, ignored));
    CHECK(foil::readFile(onCard) == "next");
    const auto split = root / "split", incoming = root / "incoming";
    fs::create_directories(split);
    fs::create_directories(incoming);
    CHECK(foil::atomicWrite((split / "00").string(), "old", ignored));
    CHECK(foil::atomicWrite((incoming / "00").string(), "new", ignored));
    CHECK(foil::replaceFile(incoming.string(), split.string(), ignored));
    CHECK(foil::readFile((split / "00").string()) == "new");
    CHECK(!fs::exists(root / "split.old") && !fs::exists(incoming));
    CHECK(!foil::replaceFile((split / "00").string(), split.string(), ignored));
    CHECK(foil::readFile((split / "00").string()) == "new");
    CHECK(!foil::replaceFile(split.string(), split.string(), ignored));
    CHECK(foil::readFile((split / "00").string()) == "new");
    fs::remove_all(root);
  }
  {
    // Writers of one file line up: several threads saving the same card
    // file whole (a ledger, the records database) must never trample the
    // temp they share, so the file is always absent or one writer's whole
    // contents, and no temp is left behind.
    const auto root = fs::temp_directory_path() / ("cf-lineup-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const auto file = (root / "ledger.json").string();
    constexpr int writers = 4, rounds = 60, size = 8 * 1024 * 1024;
    std::atomic<bool> bad{false}, torn{false}, stop{false};
    std::vector<std::thread> threads;
    for (int w = 0; w < writers; ++w)
      threads.emplace_back([&, w] {
        for (int r = 0; r < rounds; ++r) {
          std::string error;
          if (!foil::atomicWrite(file, std::string(size, char('a' + (w * rounds + r) % 26)), error)) bad = true;
        }
      });
    std::thread reader([&] {
      while (!stop.load()) {
        const auto have = foil::readFile(file);
        if (have.size() != 0 && have.size() != size) torn = true;  // a shared temp, trampled
        else if (have.size() == size && have.find_first_not_of(have[0]) != std::string::npos) torn = true;
        std::this_thread::yield();
      }
    });
    for (auto &t : threads) t.join();
    stop = true;
    reader.join();
    CHECK(!bad);
    CHECK(!torn);
    const auto last = foil::readFile(file);
    bool whole = false;
    for (int i = 0; i < writers * rounds && !whole; ++i) whole = last == std::string(size, char('a' + i % 26));
    CHECK(whole);
    CHECK(!fs::exists(root / "ledger.json.tmp"));
    fs::remove_all(root);
  }
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
    foil::PartInfo wanted{"https://h/a.nro", "sha", "", 6ull << 30, 0, ""};
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
    foil::PartInfo bare{"https://h/a.nro", "", "", 0, 0, ""};
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
    // The download token runs out mid-file: a fresh one is asked for once and
    // the file goes on from the byte it stood at, carrying the new token.
    Mock server;
    server.body = body(300000, 't');
    server.drop = [](int index) { return index == 0 ? size_t(100000) : std::string::npos; };
    server.answer = [](int index) -> std::string {
      if (index == 1)
        return "HTTP/1.1 401 Unauthorized\r\nConnection: close\r\nX-CookingFoil-Token: expired\r\n"
               "Content-Length: 0\r\n\r\n";
      if (index == 2)
        return "HTTP/1.1 200 OK\r\nConnection: close\r\nX-CookingFoil-Download-Token: fresh\r\n"
               "Content-Length: 2\r\n\r\n{}";
      return "";
    };
    server.start(4);
    int asked = 0;
    foil::net::setTokenRenewer(server.url, [&] {
      ++asked;
      return foil::net::get(server.url + "/api/library/info").status;
    });
    const auto part = (root / "token.part").string();
    std::atomic<uint64_t> received{0};
    foil::PartInfo wanted{server.url + "/f", "", "", server.body.size(), 0, ""};
    const auto got = foil::net::fetchPart(server.url + "/f", {"Authorization: Bearer stale"}, part, wanted, true,
                                          received, nullptr);
    server.thread.join();
    foil::net::setTokenRenewer(server.url, {});
    CHECK(foil::net::rangedOk(got.response) && foil::readFile(part) == server.body);
    CHECK(asked == 1 && foil::net::downloadToken(server.url) == "fresh" && !foil::net::downloadTokenRejected(server.url));
    // A part refused with the old token after that finds the new one without
    // asking; refused with the new one, the server is not asked again so soon.
    CHECK(foil::net::renewDownloadToken(server.url, "stale") && asked == 1);
    CHECK(!foil::net::renewDownloadToken(server.url, "fresh") && asked == 1);
    CHECK(!foil::net::downloadSignInRefused(server.url));
    CHECK(server.requests.size() == 4 && rangeOf(server.requests[3]) == "100000-");
    CHECK(server.requests.size() == 4 && server.requests[3].find("Authorization: Bearer fresh") != std::string::npos);
    foil::net::dropPart(part);
    // The renewal itself refused (the login no longer works): said once, not
    // asked again and again.
    int refusals = 0;
    foil::net::setTokenRenewer(server.url, [&] {
      ++refusals;
      return 401L;
    });
    CHECK(!foil::net::renewDownloadToken(server.url, "fresh") && refusals == 1 && foil::net::downloadSignInRefused(server.url));
    CHECK(!foil::net::renewDownloadToken(server.url, "fresh") && refusals == 1);
    foil::net::setTokenRenewer(server.url, {});
    CHECK(foil::net::bearerOf({"Accept: */*", "Authorization: Bearer abc"}) == "abc");
    // A bearer line takes the token held now; other lines stay as they are.
    std::vector<std::string> headers{"Authorization: Bearer old", "Accept: */*"};
    foil::net::freshenBearer(server.url, headers);
    CHECK(headers[0] == "Authorization: Bearer fresh" && headers[1] == "Accept: */*");
  }
  {
    // A big file from its start comes over several connections, each asking
    // its own range, and lands whole with its hash.
    Mock server;
    server.body = body(6u << 20, 'p');
    server.idleMs = 1500;
    server.start(64);
    const auto part = (root / "parallel.part").string();
    std::atomic<uint64_t> received{0};
    foil::PartInfo wanted{server.url + "/f", "", "", server.body.size(), 0, ""};
    const auto got = foil::net::fetchPart(server.url + "/f", {}, part, wanted, true, received, nullptr);
    server.thread.join();
    CHECK(foil::net::rangedOk(got.response) && got.size == server.body.size());
    CHECK(foil::readFile(part) == server.body && received == server.body.size());
    foil::Sha256 sha;
    sha.update(server.body.data(), server.body.size());
    CHECK(got.sha256 == sha.hex());
    // The one-connection path asks from 0 with no Range; this one names its range.
    CHECK(!server.requests.empty() && server.requests[0].find("Range: bytes=0-") != std::string::npos);
    foil::net::dropPart(part);
  }
  {
    // A big file stopped part way (here cancelled after its first 2 MB) keeps
    // its parts: the next try asks only for the rest and lands it whole.
    Mock server;
    server.body = body(6u << 20, 'q');
    const uint64_t cut = 2u << 20;
    std::atomic<bool> cancel{false};
    server.answer = [&](int index) -> std::string {
      if (index == 0)
        return "HTTP/1.1 206 Partial Content\r\nConnection: close\r\nETag: " + server.etag +
               "\r\nContent-Range: bytes 0-" + std::to_string(server.body.size() - 1) + "/" +
               std::to_string(server.body.size()) + "\r\nContent-Length: " + std::to_string(server.body.size()) +
               "\r\n\r\n" + server.body.substr(0, cut);
      if (index == 1) {
        cancel = true; // asked for the rest: stopped there
        return "HTTP/1.1 503 Busy\r\nConnection: close\r\nRetry-After: 1\r\nContent-Length: 0\r\n\r\n";
      }
      return "";
    };
    server.idleMs = 20000;
    server.start(3);
    const auto part = (root / "parts.part").string();
    foil::PartInfo wanted{server.url + "/f", "", "", server.body.size(), 0, ""};
    std::atomic<uint64_t> received{0};
    const auto stopped = foil::net::fetchPart(server.url + "/f", {}, part, wanted, true, received, &cancel);
    CHECK(foil::net::keepPart(stopped) && stopped.keptPlan && received == 0);
    CHECK(fs::exists(foil::partPlanPath(part)) && !fs::exists(foil::partInfoPath(part)));
    cancel = false;
    const auto got = foil::net::fetchPart(server.url + "/f", {}, part, wanted, true, received, &cancel);
    server.thread.join();
    CHECK(foil::net::rangedOk(got.response) && foil::readFile(part) == server.body && received == server.body.size());
    foil::Sha256 sha;
    sha.update(server.body.data(), server.body.size());
    CHECK(got.sha256 == sha.hex() && !fs::exists(foil::partPlanPath(part)));
    CHECK(server.requests.size() == 3 && rangeOf(server.requests[2]) == std::to_string(cut) + "-" + std::to_string(server.body.size() - 1));
    CHECK(server.requests.size() == 3 && server.requests[2].find("If-Range: " + server.etag) != std::string::npos);
      foil::net::dropPart(part);
    }
    {
    // Big enough to come in two parts at once: the second lands out of
    // order, so the hash takes the first as it comes and reads back the rest.
    Mock server;
    server.body = body(80u << 20, 'r');
    for (size_t i = 0; i < server.body.size(); i += 4096)
      server.body[i] = char(i / 4096); // a part in the wrong place would show
    server.idleMs = 3000;
    server.start(64);
    const auto part = (root / "split.part").string();
    std::atomic<uint64_t> received{0};
    foil::PartInfo wanted{server.url + "/f", "", "", server.body.size(), 0};
    const auto got = foil::net::fetchPart(server.url + "/f", {}, part, wanted, true, received, nullptr);
    server.thread.join();
    foil::Sha256 sha;
    sha.update(server.body.data(), server.body.size());
    CHECK(foil::net::rangedOk(got.response) && got.sha256 == sha.hex());
    size_t asked = 0;
    for (const auto &request : server.requests)
      asked += rangeOf(request).rfind("0-", 0) != 0;
    CHECK(asked > 0); // a part that starts past 0 was asked for: it did come split
    foil::net::dropPart(part);
  }
  {
    // End to end: a file cut off mid-transfer continues with a Range ask.
    Mock server;
    server.body = body(300000, 'f');
    server.drop = [](int index) { return index == 0 ? size_t(100000) : std::string::npos; };
    server.start(2);
    const auto part = (root / "file.part").string();
    std::atomic<uint64_t> received{0};
    foil::PartInfo wanted{server.url + "/f", "", "", server.body.size(), 0, ""};
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
    foil::PartInfo wanted{server.url + "/g", "", "", server.body.size(), 0, ""};
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
    foil::PartInfo wanted{server.url + "/i", "", "", server.body.size(), 0, ""};
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
    foil::Server upstream;
    upstream.url = server.url;
    foil::net::Session session(upstream, foil::State{}, (root / "cookingfoil.db").string());
    foil::UserPatch patch;
    patch.id = "p1";
    patch.titleId = Tid;
    patch.hash = "abc123";
    patch.size = data.size();
    const auto dir = (root / "p1.part").string();
    // An interrupted attempt without a usable resume record leaves more
    // than .part files. Starting over must clear the entire staging tree.
    std::string stagingError;
    CHECK(foil::atomicWrite(dir + "/nested/unrelated.old", "stale", stagingError));
    CHECK(foil::atomicWrite(dir + "/.resume.old", "invalid", stagingError));
    std::atomic<uint64_t> received{0};
    foil::Error error;
    CHECK(session.downloadPatch(patch, dir, received, error));
    server.thread.join();
    CHECK(server.requests.size() == 2 && rangeOf(server.requests[1]) == std::to_string(firstEnd) + "-");
    for (const auto &f : files)
      CHECK(foil::readFile(dir + "/" + f.path) == f.body);
    CHECK(!fs::exists(dir + "/" + foil::ResumeFile) && received == data.size());
    CHECK(!fs::exists(dir + "/nested") && !fs::exists(dir + "/.resume.old"));

    Mock later;
    later.body = data;
    later.start(1);
    upstream.url = later.url;
    foil::net::Session next(upstream, foil::State{}, (root / "cookingfoil.db").string());
    const auto second = (root / "p2.part").string();
    std::string ignored;
    foil::atomicWrite(second + "/" + files[0].path, files[0].body, ignored);
    foil::atomicWrite(second + "/" + files[1].path, "half written", ignored);
    foil::PartInfo saved{later.url + "/api/patches/p1/download", "abc123", later.etag, data.size(), firstEnd, ""};
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
      file.sha256 = foil::update::sha256Hex(f.body);
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
  for (const bool missing : {false, true}) {
    // A bundle whose file is not the one the list names, or lacks it: the
    // file is taken by itself instead, and the card ends as the list says.
    const auto sd = fs::temp_directory_path() / ("cf-bundle-check-" + std::to_string(getpid()));
    fs::remove_all(sd);
    fs::create_directories(sd);
    const std::string good = body(3000, 'g');
    const auto path = std::string("switch/pk/data.bin");
    const std::vector<File> other{{path, body(3000, 'e')}};
    Mock mock;
    mock.paths["/api/homebrew/pk/bundle"] = missing ? std::string(1024, '\0') : archive(other);
    mock.paths["/api/homebrew/pk/data"] = good;
    mock.start(2);
    foil::homebrew::App app;
    app.id = "pk";
    app.bundleUrl = mock.url + "/api/homebrew/pk/bundle";
    app.files.push_back({path, foil::update::sha256Hex(good), mock.url + "/api/homebrew/pk/data", good.size()});
    foil::Server server;
    server.url = mock.url;
    std::atomic<uint64_t> received{0};
    foil::Error error;
    CHECK(foil::net::homebrewDownload(server, foil::State{}, app, sd.string(), received, error, nullptr));
    mock.thread.join();
    CHECK(mock.requests.size() == 2);
    CHECK(foil::readFile((sd / path).string()) == good);
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
    // Downloaded again after the list changed (a file dropped, others kept):
    // what the card already has as listed is not fetched again. With most
    // of it there, only the missing file comes, on its own; with most of it
    // missing, the bundle comes and the files already there are not rewritten.
    const auto hash = [](const std::string &bytes) {
      foil::Sha256 h;
      h.update(bytes.data(), bytes.size());
      return h.hex();
    };
    const auto sd = fs::temp_directory_path() / ("cf-bundle-kept-" + std::to_string(getpid()));
    fs::remove_all(sd);
    const std::string big = body(90000, 'k'), small = body(700, 'n');
    std::string ignored;
    fs::create_directories(sd / "switch/kept");
    foil::atomicWrite((sd / "switch/kept/kept.nro").string(), big, ignored);
    Mock mock;
    mock.paths = {{"/f/small", small}};
    mock.start(1);
    foil::homebrew::App app;
    app.id = "kept";
    app.bundleUrl = mock.url + "/bundle";
    app.files.push_back({"switch/kept/kept.nro", hash(big), mock.url + "/f/big", big.size()});
    app.files.push_back({"switch/kept/new.dat", hash(small), mock.url + "/f/small", small.size()});
    foil::Server server;
    server.url = mock.url;
    std::atomic<uint64_t> received{0}, kept{0};
    foil::Error error;
    CHECK(foil::net::homebrewDownload(server, foil::State{}, app, sd.string(), received, error, nullptr, &kept));
    mock.thread.join();
    CHECK(mock.requests.size() == 1 && mock.requests[0].find("/f/small") != std::string::npos);
    CHECK(foil::readFile((sd / "switch/kept/new.dat").string()) == small);
    CHECK(received == big.size() + small.size());
    // The file already there counts to the progress, and apart: a speed
    // leaves it out (it came off the card, not the network).
    CHECK(kept == big.size());
    // Most of it missing: the bundle, with the file already there left as it is.
    const auto before = fs::last_write_time(sd / "switch/kept/kept.nro");
    const std::vector<File> files{{"switch/kept/kept.nro", big}, {"switch/kept/more.dat", body(400000, 'm')}};
    Mock tar;
    tar.body = archive(files);
    tar.start(1);
    app.bundleUrl = tar.url + "/bundle";
    app.files = {{"switch/kept/kept.nro", hash(big), tar.url + "/f/big", big.size()},
                 {"switch/kept/more.dat", hash(files[1].body), tar.url + "/f/more", files[1].body.size()}};
    server.url = tar.url;
    CHECK(foil::net::homebrewDownload(server, foil::State{}, app, sd.string(), received, error, nullptr));
    tar.thread.join();
    CHECK(tar.requests.size() == 1 && tar.requests[0].find("/bundle") != std::string::npos);
    CHECK(foil::readFile((sd / "switch/kept/more.dat").string()) == files[1].body);
    CHECK(fs::last_write_time(sd / "switch/kept/kept.nro") == before);
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
  {
    // An app the server keeps as a zip: its directory from the last bytes,
    // then the zip in one stream; cut inside the second file, it goes on
    // from the first file's end. Only the listed entries land (the manual
    // stays out), deflated and stored, with or without a data descriptor.
    const auto sd = fs::temp_directory_path() / ("cf-zip-" + std::to_string(getpid()));
    fs::remove_all(sd);
    fs::create_directories(sd);
    const auto nro = varied(70000, 1), note = varied(5000, 2);
    std::vector<uint64_t> ends;
    Mock mock;
    std::vector<ZipFile> parts(3);
    parts[0] = {"zipapp/zipapp.nro", nro, true, true};
    parts[1] = {"zipapp/data/설치.md", note, false, true};
    parts[2] = {"README.txt", "manual", true, false};
    mock.body = zipOf(parts, &ends);
    mock.drop = [&](int i) { return i == 1 ? size_t(ends[0] + 100) : std::string::npos; };
    mock.start(3);
    foil::homebrew::App app;
    app.id = "zipapp";
    app.installPath = "switch/zipapp/zipapp.nro";
    app.downloadUrl = mock.url + "/api/homebrew/zipapp/download";
    app.archive.format = "zip";
    app.archive.url = mock.url + "/api/homebrew/zipapp/archive";
    app.archive.size = mock.body.size();
    app.archive.sha256 = std::string(64, 'b');
    app.archive.entries = {{"zipapp/zipapp.nro", "switch/zipapp/zipapp.nro", nro.size()},
                           {"zipapp/data/설치.md", "switch/zipapp/data/설치.md", note.size()}};
    foil::Server server;
    server.url = mock.url;
    std::atomic<uint64_t> received{0};
    foil::Error error;
    CHECK(foil::net::homebrewDownload(server, foil::State{}, app, sd.string(), received, error, nullptr));
    mock.thread.join();
    CHECK(foil::readFile((sd / "switch/zipapp/zipapp.nro").string()) == nro);
    CHECK(foil::readFile((sd / "switch/zipapp/data/설치.md").string()) == note);
    CHECK(!fs::exists(sd / "README.txt") && !fs::exists(sd / "switch/README.txt"));
    CHECK(mock.requests.size() == 3 && rangeOf(mock.requests[2]) == std::to_string(ends[0]) + "-");
    CHECK(!fs::exists(sd / "switch/cookingfoil/resume/homebrew-zipapp.json"));
    fs::remove_all(sd);
  }
  {
    // A zip that is not what its directory says (a bad CRC), or not there at
    // all: the .nro alone comes from download_url.
    for (const bool missing : {false, true}) {
      const auto sd = fs::temp_directory_path() / ("cf-zip-bad-" + std::to_string(getpid()));
      fs::remove_all(sd);
      fs::create_directories(sd);
      auto nro = varied(3000, 3);
      nro.replace(0x10, 4, "NRO0"); // an NRO as far as its header goes: magic, then its size
      nro[0x18] = char(0x80), nro[0x19] = nro[0x1A] = nro[0x1B] = 0;
      std::vector<ZipFile> one(1);
      one[0] = {"zipapp/zipapp.nro", nro, true, false};
      auto zip = zipOf(one);
      zip[zip.size() - 22 - (46 + 17) + 16] ^= 1; // the CRC in its directory record
      Mock mock;
      if (!missing)
        mock.paths["/api/homebrew/zipapp/archive"] = zip;
      mock.paths["/api/homebrew/zipapp/download"] = nro;
      mock.start(missing ? 2 : 3);
      foil::homebrew::App app;
      app.id = "zipapp";
      app.installPath = "switch/zipapp/zipapp.nro";
      app.downloadUrl = mock.url + "/api/homebrew/zipapp/download";
      app.archive.format = "zip";
      app.archive.url = mock.url + "/api/homebrew/zipapp/archive";
      app.archive.size = zip.size();
      app.archive.sha256 = std::string(64, 'c');
      app.archive.entries = {{"zipapp/zipapp.nro", "switch/zipapp/zipapp.nro", nro.size()}};
      foil::Server server;
      server.url = mock.url;
      std::atomic<uint64_t> received{0};
      foil::Error error;
      CHECK(foil::net::homebrewDownload(server, foil::State{}, app, sd.string(), received, error, nullptr));
      mock.thread.join();
      CHECK(foil::readFile((sd / "switch/zipapp/zipapp.nro").string()) == nro);
      CHECK(mock.requests.size() == (missing ? 2u : 3u));
      fs::remove_all(sd);
    }
  }
  {
    // The directory read straight: names, sizes, methods and offsets.
    std::vector<ZipFile> two(2);
    two[0] = {"a.nro", varied(1000, 4), true, false};
    two[1] = {"b/c.txt", "hello", false, true};
    const auto zip = zipOf(two);
    uint64_t at = 0, length = 0;
    std::vector<foil::ZipEntry> entries;
    CHECK(foil::zipDirectory(zip, 0, zip.size(), at, length));
    CHECK(foil::zipEntries(zip.substr(size_t(at), size_t(length)), entries));
    CHECK(entries.size() == 2 && entries[0].name == "a.nro" && entries[0].method == 8 && entries[0].size == 1000 &&
          entries[1].name == "b/c.txt" && entries[1].method == 0 && entries[1].size == 5 && entries[1].offset > 0);
    CHECK(!foil::zipDirectory("not a zip at all, just words", 0, 28, at, length));
    // A zip64 locator whose offset wraps past 2^64 when its record's length
    // is added: refused, not read from outside the tail.
    const auto le = [](uint64_t v, int bytes) {
      std::string out;
      for (int i = 0; i < bytes; ++i) out += char(v >> (8 * i) & 0xFF);
      return out;
    };
    const std::string wrapped = le(0x07064b50, 4) + le(0, 4) + le(0xFFFFFFFFFFFFFFF0ULL, 8) + le(1, 4) +
                                le(0x06054b50, 4) + le(0, 4) + le(0xFFFF, 2) + le(0xFFFF, 2) +
                                le(0xFFFFFFFF, 4) + le(0xFFFFFFFF, 4) + le(0, 2);
    CHECK(!foil::zipDirectory(wrapped, 0, wrapped.size(), at, length));
  }
  return failures;
}
