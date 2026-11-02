#include "net/FileFetch.hpp"
#include "core/Homebrew.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Sha256.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
namespace foil::net {
std::string filePart(const std::string &sd, const std::string &path) {
  // A short plain name beside the target ("설치-스위치.md" could not be
  // created as "설치-스위치.md.part" on the console); the finished file takes
  // the listed name.
  Sha256 name;
  name.update(path.data(), path.size());
  return std::filesystem::path(sd + "/" + path).parent_path().string() + "/.cf-" + name.hex().substr(0, 16) + ".part";
}
bool fetchFile(const Server &server, const State &state, const std::string &url, const std::string &sd,
               const std::string &path, std::string sha256, uint64_t size, std::atomic<uint64_t> &received,
               Error &error, const std::atomic<bool> *cancel, int *busy, long *status, bool *refused) {
  const auto target = sd + "/" + path;
  const auto folder = std::filesystem::path(target).parent_path().string();
  const auto part = filePart(sd, path);
  // Only a real SHA-256 is checked; anything else counts as none given.
  std::transform(sha256.begin(), sha256.end(), sha256.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  if (sha256.size() != 64 || sha256.find_first_not_of("0123456789abcdef") != std::string::npos) sha256.clear();
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
  if (status) *status = got.response.status;
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
  // The old file goes only once the new one can take its place.
  if (std::string reason; !replaceFile(part, target, reason)) {
    // A full card is not the name's fault: that one is tried again.
    if (refused) *refused = reason.find(std::strerror(ENOSPC)) == std::string::npos;
    return fail(trf("Could not write the file: {0}", {path}) + " (" + reason + ")");
  }
  std::filesystem::remove(partInfoPath(part), ec);
  return true;
}
} // namespace foil::net
