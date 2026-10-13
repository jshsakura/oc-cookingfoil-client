#include "net/HomebrewApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include <set>
#include "core/Log.hpp"
#include "core/Patches.hpp"
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include "core/Sha256.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
namespace foil::net {
bool homebrewList(const Server &server, const State &state, std::string &etag, std::vector<homebrew::App> &apps,
                  bool &unchanged, Error &error, const std::atomic<bool> *cancel) {
  // Descriptions come in the app's language (Korean, else English).
  const auto url = server.url + "/api/homebrew?lang=" + languageCode();
  const auto response = get(url, authHeaders(server, state, url), etag, cancel);
  unchanged = response.status == 304;
  if (unchanged) return true;
  if (!response.error.empty()) error.message = response.error;
  else if (response.status == 404) error.message = tr("This server does not offer homebrew.");
  else if (response.status != 200) error.message = trf("Could not load the homebrew list. HTTP {0}", {std::to_string(response.status)});
  if (error) return false;
  apps = homebrew::parse(response.body, server.url);
  etag = response.etag;
  return true;
}
namespace {
// One file to sd/<path> through a .part file, resumed when an earlier try
// left one. `sha256` empty takes the download's ETag as the checksum (the
// single .nro); `size` 0 skips that check.
bool fetchFile(const Server &server, const State &state, const std::string &url, const std::string &sd,
               const std::string &path, std::string sha256, uint64_t size, std::atomic<uint64_t> &received,
               Error &error, const std::atomic<bool> *cancel) {
  // The part file has a short plain name beside the target ("설치-스위치.md"
  // could not be created as "설치-스위치.md.part" on the console); the
  // finished file takes the listed name.
  const auto target = sd + "/" + path;
  const auto folder = std::filesystem::path(target).parent_path().string();
  Sha256 name;
  name.update(path.data(), path.size());
  const auto part = folder + "/.cf-" + name.hex().substr(0, 16) + ".part";
  std::error_code ec;
  if (!makeDirectories(folder)) {
    error.message = trf("Could not create file: {0}", {path}) + " (" + std::strerror(errno) + ")";
    return false;
  }
  PartInfo wanted;
  wanted.url = url;
  wanted.tag = sha256;
  wanted.size = size;
  const auto got = fetchPart(url, authHeaders(server, state, url), part, wanted, true, received, cancel);
  const auto fail = [&](const std::string &message) {
    if (!keepPart(got)) dropPart(part);
    error.message = message;
    return false;
  };
  if (got.tooLong) return fail(tr("The downloaded file does not match the server's. Try again."));
  if (!got.failure.empty()) return fail(got.failure + " · " + path); // the listed name, not the part's
  if (!got.response.error.empty()) return fail(got.response.error);
  if (!rangedOk(got.response))
    return fail(trf("Could not get the file. HTTP {0}", {std::to_string(got.response.status)}));
  if (sha256.empty()) sha256 = homebrew::etagSha256(got.response.etag); // without one the file is kept unchecked
  if ((size && got.size != size) || (!sha256.empty() && got.sha256 != sha256))
    return fail(tr("The downloaded file does not match the server's. Try again."));
  std::filesystem::remove(target, ec);
  ec.clear();
  std::filesystem::rename(part, target, ec);
  if (ec) return fail(trf("Could not write the file: {0}", {path}) + " (" + ec.message() + ")");
  std::filesystem::remove(partInfoPath(part), ec);
  return true;
}
// A file already on the card with the listed size and hash: nothing to fetch.
bool alreadyThere(const std::string &target, uint64_t size, const std::string &sha256) {
  std::error_code ec;
  if (sha256.empty() || !size || std::filesystem::file_size(target, ec) != size || ec) return false;
  std::unique_ptr<FILE, int (*)(FILE *)> in(std::fopen(target.c_str(), "rb"), std::fclose);
  if (!in) return false;
  Sha256 hash;
  std::vector<char> chunk(256 * 1024);
  size_t n;
  while ((n = std::fread(chunk.data(), 1, chunk.size(), in.get())) > 0) hash.update(chunk.data(), n);
  return hash.hex() == sha256;
}
// A folder install as one tar stream: one request at full speed, and a cut
// goes on from the last whole file (kept in switch/cookingfoil/resume). Only
// the files the list names are written.
bool fetchBundle(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                 std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel, bool &unusable) {
  unusable = false;
  std::set<std::string> listed;
  PartInfo wanted;
  wanted.url = app.bundleUrl;
  Sha256 list;
  for (const auto &file : app.files) {
    listed.insert(file.path);
    wanted.size += file.size;
    list.update(file.path.data(), file.path.size());
    list.update(file.sha256.data(), file.sha256.size());
  }
  wanted.tag = list.hex(); // the same files, the same build
  const auto record = sd + "/switch/cookingfoil/resume/homebrew-" + app.id + ".json";
  const auto kept = parsePartInfo(readFile(record));
  const uint64_t from = samePart(kept, wanted) ? kept.offset : 0;
  if (from) logLine("homebrew " + app.id + " resumes at " + std::to_string(from >> 20) + " MB");
  const auto fetch = rangedTar(app.bundleUrl, authHeaders(server, state, app.bundleUrl), from, from ? kept.etag : "",
      [&](uint64_t) {
        return std::make_unique<TarWriter>(sd, [&listed](const std::string &path) { return listed.count(path) > 0; });
      },
      [&](uint64_t boundary, const std::string &etag) {
        auto info = wanted;
        info.offset = boundary;
        info.etag = etag;
        std::string ignored;
        makeDirectories(sd + "/switch/cookingfoil/resume");
        atomicWrite(record, serializePartInfo(info), ignored);
      },
      received, cancel);
  std::error_code ec;
  const auto fail = [&](const std::string &message, bool keep) {
    if (!keep) std::filesystem::remove(record, ec);
    error.message = message;
    return false;
  };
  // The bundle itself will not do (no such address, an error page, not a
  // tar we can read): the caller takes the files one by one instead. A cut
  // connection keeps its place for the next try.
  if (!fetch.tarError.empty()) {
    unusable = true;
    return fail(fetch.tarError, false);
  }
  if (!fetch.response.error.empty()) return fail(fetch.response.error, true);
  if (!rangedOk(fetch.response)) {
    unusable = true;
    return fail(trf("Could not get the file. HTTP {0}", {std::to_string(fetch.response.status)}), false);
  }
  if (!fetch.complete) return fail(tr("The download was interrupted. Download it again."), true);
  std::filesystem::remove(record, ec);
  return true;
}
} // namespace
bool homebrewDownload(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel) {
  if (app.files.empty())
    return fetchFile(server, state, app.downloadUrl, sd, app.installPath, "", 0, received, error, cancel);
  if (!app.bundleUrl.empty()) {
    bool unusable = false;
    const uint64_t before = received;
    if (fetchBundle(server, state, app, sd, received, error, cancel, unusable)) return true;
    if (!unusable || (cancel && *cancel)) return false;
    // Falls back to one request a file; what the bundle already wrote is
    // checked by its hash and kept.
    logLine("homebrew " + app.id + ": bundle unusable (" + error.message + "), file by file");
    received = before;
    error = {};
  }
  // A folder install: thousands of small files, each its own request, so
  // most of the time is the round trip; eight at a time, each checked. One
  // already on the card as the server lists it (size and hash) is kept.
  // One failing stops the rest (those in flight finish their file).
  constexpr size_t Workers = 8;
  std::atomic<size_t> next{0};
  std::atomic<bool> failed{false};
  std::mutex firstLock;
  Error first;
  const auto work = [&] {
    while (!failed && !(cancel && *cancel)) {
      const size_t i = next++;
      if (i >= app.files.size()) return;
      const auto &file = app.files[i];
      if (alreadyThere(sd + "/" + file.path, file.size, file.sha256)) {
        received += file.size;
        continue;
      }
      Error mine;
      if (!fetchFile(server, state, file.url, sd, file.path, file.sha256, file.size, received, mine, cancel)) {
        std::lock_guard<std::mutex> held(firstLock);
        if (!failed.exchange(true)) first = mine;
        return;
      }
    }
  };
  std::vector<std::thread> threads;
  for (size_t n = 1; n < std::min(Workers, app.files.size()); ++n) threads.emplace_back(work);
  work();
  for (auto &t : threads) t.join();
  if (failed) {
    error = first;
    return false;
  }
  return !(cancel && *cancel); // cancelled: the caller says so
}
} // namespace foil::net
