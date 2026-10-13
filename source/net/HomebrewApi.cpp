#include "net/HomebrewApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
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
  const auto target = sd + "/" + path, part = target + ".part";
  std::error_code ec;
  makeDirectories(std::filesystem::path(target).parent_path().string());
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
  if (!got.failure.empty()) return fail(got.failure);
  if (!got.response.error.empty()) return fail(got.response.error);
  if (!rangedOk(got.response))
    return fail(trf("Could not get the file. HTTP {0}", {std::to_string(got.response.status)}));
  if (sha256.empty()) sha256 = homebrew::etagSha256(got.response.etag); // without one the file is kept unchecked
  if ((size && got.size != size) || (!sha256.empty() && got.sha256 != sha256))
    return fail(tr("The downloaded file does not match the server's. Try again."));
  std::filesystem::remove(target, ec);
  ec.clear();
  std::filesystem::rename(part, target, ec);
  if (ec) return fail(trf("Could not write the file: {0}", {path}));
  std::filesystem::remove(partInfoPath(part), ec);
  return true;
}
} // namespace
bool homebrewDownload(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel) {
  if (app.files.empty())
    return fetchFile(server, state, app.downloadUrl, sd, app.installPath, "", 0, received, error, cancel);
  // A folder install: every file, each checked; one failing stops the rest.
  for (const auto &file : app.files)
    if (!fetchFile(server, state, file.url, sd, file.path, file.sha256, file.size, received, error, cancel))
      return false;
  return true;
}
} // namespace foil::net
