#pragma once
// Homebrew apps the server offers (feature: homebrew), saved to the SD card
// as sdmc:/switch/<name>/<name>.nro.
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
namespace foil::homebrew {
struct App {
  std::string id, name, version, author, description, hash, fileName, iconUrl, downloadUrl, source, releasedAt,
      repoUrl;
  uint64_t size = 0; // 0 when the server does not know it yet
  // The HOME Menu shortcut the server built for it; empty when there is none.
  std::string forwarderUrl, forwarderTitleId;
  uint64_t forwarderSize = 0;
};
// GET /api/homebrew's list; entries missing what is needed, or with a file
// name that could leave its folder, are left out.
std::vector<App> parse(const std::string &json, const std::string &base);
// One app in the list's shape, URLs absolute; parse() reads it back
// (kept with a 다운로드 entry so a failed one can start again).
std::string serialize(const App &app);
// Where an app goes, from the SD card root: switch/<stem>/<file name>.
std::string sdPath(const std::string &fileName);
// The download's ETag as 64 lowercase hex digits, or "" when it is not a
// SHA-256 ("abc", W/"...").
std::string etagSha256(const std::string &etag);
// What this console holds, kept in sdmc:/switch/cookingfoil/homebrew.json.
struct Record {
  std::string hash, version, file; // file: from the SD card root
};
using Ledger = std::map<std::string, Record>; // by app id
Ledger parseLedger(const std::string &json);
std::string serializeLedger(const Ledger &ledger);
enum class Status { Available, Installed, Update };
// `exists` checks a path from the SD card root.
Status status(const App &app, const Ledger &ledger, const std::function<bool(const std::string &)> &exists);
} // namespace foil::homebrew
