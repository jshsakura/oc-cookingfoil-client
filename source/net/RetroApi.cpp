#include "net/RetroApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include "net/Api.hpp"
#include "net/Ranged.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
namespace foil::net {
namespace {
std::string query(const std::string &text) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (const unsigned char c : text) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += char(c);
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}
// GET a JSON endpoint and sort the reply into missing, busy or failed.
Response fetch(const Server &server, const State &state, const std::string &path, RetroReply &reply,
               const std::atomic<bool> *cancel) {
  const auto url = server.url + path;
  auto response = get(url, authHeaders(server, state, url), "", cancel);
  if (response.status == 404) {
    reply.missing = true;
    reply.error.message = tr("This server has no retro games.");
  } else if (response.status == 503) {
    Error ignored;
    const auto body = parseJson(response.body, ignored);
    reply.retryAfter = int(std::clamp<int64_t>(body ? integerField(body.get(), "retryAfter", 30) : 30, 5, 300));
    reply.error.message = tr("The server is reading its retro files. Loading again shortly.");
  } else if (!response.error.empty() || response.status != 200) {
    reply.error.message = tr("Could not load the retro games.");
  }
  return response;
}
} // namespace

Parsed<std::vector<retro::System>> retroSystems(const Server &server, const State &state, RetroReply &reply,
                                                 const std::atomic<bool> *cancel) {
  const auto response = fetch(server, state, "/api/retro/systems", reply, cancel);
  if (reply.error) return {{}, reply.error};
  return {retro::parseSystems(response.body, server.url), {}};
}
Parsed<retro::Page> retroGames(const Server &server, const State &state, const std::string &system, int page,
                               const retro::Filter &filter, RetroReply &reply, const std::atomic<bool> *cancel) {
  const auto path = "/api/retro/games?system=" + query(system) + "&page=" + std::to_string(page) + "&pageSize=50" +
                    retro::filterQuery(filter);
  const auto response = fetch(server, state, path, reply, cancel);
  if (reply.error) return {{}, reply.error};
  return {retro::parseGames(response.body, server.url), {}};
}
Parsed<retro::Detail> retroDetail(const Server &server, const State &state, const std::string &id, RetroReply &reply,
                                  const std::atomic<bool> *cancel) {
  const auto response = fetch(server, state, "/api/retro/games/" + query(id), reply, cancel);
  if (reply.error) return {{}, reply.error};
  return {retro::parseDetail(response.body, server.url), {}};
}
Parsed<std::vector<retro::Shelf>> retroHome(const Server &server, const State &state, RetroReply &reply,
                                             const std::atomic<bool> *cancel) {
  const auto response = fetch(server, state, "/api/retro/home?limit=20", reply, cancel);
  if (reply.error) return {{}, reply.error};
  return {retro::parseHome(response.body, server.url), {}};
}
Parsed<retro::Pack> retroPack(const Server &server, const State &state, RetroReply &reply,
                              const std::atomic<bool> *cancel) {
  const auto response = fetch(server, state, "/api/retro/base-pack/manifest", reply, cancel);
  if (reply.error) return {{}, reply.error};
  return {retro::parsePack(response.body, server.url), {}};
}
namespace {
// A tar unpacked straight onto the card; a drop resumes at the last whole
// file (what is already there is complete), a restart overwrites.
bool fetchTar(const Server &server, const State &state, const std::string &url, const std::string &sd,
              const std::function<bool(const std::string &)> &allow,
              const std::function<bool(const std::string &)> &skip, bool game,
              std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel) {
  const auto fetch = rangedTar(url, downloadHeaders(server, state, url), 0, "",
                               [&](uint64_t) {
                                 auto writer = std::make_unique<TarWriter>(sd, allow);
                                 if (skip) writer->skip(skip);
                                 return writer;
                               },
                               nullptr, received, cancel);
  if (!fetch.tarError.empty()) { error.message = fetch.tarError; return false; }
  if (!fetch.response.error.empty()) { error.message = fetch.response.error; return false; }
  if (!rangedOk(fetch.response)) {
    error.message = retro::bundleFailure(fetch.response.status, game);
    return false;
  }
  return fetch.complete;
}
} // namespace
bool retroBundle(const Server &server, const State &state, const retro::Game &game, const std::string &sd,
                 std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel) {
  return fetchTar(server, state, game.bundleUrl, sd, [](const std::string &path) { return retro::bundlePathAllowed(path); },
                  nullptr, true, received, error, cancel);
}
bool retroPackBundle(const Server &server, const State &state, const retro::Pack &pack, const std::string &sd,
                     std::function<bool(const std::string &)> skip, std::atomic<uint64_t> &received, Error &error,
                     const std::atomic<bool> *cancel) {
  return fetchTar(server, state, pack.bundleUrl, sd, [](const std::string &path) { return retro::packPathAllowed(path); },
                  skip, false, received, error, cancel);
}
bool retroFile(const Server &server, const State &state, const retro::PackFile &file, const std::string &sd,
               std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel, long *status) {
  if (!retro::packPathAllowed(file.path)) {
    error.message = trf("Not an allowed place: {0}", {file.path});
    return false;
  }
  const std::filesystem::path target = sd + "/" + file.path;
  const auto part = target.string() + ".part";
  std::error_code ec;
  makeDirectories(std::filesystem::path(target.parent_path()).string());
  PartInfo wanted;
  wanted.url = file.url;
  wanted.tag = file.sha256;
  wanted.size = file.size;
  const auto got = fetchPart(file.url, downloadHeaders(server, state, file.url), part, wanted, false, received, cancel);
  if (status) *status = got.response.status;
  const auto fail = [&](const std::string &message) {
    if (!keepPart(got)) dropPart(part);
    error.message = message;
    return false;
  };
  if (!got.failure.empty()) return fail(got.failure);
  if (got.tooLong) return fail(trf("The downloaded file has the wrong size: {0}", {file.path}));
  if (!got.response.error.empty()) return fail(got.response.error);
  if (!rangedOk(got.response))
    return fail(trf("Could not get the file. HTTP {0}", {std::to_string(got.response.status)}));
  if (file.size && got.size != file.size) return fail(trf("The downloaded file has the wrong size: {0}", {file.path}));
  // The old file goes only once the new one can take its place.
  if (std::string reason; !replaceFile(part, target, reason))
    return fail(trf("Could not write the file: {0}", {file.path}));
  std::filesystem::remove(partInfoPath(part), ec);
  return true;
}
} // namespace foil::net
