#pragma once
// System firmware the server offers (GET /api/firmware): each version as a
// zip of .nca files, unpacked into firmware/<version> on the SD card for
// Daybreak to install. The server keeps the list and the zips, so the list
// and its downloads stay when the place it takes them from goes away.
#include "core/ZipStream.hpp"
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace foil::firmware {
struct Archive {
  std::string url, sha256;
  uint64_t size = 0;
  uint64_t files = 0;    // entries in the zip
  uint64_t unpacked = 0; // their bytes on the card
  bool usable() const { return !url.empty() && size > 0; }
};
struct Version {
  std::string version;  // "23.0.1"
  std::string released; // "2026-10-01"
  bool available = false;
  std::string reason; // why not, as the server says it
  Archive archive;
};
struct List {
  bool reachable = true; // the server's source answered its last check
  std::string checkedAt;
  std::vector<Version> versions; // newest first
};
// "major.minor.micro", digits only: the only names a version folder takes.
bool isVersion(const std::string &text);
// <0, 0, >0 as a is older, the same, newer than b (by number: 9 < 10).
int compare(const std::string &a, const std::string &b);
// False when the body is not a list (an error page answered 200). Versions
// with a bad name are left out, the rest put newest first.
bool parse(const std::string &body, const std::string &serverUrl, List &out);
std::string serialize(const Version &version);
bool parseOne(const std::string &text, Version &out);
// Where a version lands on the card, and where it is written until whole.
std::string folder(const std::string &version);
std::string staging(const std::string &version);
// A file a firmware zip may hold: a bare *.nca name, nothing else.
bool isFirmwareEntry(const std::string &name);
// Every entry of the zip, each to the staging folder; false (with why) when
// any is not a firmware file or the count is not the server's.
bool pickEntries(const Version &version, const std::vector<ZipEntry> &inZip,
                 std::vector<std::pair<ZipEntry, std::string>> &wanted, std::string &why);
// The versions with a folder of .nca files under sd/firmware.
std::set<std::string> onCard(const std::string &sd);
} // namespace foil::firmware
