#include "net/ShopSavesApi.hpp"
#include "core/I18n.hpp"
#include "core/Patches.hpp"
#include "net/Api.hpp"
namespace foil::net {
Parsed<std::vector<shopsaves::ShopSave>> shopSaves(const Server &server, const State &state, ShopSavesReply &reply,
                                                    const std::atomic<bool> *cancel) {
  // Names and descriptions come in the app's language (Korean or English).
  const auto url = server.url + "/api/saves?lang=" + languageCode();
  const auto response = get(url, authHeaders(server, state, url), "", cancel);
  if (response.status == 404) {
    reply.missing = true;
    reply.error.message = tr("이 서버는 세이브를 제공하지 않습니다.");
  } else if (!response.error.empty() || response.status != 200) {
    reply.error.message = tr("서버 세이브 목록을 불러오지 못했습니다.");
  }
  if (reply.error) return {{}, reply.error};
  return {shopsaves::parse(response.body, server.url), {}};
}
bool downloadShopSave(const Server &server, const State &state, const shopsaves::ShopSave &save,
                      const std::string &dir, std::atomic<uint64_t> &received, Error &error,
                      const std::atomic<bool> *cancel) {
  // An app's save may only land below its switch/<app>/ folder.
  TarWriter writer(dir, [&save](const std::string &path) {
    return save.homebrewApp.empty() ? shopsaves::pathAllowed(path) : shopsaves::homebrewPathAllowed(save, path);
  });
  const auto response = stream(save.url, authHeaders(server, state, save.url),
                               [&](const char *data, size_t size) {
                                 received += size;
                                 return writer.feed(data, size);
                               },
                               cancel);
  if (!writer.error().empty()) { error.message = writer.error(); return false; }
  if (!response.error.empty()) { error.message = response.error; return false; }
  if (response.status != 200) {
    error.message = trf("세이브를 받지 못했습니다. HTTP {0}", {std::to_string(response.status)});
    return false;
  }
  if (!writer.finish()) { error.message = writer.error(); return false; }
  return true;
}
} // namespace foil::net
