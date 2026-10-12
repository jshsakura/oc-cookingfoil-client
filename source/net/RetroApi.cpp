#include "net/RetroApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include "net/Api.hpp"
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
    reply.error.message = tr("이 서버는 레트로 게임을 제공하지 않습니다.");
  } else if (response.status == 503) {
    Error ignored;
    const auto body = parseJson(response.body, ignored);
    reply.retryAfter = int(std::clamp<int64_t>(body ? integerField(body.get(), "retryAfter", 30) : 30, 5, 300));
    reply.error.message = tr("서버가 레트로 파일을 읽고 있습니다. 잠시 뒤 다시 불러옵니다.");
  } else if (!response.error.empty() || response.status != 200) {
    reply.error.message = tr("레트로 게임 목록을 불러오지 못했습니다.");
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
bool retroBundle(const Server &server, const State &state, const retro::Game &game, const std::string &sd,
                 std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel) {
  TarWriter writer(sd, [](const std::string &path) { return retro::bundlePathAllowed(path); });
  const auto response = stream(game.bundleUrl, authHeaders(server, state, game.bundleUrl),
                               [&](const char *data, size_t size) {
                                 received += size;
                                 return writer.feed(data, size);
                               },
                               cancel);
  if (!writer.error().empty()) { error.message = writer.error(); return false; }
  if (!response.error.empty()) { error.message = response.error; return false; }
  if (response.status != 200) {
    error.message = trf("게임을 받지 못했습니다. HTTP {0}", {std::to_string(response.status)});
    return false;
  }
  if (!writer.finish()) { error.message = writer.error(); return false; }
  return true;
}
bool retroPackBundle(const Server &server, const State &state, const retro::Pack &pack, const std::string &sd,
                     std::function<bool(const std::string &)> skip, std::atomic<uint64_t> &received, Error &error,
                     const std::atomic<bool> *cancel) {
  TarWriter writer(sd, [](const std::string &path) { return retro::packPathAllowed(path); });
  writer.skip(std::move(skip));
  const auto response = stream(pack.bundleUrl, authHeaders(server, state, pack.bundleUrl),
                               [&](const char *data, size_t size) {
                                 received += size;
                                 return writer.feed(data, size);
                               },
                               cancel);
  if (!writer.error().empty()) { error.message = writer.error(); return false; }
  if (!response.error.empty()) { error.message = response.error; return false; }
  if (response.status != 200) {
    error.message = trf("파일을 받지 못했습니다. HTTP {0}", {std::to_string(response.status)});
    return false;
  }
  if (!writer.finish()) { error.message = writer.error(); return false; }
  return true;
}
bool retroFile(const Server &server, const State &state, const retro::PackFile &file, const std::string &sd,
               std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel, long *status) {
  if (!retro::packPathAllowed(file.path)) {
    error.message = trf("허용되지 않은 위치입니다: {0}", {file.path});
    return false;
  }
  const std::filesystem::path target = sd + "/" + file.path, part = target.string() + ".part";
  std::error_code ec;
  makeDirectories(std::filesystem::path(target.parent_path()).string());
  std::unique_ptr<FILE, int (*)(FILE *)> out(std::fopen(part.string().c_str(), "wb"), std::fclose);
  if (!out) {
    error.message = trf("파일을 만들지 못했습니다: {0}", {file.path});
    return false;
  }
  uint64_t written = 0;
  bool writeFailed = false;
  const auto response = stream(file.url, authHeaders(server, state, file.url),
                               [&](const char *data, size_t size) {
                                 if (std::fwrite(data, 1, size, out.get()) != size) {
                                   writeFailed = true;
                                   return false;
                                 }
                                 written += size;
                                 received += size;
                                 return true;
                               },
                               cancel);
  const bool closed = std::fclose(out.release()) == 0;
  if (status) *status = response.status;
  const auto fail = [&](const std::string &message) {
    std::filesystem::remove(part, ec);
    error.message = message;
    return false;
  };
  if (writeFailed || !closed) return fail(trf("파일을 쓰지 못했습니다: {0}", {file.path}));
  if (!response.error.empty()) return fail(response.error);
  if (response.status != 200) return fail(trf("파일을 받지 못했습니다. HTTP {0}", {std::to_string(response.status)}));
  if (file.size && written != file.size) return fail(trf("받은 파일 크기가 다릅니다: {0}", {file.path}));
  std::filesystem::remove(target, ec);
  std::filesystem::rename(part, target, ec);
  if (ec) return fail(trf("파일을 쓰지 못했습니다: {0}", {file.path}));
  return true;
}
} // namespace foil::net
