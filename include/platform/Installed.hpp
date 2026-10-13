#pragma once
#include <functional>
#include <string>
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
Parsed<CleanupReport> cleanOrphans(bool remove, CleanupProgress *progress = nullptr,
                                   const std::function<bool(const std::string &key)> &keep = nullptr);
} // namespace foil::platform
