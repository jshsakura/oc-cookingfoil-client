#pragma once
#include "core/Installed.hpp"
namespace foil::platform {
Parsed<InstalledLibrary> readInstalled();
Parsed<std::vector<SaveEntry>> readSaves(uint64_t applicationId);
Error removeInstalled(const RemovalRequest &request);
// Finds (remove false) or deletes (remove true) content files on the SD card
// and console storage that no installed title uses, plus leftover install
// placeholders. Deleting scans again first, so only what is unused at that
// moment goes.
Parsed<CleanupReport> cleanOrphans(bool remove, CleanupProgress *progress = nullptr);
} // namespace foil::platform
