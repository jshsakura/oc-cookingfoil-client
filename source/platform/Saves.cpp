// Save mounting and journal handling follow oc-save-keeper (MIT),
// include/fs/ScopedSaveMount.hpp, which follows JKSV; creating, sizing and
// growing a save follow JKSV (fs/save_data_functions.cpp, fs/SaveMetaData.cpp).
#include "platform/Saves.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <ctime>
#include <memory>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
namespace {
#ifdef __SWITCH__
constexpr const char *MountName = "cfsave";
Error failure(const char *action, Result rc) {
  char code[20];
  std::snprintf(code, sizeof(code), "%08X", rc);
  return {trf("{0} Code {1}", {action, code})};
}
FsSaveDataAttribute attributeOf(const SaveEntry &save) {
  FsSaveDataAttribute attr{};
  attr.application_id = save.applicationId;
  if (save.type == FsSaveDataType_Account) {
    attr.uid.uid[0] = save.user[0];
    attr.uid.uid[1] = save.user[1];
  }
  attr.save_data_type = save.type;
  attr.save_data_rank = FsSaveDataRank_Primary;
  attr.save_data_index = 0;
  return attr;
}
struct GameSave {
  saves::SaveSizes sizes;
  uint64_t owner = 0;
};
// The save's sizes and owner from the game's control data, as JKSV reads
// them for a new save.
Parsed<GameSave> gameSave(const SaveEntry &save) {
  if (R_FAILED(nsInitialize())) return {{}, {tr("Could not read the game's data to create a save.")}};
  auto control = std::make_unique<NsApplicationControlData>();
  u64 size = 0;
  const auto rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, save.applicationId, control.get(),
                                              sizeof(*control), &size);
  nsExit();
  if (R_FAILED(rc) || size < sizeof(NacpStruct))
    return {{}, failure(tr("Could not read the game's data to create a save."), rc)};
  const auto &nacp = control->nacp;
  GameSave game;
  game.sizes = save.type == FsSaveDataType_Device
                   ? saves::creationSizes(nacp.device_save_data_size, nacp.device_save_data_size_max,
                                          nacp.device_save_data_journal_size, nacp.device_save_data_journal_size_max)
                   : saves::creationSizes(nacp.user_account_save_data_size, nacp.user_account_save_data_size_max,
                                          nacp.user_account_save_data_journal_size,
                                          nacp.user_account_save_data_journal_size_max);
  game.owner = nacp.save_data_owner_id ? nacp.save_data_owner_id : save.applicationId;
  return {game, {}};
}
// A save the game has not made yet for this account (or the shared one),
// created the way JKSV does. A JKSV backup's meta may ask for more room.
Parsed<saves::SaveSizes> createSave(const SaveEntry &save, const std::optional<saves::SaveSizes> &wanted,
                                    int64_t bytes) {
  auto game = gameSave(save);
  if (game.error) return {{}, game.error};
  auto sizes = game.value.sizes;
  if (sizes.data <= 0) return {{}, {tr("This game does not use this kind of save.")}};
  if (wanted) {
    sizes.data = std::max(sizes.data, wanted->data);
    sizes.journal = std::max(sizes.journal, wanted->journal);
  }
  if (!saves::fitsSave(bytes, sizes.data)) return {{}, {tr("The backup is larger than the save can hold, so it was not restored.")}};
  const auto attr = attributeOf(save);
  FsSaveDataCreationInfo info{};
  info.save_data_size = sizes.data;
  info.journal_size = sizes.journal;
  info.available_size = 0x4000;
  info.owner_id = game.value.owner;
  info.flags = 0;
  info.save_data_space_id = FsSaveDataSpaceId_User;
  FsSaveDataMetaInfo meta{};
  meta.size = 0x40060;
  meta.type = FsSaveDataMetaType_Thumbnail;
  const auto created = fsCreateSaveDataFileSystem(&attr, &info, &meta);
  if (R_FAILED(created)) return {{}, failure(tr("Could not create the save."), created)};
  return {sizes, {}};
}
// An existing save's own sizes (JKSV takes the journal from here, not from
// the game), grown first when a JKSV backup's meta asks for more room.
Parsed<saves::SaveSizes> existingSave(const SaveEntry &save, const std::optional<saves::SaveSizes> &wanted,
                                      int64_t bytes) {
  FsSaveDataExtraData extra{};
  const auto rc = fsReadSaveDataFileSystemExtraDataBySaveDataSpaceId(&extra, sizeof(extra), FsSaveDataSpaceId_User,
                                                                     save.saveId);
  if (R_FAILED(rc)) {
    // Size unknown: commit at the game's journal, as JKSV falls back to.
    auto game = gameSave(save);
    return {{0, game.error ? saves::DefaultJournal : game.value.sizes.journal}, {}};
  }
  saves::SaveSizes sizes{extra.data_size, extra.journal_size > 0 ? extra.journal_size : saves::DefaultJournal};
  if (wanted && wanted->data > sizes.data) {
    const auto grown = fsExtendSaveDataFileSystem(FsSaveDataSpaceId_User, save.saveId, wanted->data, wanted->journal);
    if (R_FAILED(grown)) return {{}, failure(tr("Could not make the save larger."), grown)};
    sizes = *wanted;
  }
  if (!saves::fitsSave(bytes, sizes.data)) return {{}, {tr("The backup is larger than the save can hold, so it was not restored.")}};
  return {sizes, {}};
}
// A save made for a restore that then failed goes again, so the game does
// not open half a save.
void deleteSave(const SaveEntry &save) {
  const auto attr = attributeOf(save);
  const auto rc = fsDeleteSaveDataFileSystemBySaveDataAttribute(FsSaveDataSpaceId_User, &attr);
  if (R_FAILED(rc)) logLine("saves: could not remove the new save after a failed restore");
}
// Mounts one save as "cfsave:" for as long as it lives.
class SaveMount {
public:
  explicit SaveMount(const SaveEntry &save) {
    fsdevUnmountDevice(MountName); // a mount left over from a crash
    const auto attr = attributeOf(save);
    FsFileSystem fs{};
    rc_ = fsOpenSaveDataFileSystem(&fs, FsSaveDataSpaceId_User, &attr);
    if (R_FAILED(rc_)) return;
    if (fsdevMountDevice(MountName, fs) == -1) {
      fsFsClose(&fs);
      rc_ = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
      return;
    }
    open_ = true;
  }
  ~SaveMount() {
    if (open_) fsdevUnmountDevice(MountName);
  }
  SaveMount(const SaveMount &) = delete;
  SaveMount &operator=(const SaveMount &) = delete;
  bool open() const { return open_; }
  Result rc() const { return rc_; }
  std::string root() const { return std::string(MountName) + ":"; }
  bool commit() const { return R_SUCCEEDED(fsdevCommitDevice(MountName)); }

private:
  bool open_ = false;
  Result rc_ = 0;
};
Error mountFailure(const SaveMount &mount) {
  auto error = failure(tr("Could not open the save."), mount.rc());
  if (appletGetAppletType() != AppletType_Application)
    error.message += " " + std::string(tr("Launch a game while holding R and try again in application mode."));
  return error;
}
#else
// The preview's stand-in for a mounted save.
std::string fakeSave(const SaveJob &job) {
  return job.sd + "/fake-saves/" + saves::titleHex(job.save.applicationId) + "/" + saves::userIdOf(job.save.user);
}
bool isDirectory(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
#endif
saves::CopyHooks progressOnly(const SaveJob &job) {
  saves::CopyHooks hooks;
  if (job.copied) hooks.progress = [copied = job.copied](uint64_t bytes) { copied->store(bytes); };
  return hooks;
}
// A new backup folder filled by `fill`, then its .meta written and the
// oldest past the limit moved to the trash. `keep` is never trashed: the
// backup about to be restored.
Parsed<saves::LocalBackup> makeBackup(const SaveJob &job, const std::function<Error(const std::string &)> &fill,
                                      const std::string &source, bool rollback, const std::string &keep) {
  Parsed<saves::LocalBackup> result;
  const auto root = saves::keeperRoot(job.sd);
  auto device = saves::loadDevice(root, std::time(nullptr), unsigned(std::clock()));
  if (device.error) logLine("saves: " + device.error.message);
  const auto now = std::time(nullptr);
  const auto name = saves::freshBackupName(root, job.save.applicationId, now);
  const auto folder = root + "/backups/" + saves::titleHex(job.save.applicationId);
  const auto path = folder + "/" + name;
  if (auto error = fill(path)) {
    saves::removeTree(path);
    result.error = error;
    return result;
  }
  saves::BackupMeta meta;
  meta.titleId = job.save.applicationId;
  meta.titleName = job.game.name;
  meta.backupName = meta.revisionId = name;
  meta.deviceId = device.value.id;
  meta.deviceLabel = device.value.label;
  meta.devicePriority = device.value.priority;
  meta.userId = saves::userIdOf(job.save.user);
  meta.userName = job.save.userName;
  meta.source = source;
  meta.createdAt = now;
  meta.size = saves::directorySize(path);
  meta.isAutoBackup = rollback;
  if (auto error = saves::writeMetaFile(path + ".meta", meta)) {
    saves::removeTree(path);
    result.error.message = tr("Could not save the backup details.");
    logLine("saves: " + error.message);
    return result;
  }
  auto &backup = result.value;
  backup.path = path;
  backup.name = name;
  backup.meta = meta;
  backup.hasMeta = true;
  backup.timestamp = now;
  backup.size = meta.size;
  for (const auto &old : saves::overLimit(saves::listBackups(root, job.save.applicationId)))
    if (old.path != keep)
      if (auto error = saves::moveToTrash(root, old, now)) logLine("saves: " + error.message);
  return result;
}
// Copies the save at `source` (already mounted) into a new backup folder.
Parsed<saves::LocalBackup> copyOut(const SaveJob &job, const std::string &source, bool rollback,
                                   const std::string &keep = "") {
  return makeBackup(
      job, [&](const std::string &path) { return saves::copyTree(source, path, progressOnly(job)); }, "local",
      rollback, keep);
}
// Replaces what is at `target` with `source`, committing as it goes.
Error copyIn(const std::string &source, const std::string &target, const saves::CopyHooks &hooks) {
  if (auto error = saves::clearTree(target)) return error;
  if (hooks.commit && !hooks.commit()) return {tr("Could not write to the save.")};
  return saves::copyTree(source, target, hooks);
}
} // namespace

namespace {
Parsed<saves::LocalBackup> backupKeeping(const SaveJob &job, bool rollback, const std::string &keep) {
#ifdef __SWITCH__
  SaveMount mount(job.save);
  if (!mount.open()) return {{}, mountFailure(mount)};
  return copyOut(job, mount.root(), rollback, keep);
#else
  if (!isDirectory(fakeSave(job))) return {{}, {tr("Save backups work on the console.")}};
  return copyOut(job, fakeSave(job), rollback, keep);
#endif
}
} // namespace
Parsed<saves::LocalBackup> backupSave(const SaveJob &job, bool rollback) {
  if (job.save.missing) return {{}, {tr("There is no save yet, so nothing to back up.")}};
  return backupKeeping(job, rollback, "");
}
Parsed<saves::LocalBackup> importBackup(const SaveJob &job, const std::function<Error(const std::string &)> &fill,
                                        const std::string &source) {
  return makeBackup(job, fill, source, false, "");
}
Error restoreSave(const SaveJob &job, const saves::LocalBackup &backup) {
  struct stat st {};
  if (stat(backup.path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return {tr("Could not find the backup folder.")};
  if (auto error = saves::restoreMismatch(backup, job.save.device())) return error;
  // An account without a save gets one first; there is nothing to fall
  // back on then. Otherwise the copy to fall back on comes first, and
  // without it nothing is overwritten. A backup the save cannot hold is
  // turned down before either.
  Parsed<saves::LocalBackup> rollback;
  saves::CopyHooks hooks = progressOnly(job);
#ifdef __SWITCH__
  const auto wanted = saves::readJksvMeta(backup.path);
  const auto bytes = saves::restoreBytes(backup.path);
  auto sizes = job.save.missing ? createSave(job.save, wanted, bytes) : existingSave(job.save, wanted, bytes);
  if (sizes.error) return sizes.error;
  hooks.journal = sizes.value.journal;
#else
  if (job.save.missing) {
    std::error_code ec;
    std::filesystem::create_directories(fakeSave(job), ec);
  }
#endif
  if (!job.save.missing) {
    rollback = backupKeeping(job, true, backup.path);
    if (rollback.error) return {trf("Did not restore because the current save could not be backed up first. {0}", {rollback.error.message})};
  }
  if (job.copied) job.copied->store(0);
  Error error;
  {
#ifdef __SWITCH__
    SaveMount mount(job.save);
    if (!mount.open()) {
      if (job.save.missing) deleteSave(job.save);
      return mountFailure(mount);
    }
    hooks.commit = [&mount] { return mount.commit(); };
    const auto target = mount.root();
#else
    hooks.commit = [] { return true; }; // the fake save stands in for a console one
    const auto target = fakeSave(job);
#endif
    error = copyIn(backup.path, target, hooks);
    if (!error) return {};
    if (!job.save.missing) {
      logLine("saves: restore failed, putting the rollback back: " + error.message);
      if (auto again = copyIn(rollback.value.path, target, hooks)) {
        logLine("saves: rollback failed: " + again.message);
        return {trf("The restore failed and the original save could not be put back. Restore again from backup {0}.", {rollback.value.name})};
      }
      return {trf("The restore failed, so the original save was put back. {0}", {error.message})};
    }
  }
  // The save made for this restore goes again once it is unmounted.
#ifdef __SWITCH__
  deleteSave(job.save);
#endif
  return {trf("Could not restore into the new save. {0}", {error.message})};
}
} // namespace foil::platform
