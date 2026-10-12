#pragma once
// Local save backups in oc-save-keeper's own folders and format (MIT,
// source/core/SaveManager.cpp), so a backup made by either app is listed and
// restored by the other. Paths take the keeper root (sdmc:/switch/oc-save-keeper
// on the console) so the host tests run on a plain folder.
#include "core/SaveMeta.hpp"
#include <array>
#include <functional>
#include <optional>
namespace foil::saves {
// save-keeper keeps 5 versions per game. Rollback copies made before a
// restore are counted on their own so they never push out a user's backup.
constexpr int MaxBackups = 5;
// Where the console keeps save-keeper's folders, given the SD root
// ("sdmc:" on the console, the preview's fake SD on the host).
std::string keeperRoot(const std::string &sd);
std::string titleHex(uint64_t titleId);
// save-keeper's user id: uid[1] then uid[0], 32 upper-case hex digits.
// "device" for the shared save every account uses.
std::string userIdOf(const std::array<uint64_t, 2> &user);
// Local time as YYYYmmdd_HHMMSS, save-keeper's backup folder name.
std::string backupStamp(std::time_t when);
struct LocalBackup {
  std::string path, name; // backups/<TID>/<name>, the folder with the files
  BackupMeta meta;
  bool hasMeta = false;
  std::time_t timestamp = 0; // created_at, else the folder's mtime
  int64_t size = 0;
};
// Which save a backup holds: user_id "device" marks the shared save, as
// save-keeper writes it. A backup without that tag (no .meta, or an older
// one) holds an account's.
bool deviceBackup(const LocalBackup &backup);
// A shared save's files go only into the shared save, an account's only
// into an account; otherwise the error says which kind they are for.
Error kindMismatch(bool deviceSource, bool deviceTarget);
Error restoreMismatch(const LocalBackup &backup, bool deviceTarget);
// Newest first, as save-keeper sorts them; backups made in the same second
// by their counted-up names.
std::vector<LocalBackup> listBackups(const std::string &root, uint64_t titleId);
// Just how many backup folders there are, without reading them.
int countBackups(const std::string &root, uint64_t titleId);
// The stamp, or the stamp with _2, _3 ... when that folder already exists
// (two backups in one second would otherwise share a folder).
std::string freshBackupName(const std::string &root, uint64_t titleId, std::time_t when);
// Moves the folder, its .meta and .zip to trash/<TID>/<name>_<epoch>, which
// save-keeper lists and restores from.
Error moveToTrash(const std::string &root, const LocalBackup &backup, std::time_t now);
// The backups past the limit: the oldest user backups beyond MaxBackups and,
// counted apart, the oldest rollback copies beyond MaxBackups.
std::vector<LocalBackup> overLimit(const std::vector<LocalBackup> &backups, int limit = MaxBackups);

struct Device {
  std::string id, label;
  int priority = 100;
};
// "dev-1A2B3C4D-0042" -> "1A2B3C4D", save-keeper's short label.
std::string deviceLabel(const std::string &id);
// Reads device_id and device_priority from config/settings.json (falling
// back to config/device_id.txt), creating them the way save-keeper does when
// missing. Other keys in settings.json are kept.
Parsed<Device> loadDevice(const std::string &root, std::time_t now, unsigned clockTicks);

// Copying save files. `commit` is set only when the destination is a console
// save; it then runs before `journal` bytes would be pending, after each file
// and after each folder made, as JKSV and save-keeper do, so the save never
// overflows its journal (folder and file entries use it too). Writing into a
// save also leaves out JKSV's own meta file.
struct CopyHooks {
  int64_t journal = 0;
  std::function<bool()> commit;
  std::function<void(uint64_t)> progress; // bytes copied so far
};
// True when writing `chunk` more bytes on top of `pending` uncommitted ones
// would reach the journal, so a commit has to come first (JKSV's test).
bool commitDue(int64_t pending, int64_t chunk, int64_t journal);

// Sizes a save is made or extended with.
struct SaveSizes {
  int64_t data = 0, journal = 0;
};
// save-keeper's journal when a game names none.
constexpr int64_t DefaultJournal = 0x1000000;
// A new save's sizes from the game's control data, as JKSV picks them: the
// size, else the maximum when the size is 0; the journal likewise, else
// save-keeper's default. data 0 means the game does not use this save.
SaveSizes creationSizes(int64_t size, int64_t sizeMax, int64_t journal, int64_t journalMax);
// JKSV's backup meta (fs/SaveMetaData.hpp): a packed record at the top of a
// JKSV backup. It never goes into a save; its sizes make a smaller save grow
// before the files go in, as JKSV's process_save_meta_data does.
constexpr const char *JksvMetaName = ".nx_save_meta.bin";
std::optional<SaveSizes> parseJksvMeta(const std::string &bytes);
std::optional<SaveSizes> readJksvMeta(const std::string &backupPath);
// The bytes a restore of `backupPath` writes into the save: its files
// without JKSV's meta.
int64_t restoreBytes(const std::string &backupPath);
// Whether `bytes` of files can go into a save of `room` usable bytes. More
// than that never fits, so the restore is turned down before anything is
// touched; 0 means the size could not be read and the copy finds out.
bool fitsSave(int64_t bytes, int64_t room);
int64_t directorySize(const std::string &path);
Error copyTree(const std::string &from, const std::string &to, const CopyHooks &hooks);
Error clearTree(const std::string &path);
Error removeTree(const std::string &path);
} // namespace foil::saves
