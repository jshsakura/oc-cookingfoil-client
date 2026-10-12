#pragma once
// A backup as one zip, the way oc-save-keeper sends it to Dropbox (MIT,
// SaveManager::exportBackupArchive and importBackupArchive): the backup
// folder's files, plus its .meta stored as .dropkeep.meta.
#include "core/SaveBackups.hpp"
namespace foil::saves {
constexpr const char *ArchiveMetaEntry = ".dropkeep.meta";
// Writes zipPath from `backup`. A backup without a .meta gets `fallback`
// written next to it first, as save-keeper does before exporting; a .meta
// without a device_id gets fallback's device, so each upload names its console.
Error exportArchive(const LocalBackup &backup, const BackupMeta &fallback, const std::string &zipPath);
// Unpacks zipPath into backups/<TID>/<name>, next to save-keeper's own:
// the name is the archive's backup_name, else `name`, with _<now> added when
// that folder is taken. Its .meta is the archive's, marked source "cloud"
// (save-keeper's import), or a new one when the archive has none. Nothing
// is restored here; the backup is then restored like any other.
Parsed<LocalBackup> importArchive(const std::string &root, uint64_t titleId, const std::string &zipPath,
                                  const std::string &name, std::time_t now);
} // namespace foil::saves
