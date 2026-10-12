#pragma once
// Save backup and restore on the console, in oc-save-keeper's folders.
// On the host a folder <sd>/fake-saves/<TID>/<user id> stands in for the
// mounted save so the preview can run the whole flow.
#include "core/Installed.hpp"
#include "core/SaveBackups.hpp"
#include <atomic>
#include <functional>
namespace foil::platform {
struct SaveJob {
  SaveEntry save;
  InstalledGame game;
  std::string sd;                          // "sdmc:" on the console
  std::atomic<uint64_t> *copied = nullptr; // bytes copied, for the progress line
};
// Copies the save into backups/<TID>/<stamp> with its .meta, then moves the
// oldest past save-keeper's limit to the trash. `rollback` marks the copy
// made before a restore (is_auto_backup), which has its own limit.
Parsed<saves::LocalBackup> backupSave(const SaveJob &job, bool rollback = false);
// A backup made from elsewhere (a shop save): `fill` writes the save's files
// into the new backup folder; the .meta records `source` and job's account.
Parsed<saves::LocalBackup> importBackup(const SaveJob &job, const std::function<Error(const std::string &)> &fill,
                                        const std::string &source);
// Backs the current save up first, then replaces it with `backup`; if that
// fails part way the rollback copy is put back.
Error restoreSave(const SaveJob &job, const saves::LocalBackup &backup);
} // namespace foil::platform
