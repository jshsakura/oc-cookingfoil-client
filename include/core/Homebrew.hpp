#pragma once
// Homebrew apps the server offers (feature: homebrew), saved to the SD card
// as sdmc:/switch/<name>/<name>.nro.
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
namespace foil::homebrew {
// One file of a folder install, saved at sdmc:/<path>.
struct File {
  std::string path, sha256, url; // sha256: 64 lowercase hex digits
  uint64_t size = 0;
};
// An app the server keeps as a zip, taken whole: each listed entry (its name
// in the zip) goes to its SD path; anything else in the zip stays out.
struct ArchiveEntry {
  std::string entry, path;
  uint64_t size = 0;
};
struct Archive {
  std::string format, url, sha256;
  uint64_t size = 0; // the zip's own size
  std::vector<ArchiveEntry> entries;
  bool usable() const { return format == "zip" && !url.empty() && size > 0 && !entries.empty(); }
};
// 도구 or 포트, the 홈브류 tab an app shows under.
enum class Kind { Tool, Port };
struct App {
  Kind kind = Kind::Tool;
  // The app's folder under switch/; its patches and saves name it.
  std::string app;
  std::string id, name, version, author, description, hash, fileName, iconUrl, downloadUrl, source, releasedAt,
      repoUrl;
  uint64_t size = 0; // 0 when the server does not know it yet
  // The HOME Menu shortcut the server built for it; empty when there is none.
  std::string forwarderUrl, forwarderTitleId;
  uint64_t forwarderSize = 0;
  // Where the .nro goes from the SD card root (switch/<app>/<file>.nro), and
  // for a folder install every file of it, the .nro included; empty files
  // means the .nro alone from downloadUrl.
  std::string installPath;
  std::vector<File> files;
  // A folder install's files as one tar stream (one request, resumable);
  // empty when the server has none, and the files come one by one.
  std::string bundleUrl;
  // The app as the server's zip (archive); format empty when there is none.
  Archive archive;
  // A file found on the SD card that no listed app owns: shown under 설치됨
  // with what the file says, never downloaded or updated, only removed.
  bool cardOnly = false;
};
// GET /api/homebrew's list; entries missing what is needed, or with a file
// name that could leave its folder, are left out.
std::vector<App> parse(const std::string &json, const std::string &base);
// One app in the list's shape, URLs absolute; parse() reads it back
// (kept with a 다운로드 entry so a failed one can start again).
std::string serialize(const App &app);
// What a folder app's bundle (ustar) carries: a 512-byte header and the data
// padded to 512 for each file, two closing blocks, plus one header for each
// folder; the progress of a bundle download is counted against it.
uint64_t bundleSize(const App &app);
// An app folder name: one path part of letters, digits, ". _ -" and spaces,
// no "..", not starting with a space or dot nor ending with one.
bool appFolderAllowed(const std::string &app);
// Whether an SD-root relative path lies below switch/<app>/, every part of
// it a plain name.
bool insideApp(const std::string &app, const std::string &path);
// The listed app whose folder (App::app) is `folder`, or nullptr; patches
// and saves name an app by its folder.
const App *findApp(const std::vector<App> &apps, const std::string &folder);
// The name shown for an app's patches and saves: the list's name, else the
// folder itself.
std::string appTitle(const std::vector<App> &apps, const std::string &folder);
// One key per game or app, so lists of patches and saves group by it: the
// Title ID, or "app:<folder>" for an app's (which has no Title ID).
std::string ownerKey(const std::string &titleId, const std::string &folder);
// Where an app goes, from the SD card root: switch/<stem>/<file name>.
std::string sdPath(const std::string &fileName);
// The download's ETag as 64 lowercase hex digits, or "" when it is not a
// SHA-256 ("abc", W/"...").
std::string etagSha256(const std::string &etag);
// What this console holds, kept in sdmc:/switch/cookingfoil/homebrew.json.
// The card is the truth and the ledger a cache of it: `size`, `mtime` and
// `fileSha` are the .nro as it was last seen, so a later look that finds the
// same size and time needs no hash.
struct CardFile {
  uint64_t size = 0;
  int64_t mtime = 0;
  bool operator==(const CardFile &other) const { return size == other.size && mtime == other.mtime; }
};
struct Record {
  std::string hash, version, file; // file: the .nro, from the SD card root
  std::vector<std::string> files;  // a folder install's other files, for removal
  CardFile seen;                   // size 0: never seen (a ledger from before)
  std::string fileSha;             // the .nro's own SHA-256, lowercase hex
  bool operator==(const Record &other) const {
    return hash == other.hash && version == other.version && file == other.file && files == other.files &&
           seen == other.seen && fileSha == other.fileSha;
  }
};
using Ledger = std::map<std::string, Record>; // by app id
Ledger parseLedger(const std::string &json);
std::string serializeLedger(const Ledger &ledger);
// The size and modified time of a file at a full path, or nothing.
std::optional<CardFile> cardFile(const std::string &fullPath);
// The ledger checked against the card. A file with the size and time it was
// seen with is taken as it is, unhashed. Otherwise it is hashed: the same
// bytes (only touched, or a ledger from before marks) keep the record and
// take the new marks; other bytes mean the card holds something else, and the
// record follows it: no build hash (it reads as an update) and the card's
// version. A file gone is only reported; status() already reads it so.
struct LedgerCheck {
  std::map<std::string, bool> onCard; // ledger file -> on the card
  Ledger fixed;                       // the records that change, by app id
  std::vector<std::string> followed;  // app ids whose card file differed
  size_t hashed = 0;
};
LedgerCheck checkLedger(const Ledger &ledger, const std::function<std::optional<CardFile>(const std::string &)> &look,
                        const std::function<std::string(const std::string &)> &sha256,
                        const std::function<std::string(const std::string &)> &cardVersion);
enum class Status { Available, Installed, Update };
// `exists` checks a path from the SD card root. Without a ledger entry on
// the card, `cardVersion` (the version of the app's file the card scan
// found, nullptr when none) decides: see cardStatus().
Status status(const App &app, const Ledger &ledger, const std::function<bool(const std::string &)> &exists,
              const std::string *cardVersion = nullptr);
} // namespace foil::homebrew
