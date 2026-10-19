#pragma once
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "core/Installed.hpp"
namespace foil::platform {
Parsed<InstalledLibrary> readInstalled();
Parsed<std::vector<SaveEntry>> readSaves(uint64_t applicationId);
// Every game with a save on the console (an account's or the shared one),
// read from one pass over the save list.
Parsed<std::set<uint64_t>> savedApplications();
Error removeInstalled(const RemovalRequest &request);
// Closes this app and starts the installed game, as the HOME Menu would.
// Only as an application (not opened from the Album).
Error launchApplication(uint64_t applicationId);
// Finds (remove false) or deletes (remove true) content files on the SD card
// and console storage that no installed title uses, plus leftover install
// placeholders. Deleting scans again first, so only what is unused at that
// moment goes.
// `keep` names placeholders to leave alone ("sd:0123…", "nand:0123…"): a
// download cut off mid-file that picks up from them.
// For each "sd:0123…" or "nand:0123…" key, whether its install placeholder
// is there; a key whose storage could not be opened is left out.
std::map<std::string, bool> placeholdersThere(const std::vector<std::string> &keys);
// Deletes the install placeholder of each key that has one: the keys whose
// placeholder is gone now (deleted, or not there at all) and the bytes freed.
struct DroppedPlaceholders {
  std::vector<std::string> gone;
  uint64_t bytes = 0;
};
DroppedPlaceholders dropPlaceholders(const std::vector<std::string> &keys);
Parsed<CleanupReport> cleanOrphans(bool remove, CleanupProgress *progress = nullptr,
                                   const std::function<bool(const std::string &key)> &keep = nullptr);
} // namespace foil::platform
