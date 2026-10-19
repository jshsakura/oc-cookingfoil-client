#include "net/HomebrewApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include "platform/Storage.hpp"
#include <set>
#include <algorithm>
#include "core/Log.hpp"
#include "core/Patches.hpp"
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include "core/Sha256.hpp"
#include "core/ZipStream.hpp"
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
namespace foil::net {
bool homebrewList(const Server &server, const State &state, std::string &etag, std::vector<homebrew::App> &apps,
                  bool &unchanged, Error &error, const std::atomic<bool> *cancel, bool titles) {
  // Descriptions come in the app's language (Korean, else English).
  const auto url = server.url + "/api/homebrew?lang=" + languageCode() + (titles ? "&with=titles" : "");
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
               Error &error, const std::atomic<bool> *cancel, int *busy = nullptr) {
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
  const auto got = fetchPart(url, downloadHeaders(server, state, url), part, wanted, true, received, cancel);
  const auto fail = [&](const std::string &message) {
    if (!keepPart(got)) dropPart(part);
    error.message = message;
    return false;
  };
  if (got.tooLong) return fail(tr("The downloaded file does not match the server's. Try again."));
  if (!got.failure.empty()) return fail(got.failure + " · " + path); // the listed name, not the part's
  if (!got.response.error.empty()) return fail(got.response.error);
  if (!rangedOk(got.response)) {
    // The file changed on the server since the list: it says how long to wait.
    if (busy && got.response.status == 503) *busy = got.response.retryAfter > 0 ? got.response.retryAfter : 5;
    return fail(trf("Could not get the file. HTTP {0}", {std::to_string(got.response.status)}));
  }
  if (sha256.empty()) sha256 = homebrew::etagSha256(got.response.etag); // without one the file is kept unchecked
  if ((size && got.size != size) || (!sha256.empty() && got.sha256 != sha256))
    return fail(tr("The downloaded file does not match the server's. Try again."));
  // An app with no hash to check it by: at least whole as an NRO (its icon
  // and name are at the end, which a broken write would lose).
  if (sha256.empty() && path.size() > 4 && path.compare(path.size() - 4, 4, ".nro") == 0 && !platform::wholeNro(part))
    return fail(tr("The downloaded file does not match the server's. Try again."));
  std::filesystem::remove(target, ec);
  ec.clear();
  std::filesystem::rename(part, target, ec);
  if (ec) return fail(trf("Could not write the file: {0}", {path}) + " (" + ec.message() + ")");
  std::filesystem::remove(partInfoPath(part), ec);
  return true;
}
// A file already on the card with the listed size and hash: nothing to fetch.
bool alreadyThere(const std::string &target, uint64_t size, const std::string &sha256, const std::atomic<bool> *cancel,
                  std::atomic<uint64_t> *read) {
  std::error_code ec;
  if (sha256.empty() || !size || std::filesystem::file_size(target, ec) != size || ec) return false;
  return sha256File(target, cancel, read) == sha256;
}
// A folder install as one tar stream: one request at full speed, and a cut
// goes on from the last whole file (kept in switch/cookingfoil/resume). Only
// the files the list names are written.
bool fetchBundle(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                 const std::set<std::string> &onCard, std::atomic<uint64_t> &received, Error &error,
                 const std::atomic<bool> *cancel, bool &unusable) {
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
  const auto fetch = rangedTar(app.bundleUrl, downloadHeaders(server, state, app.bundleUrl), from, from ? kept.etag : "",
      [&](uint64_t) {
        auto writer = std::make_unique<TarWriter>(sd, [&listed](const std::string &path) { return listed.count(path) > 0; });
        // Files already on the card as listed are read past, not written again.
        writer->skip([&onCard](const std::string &path) { return onCard.count(path) > 0; });
        return writer;
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
// The server keeps the app as a zip: its directory first (from the zip's
// last bytes), then the whole zip in one request, each listed entry written
// to its path as it arrives and checked by its CRC. A cut goes on from the
// last whole file. `unusable`: the zip itself will not do (no such address,
// not a zip, an entry the list names missing); `retryAfter`: the server's
// file changed since the list (503), ask the list again.
bool fetchArchive(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                  std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel, bool &unusable,
                  int &retryAfter) {
  unusable = false;
  retryAfter = 0;
  const auto &zip = app.archive;
  const auto headers = downloadHeaders(server, state, zip.url);
  const auto busy = [&](const Response &reply) {
    if (reply.status != 503) return false;
    retryAfter = reply.retryAfter > 0 ? reply.retryAfter : 5;
    error.message = trf("Could not get the file. HTTP {0}", {"503"});
    return true;
  };
  const auto range = [&](uint64_t from, uint64_t length, std::string &out) {
    auto asked = headers;
    asked.push_back("Range: bytes=" + std::to_string(from) + "-" + std::to_string(from + length - 1));
    const auto reply = get(zip.url, asked, "", cancel);
    if (busy(reply)) return false;
    if (!reply.error.empty()) {
      error.message = reply.error;
      return false;
    }
    if (reply.status == 200 && reply.body.size() == zip.size) out = reply.body.substr(size_t(from), size_t(length));
    else if (reply.status == 206 && reply.body.size() == length) out = reply.body;
    else {
      unusable = true;
      error.message = trf("Could not get the file. HTTP {0}", {std::to_string(reply.status)});
      return false;
    }
    return true;
  };
  const uint64_t tailStart = zip.size > ZipTailBytes ? zip.size - ZipTailBytes : 0;
  std::string tail, directory;
  if (!range(tailStart, zip.size - tailStart, tail)) return false;
  uint64_t at = 0, length = 0;
  std::vector<ZipEntry> inZip;
  if (!zipDirectory(tail, tailStart, zip.size, at, length)) {
    unusable = true;
    error.message = tr("This archive cannot be unpacked.");
    return false;
  }
  if (at >= tailStart) directory = tail.substr(size_t(at - tailStart), size_t(length));
  else if (!range(at, length, directory)) return false;
  if (!zipEntries(directory, inZip)) {
    unusable = true;
    error.message = tr("This archive cannot be unpacked.");
    return false;
  }
  std::vector<std::pair<ZipEntry, std::string>> wanted;
  for (const auto &listed : zip.entries) {
    const auto found = std::find_if(inZip.begin(), inZip.end(), [&](const ZipEntry &e) { return e.name == listed.entry; });
    if (found == inZip.end() || (listed.size && found->size != listed.size)) {
      unusable = true;
      error.message = trf("The archive does not match the server's list: {0}", {listed.path});
      return false;
    }
    wanted.emplace_back(*found, listed.path);
  }
  PartInfo part;
  part.url = zip.url;
  part.size = zip.size;
  part.tag = zip.sha256; // the same zip
  const auto record = sd + "/switch/cookingfoil/resume/homebrew-" + app.id + ".json";
  const auto kept = parsePartInfo(readFile(record));
  const uint64_t from = samePart(kept, part) ? kept.offset : 0;
  if (from) logLine("homebrew " + app.id + " resumes the zip at " + std::to_string(from >> 20) + " MB");
  const auto fetch = rangedTar(zip.url, headers, from, from ? kept.etag : "",
      [&](uint64_t) { return std::make_unique<ZipWriter>(sd, wanted); },
      [&](uint64_t boundary, const std::string &etag) {
        auto info = part;
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
    if (error.message.empty()) error.message = message;
    return false;
  };
  if (busy(fetch.response)) return fail(error.message, false);
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
// The zip, and once more after the server's file changed (a new list, the
// zip from the start); a zip that will not do leaves the .nro alone, from
// download_url.
bool homebrewArchive(const Server &server, const State &state, homebrew::App app, const std::string &sd,
                     std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel) {
  const uint64_t before = received;
  for (int round = 0;; ++round) {
    bool unusable = false;
    int retryAfter = 0;
    if (fetchArchive(server, state, app, sd, received, error, cancel, unusable, retryAfter)) return true;
    if (cancel && *cancel) return false;
    if (retryAfter && round == 0) {
      logLine("homebrew " + app.id + ": the zip changed on the server, listing again");
      const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::min(retryAfter, 30));
      while (std::chrono::steady_clock::now() < until && !(cancel && *cancel))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      std::string etag;
      std::vector<homebrew::App> apps;
      bool unchanged = false;
      Error listError;
      if (!homebrewList(server, state, etag, apps, unchanged, listError, cancel)) return false;
      const auto again = std::find_if(apps.begin(), apps.end(), [&](const homebrew::App &a) { return a.id == app.id; });
      if (again == apps.end() || !again->archive.usable()) return false;
      app = *again;
      received = before;
      error = {};
      continue;
    }
    if (!unusable) return false;
    logLine("homebrew " + app.id + ": zip unusable (" + error.message + "), the .nro alone");
    received = before;
    error = {};
    return fetchFile(server, state, app.downloadUrl, sd, app.installPath, "", 0, received, error, cancel);
  }
}
// A folder install: each listed file the card does not have yet. `busy`:
// a file changed on the server since the list (503), and how long to wait.
bool homebrewFiles(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                   std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel,
                   std::atomic<uint64_t> *keptBytes, int &busy, CheckProgress *check) {
  busy = 0;
  const auto passed = [keptBytes](uint64_t bytes) {
    if (keptBytes) *keptBytes += bytes;
  };
  // What the card already has as listed (a cut install, or a new build that
  // kept most files): checked once, by size and then hash. A fresh install
  // has none of them and passes at once.
  // Each file found is counted as it is found, so the screen fills its
  // lighter part as the check goes rather than jumping at the end.
  std::vector<bool> have(app.files.size());
  std::set<std::string> kept;
  uint64_t total = 0, missing = 0;
  size_t missingFiles = 0;
  if (check) {
    uint64_t bytes = 0;
    for (const auto &file : app.files) bytes += file.size;
    check->done = 0;
    check->total = bytes;
  }
  struct Done {
    CheckProgress *check;
    ~Done() { if (check) check->total = 0; }
  } done{check};
  for (size_t i = 0; i < app.files.size(); ++i) {
    if (cancel && *cancel) return false;
    const auto &file = app.files[i];
    total += file.size;
    const uint64_t before = check ? check->done.load() : 0;
    have[i] = alreadyThere(sd + "/" + file.path, file.size, file.sha256, cancel, check ? &check->done : nullptr);
    if (have[i]) {
      kept.insert(file.path);
      received += file.size;
      passed(file.size);
    } else
      missing += file.size, ++missingFiles;
    if (check) check->done = before + file.size; // a file passed by its size counts whole
  }
  if (check) check->total = 0;
  if (!missingFiles) return true;

  // Most of it there: only what is missing, a file at a time. Otherwise the
  // whole bundle in one stream, the files already there read past.
  constexpr uint64_t FewFiles = 200;
  const bool fewMissing = missing * 4 <= total && missingFiles <= FewFiles;
  const bool bundle = !app.bundleUrl.empty() && !fewMissing;
  if (bundle) {
    // The bundle streams the files already there too, and counts them as
    // they come.
    received -= std::min<uint64_t>(received, total - missing);
    if (keptBytes) *keptBytes -= std::min<uint64_t>(*keptBytes, total - missing);
    bool unusable = false;
    const uint64_t before = received;
    if (fetchBundle(server, state, app, sd, kept, received, error, cancel, unusable)) return true;
    if (!unusable || (cancel && *cancel)) return false;
    // Falls back to one request a file; what the bundle already wrote is
    // checked by its hash and kept.
    logLine("homebrew " + app.id + ": bundle unusable (" + error.message + "), file by file");
    received = before;
    error = {};
  }
  // What the card has counts again after a bundle that gave out (it took
  // them back to count them as they streamed).
  if (bundle) {
    received += total - missing;
    passed(total - missing);
  }
  // A folder install: thousands of small files, each its own request, so
  // most of the time is the round trip; eight at a time, each checked. One
  // already on the card as the server lists it (found above, or written by
  // the bundle before it gave out) is kept.
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
      if (have[i]) continue; // counted above
      if (alreadyThere(sd + "/" + file.path, file.size, file.sha256, cancel, nullptr)) { // the bundle wrote it before it gave out
        received += file.size;
        passed(file.size);
        continue;
      }
      Error mine;
      int wait = 0;
      if (!fetchFile(server, state, file.url, sd, file.path, file.sha256, file.size, received, mine, cancel, &wait)) {
        std::lock_guard<std::mutex> held(firstLock);
        if (!failed.exchange(true)) first = mine;
        if (wait) busy = wait;
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
} // namespace
bool homebrewFilesOf(const Server &server, const State &state, homebrew::App &app, Error &error,
                     const std::atomic<bool> *cancel) {
  if (!app.files.empty() || !app.filesCount) return true;
  const auto response = get(app.filesUrl, authHeaders(server, state, app.filesUrl), "", cancel);
  if (!response.error.empty()) error.message = response.error;
  else if (response.status != 200) error.message = trf("Could not load the homebrew list. HTTP {0}", {std::to_string(response.status)});
  else if (!homebrew::parseFiles(response.body, app, server.url)) error.message = tr("Could not read this app's file list.");
  if (error) logLine("homebrew " + app.id + ": its file list did not come: " + error.message);
  return !error;
}
bool homebrewDownload(const Server &server, const State &state, const homebrew::App &listed, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel,
                      std::atomic<uint64_t> *keptBytes, CheckProgress *check) {
  if (listed.archive.usable()) return homebrewArchive(server, state, listed, sd, received, error, cancel);
  auto app = listed;
  if (!homebrewFilesOf(server, state, app, error, cancel)) return false;
  if (app.files.empty())
    return fetchFile(server, state, app.downloadUrl, sd, app.installPath, "", 0, received, error, cancel);
  // A file the server changed since the list (503): wait as it asks, read
  // the list again and go on with the app as it is now; once.
  const uint64_t before = received, keptBefore = keptBytes ? keptBytes->load() : 0;
  for (int round = 0;; ++round) {
    int busy = 0;
    if (homebrewFiles(server, state, app, sd, received, error, cancel, keptBytes, busy, check)) return true;
    if ((cancel && *cancel) || !busy || round > 0) return false;
    logLine("homebrew " + app.id + ": a file changed on the server, listing again");
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::min(busy, 30));
    while (std::chrono::steady_clock::now() < until && !(cancel && *cancel))
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string etag;
    std::vector<homebrew::App> apps;
    bool unchanged = false;
    Error listError;
    if (!homebrewList(server, state, etag, apps, unchanged, listError, cancel)) return false;
    const auto again = std::find_if(apps.begin(), apps.end(), [&](const homebrew::App &a) { return a.id == app.id; });
    if (again == apps.end()) return false;
    app = *again;
    if (!homebrewFilesOf(server, state, app, error, cancel) || app.files.empty()) return false;
    received = before;
    if (keptBytes) *keptBytes = keptBefore;
    error = {};
  }
}
} // namespace foil::net
