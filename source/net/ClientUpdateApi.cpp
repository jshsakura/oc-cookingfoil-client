#include "net/ClientUpdateApi.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
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
    reply.error.message = tr("업데이트를 확인하지 못했습니다.");
    return std::nullopt;
  }
  auto release = update::parseRelease(response.body, server.url);
  if (!release) reply.error.message = tr("서버의 업데이트 정보를 읽지 못했습니다.");
  return release;
}
bool installClient(const Server &server, const State &state, const update::Release &release,
                   const std::string &target, std::atomic<uint64_t> &received, Error &error,
                   const std::atomic<bool> *cancel) {
  const std::string part = target + ".new";
  std::error_code ec;
  makeDirectories(std::filesystem::path(target).parent_path().string());
  std::unique_ptr<FILE, int (*)(FILE *)> out(std::fopen(part.c_str(), "wb"), std::fclose);
  writeBuffer(out.get());
  if (!out) {
    error.message = trf("파일을 만들지 못했습니다: {0}", {part}) + " (" + std::strerror(errno) + ")";
    return false;
  }
  // Kept in memory as it arrives for the checksum; a build is a few MB.
  std::string bytes;
  bytes.reserve(size_t(release.size));
  bool writeFailed = false;
  const auto response = stream(release.url, authHeaders(server, state, release.url),
                               [&](const char *data, size_t size) {
                                 if (bytes.size() + size > release.size ||
                                     std::fwrite(data, 1, size, out.get()) != size) {
                                   writeFailed = true;
                                   return false;
                                 }
                                 bytes.append(data, size);
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
  if (response.status != 200) return fail(trf("업데이트를 받지 못했습니다. HTTP {0}", {std::to_string(response.status)}));
  if (writeFailed || !closed) return fail(tr("업데이트 파일을 쓰지 못했습니다."));
  if (bytes.size() != release.size || update::sha256Hex(bytes) != release.sha256)
    return fail(tr("받은 파일이 서버의 것과 다릅니다. 다시 시도합니다."));
  // Checked and complete: platform::replaceSelf puts it in place on the
  // main thread, where the app can let go of its own file.
  return true;
}
bool sendClientLog(const Server &server, const State &state, const std::string &log, const std::string &reason,
                   Error &error, const std::atomic<bool> *cancel) {
  const auto url = server.url + "/api/client/log";
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: text/plain; charset=utf-8");
  headers.push_back("X-Log-Reason: " + dropbox::percentEncode(reason.substr(0, 200)));
  PostOptions options;
  options.body = log;
  options.timeout = 30;
  const auto response = post(url, headers, options, cancel);
  if (!response.error.empty()) error.message = response.error;
  else if (response.status == 404) error.message = tr("서버가 오류 기록을 받지 않습니다.");
  else if (response.status < 200 || response.status >= 300)
    error.message = trf("오류 기록을 보내지 못했습니다. HTTP {0}", {std::to_string(response.status)});
  return !error;
}
} // namespace foil::net
