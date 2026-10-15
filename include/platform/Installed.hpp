#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "core/Installed.hpp"
namespace foil::platform {
Parsed<InstalledLibrary> readInstalled();
Parsed<std::vector<SaveEntry>> readSaves(uint64_t applicationId);
Error removeInstalled(const RemovalRequest &request);
// Finds (remove false) or deletes (remove true) content files on the SD card
// and console storage that no installed title uses, plus leftover install
// placeholders. Deleting scans again first, so only what is unused at that
// moment goes.
// `keep` names placeholders to leave alone ("sd:0123…", "nand:0123…"): a
// download cut off mid-file that picks up from them.
// For each "sd:0123…" or "nand:0123…" key, whether its install placeholder
// is there; a key whose storage could not be opened is left out.
std::map<std::string, bool> placeholdersThere(const std::vector<std::string> &keys);
Parsed<CleanupReport> cleanOrphans(bool remove, CleanupProgress *progress = nullptr,
                                   const std::function<bool(const std::string &key)> &keep = nullptr);
} // namespace foil::platform
