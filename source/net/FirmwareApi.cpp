#include "net/FirmwareApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "net/Api.hpp"
#include "net/ZipFetch.hpp"
#include "platform/FileSystem.hpp"
#include <algorithm>
#include <filesystem>
namespace foil::net {
namespace {
bool sourceOff(const std::string &body) {
  Error ignored;
  const auto root = parseJson(body, ignored);
  return stringField(root.get(), "code") == "firmware_off";
}
} // namespace
bool firmwareList(const Server &server, const State &state, std::string &etag, firmware::List &list, bool &unchanged,
                  Error &error, const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/firmware";
  const auto response = get(url, authHeaders(server, state, url), etag, cancel);
  unchanged = response.status == 304;
  if (unchanged) return true;
  // No source set on the server yet: nothing to list, which the tab says
  // as an empty list rather than as a failure.
  if (response.status == 404 && sourceOff(response.body)) {
    list = {};
    etag.clear();
    return true;
  }
  if (!response.error.empty()) error.message = response.error;
  else if (response.status != 200) error.message = trf("Could not load the firmware list. HTTP {0}", {std::to_string(response.status)});
  else if (!firmware::parse(response.body, server.url, list)) error.message = tr("Could not read the firmware list.");
  if (error) return false;
  etag = response.etag;
  return true;
}
bool firmwareDownload(const firmware::Version &version, const std::string &sd, std::atomic<uint64_t> &received,
                      Error &error, const std::atomic<bool> *cancel) {
  if (!firmware::isVersion(version.version) || !version.archive.usable()) {
    error.message = tr("This firmware cannot be downloaded right now.");
    return false;
  }
  const auto staging = sd + "/" + firmware::staging(version.version);
  const auto target = sd + "/" + firmware::folder(version.version);
  const firmware::Version &v = version;
  ZipSource zip{v.archive.url, v.archive.size, v.archive.sha256.empty() ? v.archive.url : v.archive.sha256,
                sd + "/switch/cookingfoil/resume/firmware-" + v.version + ".json"};
  zip.follow = true; // a release asset answers with a redirect to its storage
  const auto pick = [&](const std::vector<ZipEntry> &inZip, std::vector<std::pair<ZipEntry, std::string>> &wanted,
                        std::string &why) { return firmware::pickEntries(v, inZip, wanted, why); };
  // A staging folder with no resume point is what an older try left: gone
  // before this one writes.
  std::error_code ec;
  if (!std::filesystem::exists(zip.record, ec) && !platform::removePath(staging, ec)) {
    error.message = ec.message();
    return false;
  }
  bool unusable = false;
  int retryAfter = 0;
  long refused = 0;
  // No headers: the source is not ours, and gets neither our server's key
  // nor the console's.
  if (!fetchZip({}, zip, sd, pick, received, error, cancel, unusable, retryAfter, &refused)) {
    if (cancel && *cancel) return false;
    // Gone from the source (410), or no longer there (404): the list's own
    // word, not a bare status.
    if (refused == 410 || refused == 404) error.message = tr("This firmware cannot be downloaded right now.");
    if (unusable) platform::removePath(staging, ec);
    return false;
  }
  // Whole: it takes the version's place, an older copy of it going first.
  makeDirectories(sd + "/firmware");
  if (!platform::removePath(target, ec) || !platform::renamePath(staging, target, ec)) {
    error.message = ec.message();
    logLine("firmware " + v.version + ": not moved into place: " + ec.message());
    return false;
  }
  return true;
}
} // namespace foil::net
