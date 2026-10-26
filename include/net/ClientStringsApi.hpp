#pragma once
// Screen text from a server's dictionary (feature "client-strings"): for a
// language the app ships, the keys the server words differently from this
// version's built-in text. One row per language in cookingfoil.db keeps the
// last answer, so the text is there before any server is.
#include "core/DownloadStore.hpp"
#include "core/State.hpp"
#include <atomic>
#include <map>
#include <optional>
#include <string>
namespace foil::net {
// GET /api/client/strings: {"lang": "ko", "base": "zh" | null,
// "strings": {key: text}}. Values that are not text are left out; null when
// the reply is not that shape.
struct StringsReply {
  std::string lang, base;
  std::map<std::string, std::string> strings;
};
std::optional<StringsReply> parseStringsReply(const std::string &json);
// The texts for `lang`: the row kept for this version of the app, first
// asked again of `server` when there is one (a 304 keeps the row, a 200 for
// that language takes its place). Empty when there are none.
std::map<std::string, std::string> serverStrings(DownloadStore &store, const Server *server, const State &state,
                                                 const std::string &lang, const std::atomic<bool> *cancel);
// A language the app does not ship, got whole while a window shows how far
// it is (received of total bytes; total 0 until the server says). On a 200
// for that language the row is kept and `reply` filled; else `error` says
// why, in the current language. A body over a few MB is refused.
struct StringsDownload {
  std::atomic<uint64_t> received{0}, total{0};
};
bool downloadStrings(DownloadStore &store, const Server &server, const State &state, const std::string &lang,
                     StringsDownload &progress, StringsReply &reply, std::string &error,
                     const std::atomic<bool> *cancel);
// Every row kept for this version of the app, by language code: read once
// while the app loads, so a language picked later has its text at once.
std::map<std::string, std::map<std::string, std::string>> keptStrings(DownloadStore &store);
} // namespace foil::net
