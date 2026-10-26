#include "net/ClientStringsApi.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "core/I18n.hpp"
#include "core/Version.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
namespace foil::net {
std::optional<StringsReply> parseStringsReply(const std::string &json) {
  Error ignored;
  const auto root = parseJson(json, ignored);
  auto *strings = root ? field(root.get(), "strings") : nullptr;
  if (!json_object_is_type(strings, json_type_object)) return std::nullopt;
  StringsReply reply{stringField(root.get(), "lang"), stringField(root.get(), "base"), {}};
  if (reply.lang.empty()) return std::nullopt;
  json_object_object_foreach(strings, key, value) {
    if (json_object_is_type(value, json_type_string)) reply.strings[key] = json_object_get_string(value);
  }
  return reply;
}
std::map<std::string, std::string> serverStrings(DownloadStore &store, const Server *server, const State &state,
                                                 const std::string &lang, const std::atomic<bool> *cancel) {
  auto kept = store.clientStrings(lang);
  if (kept && kept->version != appVersion()) kept.reset(); // the server answered for another build's text
  std::string body = kept ? kept->body : "";
  if (server) {
    const auto url = server->url + "/api/client/strings?lang=" + dropbox::percentEncode(lang) +
                     "&v=" + dropbox::percentEncode(appVersion());
    const auto etag = kept && kept->server == server->url ? kept->etag : "";
    const auto reply = get(url, authHeaders(*server, state, url), etag, cancel);
    const auto fresh = reply.status == 200 ? parseStringsReply(reply.body) : std::nullopt;
    if (fresh && fresh->lang == lang) {
      body = reply.body;
      std::string error;
      if (!store.saveClientStrings(lang, {server->url, appVersion(), reply.etag, body}, error))
        logLine("strings: " + error);
    } else if (reply.status != 304) {
      logLine("strings: " + lang + " " + std::to_string(reply.status) + " " + reply.error);
    }
  }
  const auto parsed = parseStringsReply(body);
  return parsed && parsed->lang == lang ? parsed->strings : std::map<std::string, std::string>{};
}
bool downloadStrings(DownloadStore &store, const Server &server, const State &state, const std::string &lang,
                     StringsDownload &progress, StringsReply &reply, std::string &error,
                     const std::atomic<bool> *cancel) {
  constexpr uint64_t Most = 4 << 20; // a whole language is about 100KB
  const auto url = server.url + "/api/client/strings?lang=" + dropbox::percentEncode(lang) +
                   "&v=" + dropbox::percentEncode(appVersion());
  std::string body;
  const auto answer = stream(
      url, authHeaders(server, state, url),
      [&](const Response &head) {
        if (head.status != 200) return Start::Drop;
        progress.total = head.contentLength;
        return head.contentLength > Most ? Start::Stop : Start::Take;
      },
      [&](const char *data, size_t size) {
        body.append(data, size);
        progress.received += size;
        return body.size() <= Most;
      },
      cancel);
  if (!answer.error.empty() && answer.status != 200) {
    error = answer.error;
    return false;
  }
  if (answer.status != 200) {
    error = trf("Could not reach the server. HTTP {0}", {std::to_string(answer.status)});
    return false;
  }
  auto parsed = body.size() <= Most ? parseStringsReply(body) : std::nullopt;
  if (!answer.error.empty() || !parsed || parsed->lang != lang || parsed->strings.empty()) {
    error = answer.error.empty() ? tr("The server's answer could not be read.") : answer.error;
    return false;
  }
  std::string unsaved;
  if (!store.saveClientStrings(lang, {server.url, appVersion(), answer.etag, body}, unsaved))
    logLine("strings: " + unsaved);
  reply = std::move(*parsed);
  return true;
}
std::map<std::string, std::map<std::string, std::string>> keptStrings(DownloadStore &store) {
  std::map<std::string, std::map<std::string, std::string>> out;
  for (auto &[lang, row] : store.allClientStrings()) {
    if (row.version != appVersion()) continue;
    if (auto parsed = parseStringsReply(row.body); parsed && parsed->lang == lang) out[lang] = std::move(parsed->strings);
  }
  return out;
}
} // namespace foil::net
