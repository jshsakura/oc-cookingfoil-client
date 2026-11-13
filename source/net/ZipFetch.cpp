#include "net/ZipFetch.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Resume.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include <filesystem>
#include <memory>
namespace foil::net {
namespace {
constexpr int MaxRedirects = 5;
constexpr int MaxRenewals = 2;
bool isRedirect(long status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }
bool startsWith(const std::string &text, const char *prefix) { return text.rfind(prefix, 0) == 0; }
// An absolute http(s) address, and never from https down to http.
bool followable(const std::string &from, const std::string &to) {
  if (startsWith(to, "https://")) return true;
  return startsWith(to, "http://") && !startsWith(from, "https://");
}
} // namespace
bool fetchZip(const std::vector<std::string> &headers, const ZipSource &zip, const std::string &root,
              const ZipPick &pick, std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel,
              bool &unusable, int &retryAfter, long *refused) {
  unusable = false;
  retryAfter = 0;
  if (refused) *refused = 0;
  const auto busy = [&](const Response &reply) {
    if (reply.status != 503) return false;
    retryAfter = reply.retryAfter > 0 ? reply.retryAfter : 5;
    error.message = trf("Could not get the file. HTTP {0}", {"503"});
    return true;
  };
  // Where the bytes are asked: the source's own address, or where its
  // redirects lead (the resume point keeps the source's address).
  std::string asked = zip.url;
  const auto range = [&](uint64_t from, uint64_t length, std::string &out) {
    auto ranged = headers;
    ranged.push_back("Range: bytes=" + std::to_string(from) + "-" + std::to_string(from + length - 1));
    auto reply = get(asked, ranged, "", cancel);
    for (int hop = 0; zip.follow && hop < MaxRedirects && isRedirect(reply.status); ++hop) {
      if (!followable(asked, reply.location)) break;
      asked = reply.location;
      reply = get(asked, ranged, "", cancel);
    }
    if (busy(reply)) return false;
    if (!reply.error.empty()) {
      error.message = reply.error;
      return false;
    }
    if (reply.status == 200 && reply.body.size() == zip.size) out = reply.body.substr(size_t(from), size_t(length));
    else if (reply.status == 206 && reply.body.size() == length) out = reply.body;
    else {
      unusable = true;
      if (refused && reply.status >= 400) *refused = reply.status;
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
  if (std::string why; !pick(inZip, wanted, why)) {
    unusable = true;
    error.message = why;
    return false;
  }
  PartInfo part;
  part.url = zip.url;
  part.size = zip.size;
  part.tag = zip.tag; // the same zip
  const auto &record = zip.record;
  const auto download = [&] {
    const auto kept = parsePartInfo(readFile(record));
    const uint64_t from = samePart(kept, part) ? kept.offset : 0;
    return rangedTar(asked, headers, from, from ? kept.etag : "",
        [&](uint64_t) { return std::make_unique<ZipWriter>(root, wanted); },
        [&](uint64_t boundary, const std::string &etag) {
          auto info = part;
          info.offset = boundary;
          info.etag = etag;
          std::string ignored;
          makeDirectories(std::filesystem::path(record).parent_path().string());
          atomicWrite(record, serializePartInfo(info), ignored);
        },
        received, cancel);
  };
  auto fetch = download();
  // A signed address runs out after a few minutes: refused mid-download, the
  // source names a fresh one and it goes on from the last whole file.
  for (int renewed = 0; zip.follow && asked != zip.url && renewed < MaxRenewals && fetch.tarError.empty() &&
                        fetch.response.error.empty() && fetch.response.status >= 400 && !(cancel && *cancel);
       ++renewed) {
    asked = zip.url;
    std::string ignored;
    if (!range(0, 1, ignored)) return false;
    fetch = download();
  }
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
    if (refused && fetch.response.status >= 400) *refused = fetch.response.status;
    return fail(trf("Could not get the file. HTTP {0}", {std::to_string(fetch.response.status)}), false);
  }
  if (!fetch.complete) return fail(tr("The download was interrupted. Download it again."), true);
  std::filesystem::remove(record, ec);
  return true;
}
} // namespace foil::net
