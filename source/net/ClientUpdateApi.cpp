#include "net/ClientUpdateApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/Version.hpp"
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
  // The download goes to its own name; only once it is whole and its hash
  // checks out does it become ".new", which the next start puts in place.
  const std::string part = target + ".download", ready = target + ".new";
  {
    std::error_code ec; // a half download an earlier version left as ".new"
    if (std::filesystem::exists(ready + ResumeFile, ec)) {
      std::filesystem::remove(ready, ec);
      std::filesystem::remove(ready + ResumeFile, ec);
    }
  }
  makeDirectories(std::filesystem::path(target).parent_path().string());
  PartInfo wanted;
  wanted.url = release.url;
  wanted.tag = release.sha256;
  wanted.size = release.size;
  const auto got = fetchPart(release.url, downloadHeaders(server, state, release.url), part, wanted, true, received, cancel);
  const auto fail = [&](const std::string &message) {
    if (!keepPart(got)) dropPart(part);
    error.message = message;
    return false;
  };
  // Longer than the release said: the server has a newer build than the one
  // asked about.
  if (got.tooLong) return fail(tr("The update on the server changed. Checking again."));
  if (!got.failure.empty()) return fail(got.failure);
  if (!got.response.error.empty()) {
    // The connection's own words go to the log; the player reads what to do.
    logLine("update download stopped: " + got.response.error);
    return fail(cancel && cancel->load() ? tr("Cancelled the update.")
                                         : tr("The update download stopped. Check the connection and try again."));
  }
  if (!rangedOk(got.response))
    return fail(trf("Could not download the update. HTTP {0}", {std::to_string(got.response.status)}));
  if (got.size != release.size || got.sha256 != release.sha256)
    return fail(tr("The downloaded file does not match the server's. Try again."));
  // Checked and complete: platform::finishUpdate puts it in place at the
  // next start, before anything opens the app's own file.
  std::error_code ec;
  std::filesystem::remove(partInfoPath(part), ec);
  std::filesystem::remove(ready, ec);
  ec.clear();
  std::filesystem::rename(part, ready, ec);
  if (ec) {
    dropPart(part);
    error.message = trf("Could not write the file: {0}", {"cookingfoil.nro"}) + " (" + ec.message() + ")";
    return false;
  }
  return true;
}
ReportPrivate reportPrivate(const Server &server, const State &state) {
  ReportPrivate known;
  const auto scheme = server.url.find("://");
  auto host = server.url.substr(scheme == std::string::npos ? 0 : scheme + 3);
  host = host.substr(0, host.find_first_of("/?#"));
  if (const auto at = host.rfind('@'); at != std::string::npos) host.erase(0, at + 1);
  if (!host.empty() && host.front() != '[') host = host.substr(0, host.find(':'));
  if (!host.empty()) known.hosts.push_back(host);
  known.names.push_back(server.username);
  for (const auto *value : {&server.password, &server.enrollToken, &server.cfClientId, &server.cfClientSecret})
    known.secrets.push_back(*value);
  for (const auto *values : {&state.accessKeys, &state.pairSecrets, &state.pairMessages})
    for (const auto &[url, value] : *values) known.secrets.push_back(value);
  return known;
}
bool sendBugReport(const Server &server, const State &state, const std::string &log, Error &error,
                   const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/client/log";
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: text/plain; charset=utf-8");
  headers.push_back("X-Log-Reason: bug%20report"); // the version rides on every request (authHeaders)
  PostOptions options;
  options.body = forReport(log, reportPrivate(server, state));
  options.timeout = 30;
  const auto response = post(url, headers, options, cancel);
  if (!response.error.empty()) error.message = response.error;
  else if (response.status == 404) error.message = tr("The server does not take bug reports.");
  else if (response.status == 429) error.message = tr("Sent a lot just now. Try again in a minute.");
  else if (response.status < 200 || response.status >= 300)
    error.message = trf("Could not send the bug report. HTTP {0}", {std::to_string(response.status)});
  return !error;
}
} // namespace foil::net
