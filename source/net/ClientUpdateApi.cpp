#include "net/ClientUpdateApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
#include "net/Ranged.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
namespace foil::net {
std::optional<update::Release> latestClient(const Server &server, const State &state, ClientUpdateReply &reply,
                                            const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/client/latest";
  const auto response = get(url, authHeaders(server, state, url), "", cancel);
  if (response.status == 404) {
    reply.missing = true;
    return std::nullopt;
  }
  if (!response.error.empty() || response.status != 200) {
    reply.error.message = tr("Could not check for an update.");
    return std::nullopt;
  }
  auto release = update::parseRelease(response.body, server.url);
  if (!release) reply.error.message = tr("Could not read the server's update details.");
  return release;
}
bool installClient(const Server &server, const State &state, const update::Release &release,
                   const std::string &target, std::atomic<uint64_t> &received, Error &error,
                   const std::atomic<bool> *cancel) {
  // Into <target>.new, resumed when an earlier try of the same build left
  // part of it there.
  const std::string part = target + ".new";
  makeDirectories(std::filesystem::path(target).parent_path().string());
  PartInfo wanted;
  wanted.url = release.url;
  wanted.tag = release.sha256;
  wanted.size = release.size;
  const auto got = fetchPart(release.url, authHeaders(server, state, release.url), part, wanted, true, received, cancel);
  const auto fail = [&](const std::string &message) {
    if (!keepPart(got)) dropPart(part);
    error.message = message;
    return false;
  };
  // Longer than the release said: the server has a newer build than the one
  // asked about.
  if (got.tooLong) return fail(tr("The update on the server changed. Checking again."));
  if (!got.failure.empty()) return fail(got.failure);
  if (!got.response.error.empty()) return fail(got.response.error);
  if (!rangedOk(got.response))
    return fail(trf("Could not download the update. HTTP {0}", {std::to_string(got.response.status)}));
  if (got.size != release.size || got.sha256 != release.sha256)
    return fail(tr("The downloaded file does not match the server's. Try again."));
  // Checked and complete: platform::replaceSelf puts it in place on the
  // main thread, where the app can let go of its own file.
  std::error_code ec;
  std::filesystem::remove(partInfoPath(part), ec);
  return true;
}
} // namespace foil::net
