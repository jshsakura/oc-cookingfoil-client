#include "net/HomebrewApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Sha256.hpp"
#include "net/Api.hpp"
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
  else if (response.status == 404) error.message = tr("이 서버는 홈브류를 제공하지 않습니다.");
  else if (response.status != 200) error.message = trf("홈브류 목록을 불러오지 못했습니다. HTTP {0}", {std::to_string(response.status)});
  if (error) return false;
  apps = homebrew::parse(response.body, server.url);
  etag = response.etag;
  return true;
}
bool homebrewDownload(const Server &server, const State &state, const homebrew::App &app, const std::string &sd,
                      std::atomic<uint64_t> &received, Error &error, const std::atomic<bool> *cancel) {
  const auto target = sd + "/" + homebrew::sdPath(app.fileName), part = target + ".part";
  std::error_code ec;
  makeDirectories(std::filesystem::path(target).parent_path().string());
  std::unique_ptr<FILE, int (*)(FILE *)> out(std::fopen(part.c_str(), "wb"), std::fclose);
  if (!out) {
    error.message = trf("파일을 만들지 못했습니다: {0}", {homebrew::sdPath(app.fileName)}) + " (" + std::strerror(errno) + ")";
    return false;
  }
  Sha256 hash;
  bool writeFailed = false;
  const auto response = stream(app.downloadUrl, authHeaders(server, state, app.downloadUrl),
                               [&](const char *data, size_t size) {
                                 if (std::fwrite(data, 1, size, out.get()) != size) {
                                   writeFailed = true;
                                   return false;
                                 }
                                 hash.update(data, size);
                                 received += size;
                                 return true;
                               },
                               cancel);
  const bool closed = std::fclose(out.release()) == 0;
  const auto fail = [&](const std::string &message) {
    std::filesystem::remove(part, ec);
    error.message = message;
    return false;
  };
  if (!response.error.empty()) return fail(response.error);
  if (response.status != 200) return fail(trf("파일을 받지 못했습니다. HTTP {0}", {std::to_string(response.status)}));
  if (writeFailed || !closed) return fail(trf("파일을 쓰지 못했습니다: {0}", {homebrew::sdPath(app.fileName)}));
  // The ETag is the file's SHA-256; without one the file is kept unchecked.
  const auto expected = homebrew::etagSha256(response.etag);
  if (!expected.empty() && hash.hex() != expected) return fail(tr("받은 파일이 서버의 것과 다릅니다. 다시 시도합니다."));
  std::filesystem::remove(target, ec);
  ec.clear();
  std::filesystem::rename(part, target, ec);
  if (ec) return fail(trf("파일을 쓰지 못했습니다: {0}", {homebrew::sdPath(app.fileName)}));
  return true;
}
} // namespace foil::net
