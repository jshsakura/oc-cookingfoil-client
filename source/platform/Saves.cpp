// Save mounting and journal handling follow oc-save-keeper (MIT),
// include/fs/ScopedSaveMount.hpp, which follows JKSV.
#include "platform/Saves.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include <cstdio>
#include <functional>
#include <ctime>
#include <memory>
#include <sys/stat.h>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
namespace {
// save-keeper's fallback when a game does not say.
constexpr int64_t DefaultJournal = 0x1000000;
#ifdef __SWITCH__
constexpr const char *MountName = "cfsave";
Error failure(const char *action, Result rc) {
  char code[20];
  std::snprintf(code, sizeof(code), "%08X", rc);
  return {trf("{0} 코드 {1}", {action, code})};
}
// The game's own journal size from its control data: a commit is due before
// that many bytes are written.
int64_t journalSize(const SaveEntry &save) {
  if (R_FAILED(nsInitialize())) return DefaultJournal;
  auto control = std::make_unique<NsApplicationControlData>();
  u64 size = 0;
  const auto rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, save.applicationId,
                                              control.get(), sizeof(*control), &size);
  nsExit();
  if (R_FAILED(rc) || size < sizeof(NacpStruct)) return DefaultJournal;
  const auto bytes = save.type == FsSaveDataType_Device ? control->nacp.device_save_data_journal_size
                                                        : control->nacp.user_account_save_data_journal_size;
  return bytes > 0 ? bytes : DefaultJournal;
}
// Mounts one save as "cfsave:" for as long as it lives.
class SaveMount {
public:
  explicit SaveMount(const SaveEntry &save) {
    fsdevUnmountDevice(MountName); // a mount left over from a crash
    FsSaveDataAttribute attr{};
    attr.application_id = save.applicationId;
    if (save.type == FsSaveDataType_Account) {
      attr.uid.uid[0] = save.user[0];
      attr.uid.uid[1] = save.user[1];
    }
    attr.save_data_type = save.type;
    attr.save_data_rank = FsSaveDataRank_Primary;
    attr.save_data_index = 0;
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
  auto error = failure(tr("세이브를 열지 못했습니다."), mount.rc());
  if (appletGetAppletType() != AppletType_Application)
    error.message += " " + std::string(tr("게임을 R 버튼을 누른 채 실행해 애플리케이션 모드에서 다시 시도합니다."));
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
    result.error.message = tr("백업 정보를 저장하지 못했습니다.");
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
  if (hooks.commit && !hooks.commit()) return {tr("세이브에 기록하지 못했습니다.")};
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
  if (!isDirectory(fakeSave(job))) return {{}, {tr("세이브 백업은 본체에서 사용할 수 있습니다.")}};
  return copyOut(job, fakeSave(job), rollback, keep);
#endif
}
} // namespace
Parsed<saves::LocalBackup> backupSave(const SaveJob &job, bool rollback) {
  return backupKeeping(job, rollback, "");
}
Parsed<saves::LocalBackup> importBackup(const SaveJob &job, const std::function<Error(const std::string &)> &fill,
                                        const std::string &source) {
  return makeBackup(job, fill, source, false, "");
}
Error restoreSave(const SaveJob &job, const saves::LocalBackup &backup) {
  struct stat st {};
  if (stat(backup.path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return {tr("백업 폴더를 찾지 못했습니다.")};
  // The copy to fall back on. Without it nothing is overwritten.
  auto rollback = backupKeeping(job, true, backup.path);
  if (rollback.error) return {trf("복원 전에 지금 세이브를 백업하지 못해 복원하지 않았습니다. {0}", {rollback.error.message})};
  if (job.copied) job.copied->store(0);
#ifdef __SWITCH__
  SaveMount mount(job.save);
  if (!mount.open()) return mountFailure(mount);
  saves::CopyHooks hooks = progressOnly(job);
  hooks.journal = journalSize(job.save);
  hooks.commit = [&mount] { return mount.commit(); };
  const auto target = mount.root();
#else
  saves::CopyHooks hooks = progressOnly(job);
  const auto target = fakeSave(job);
#endif
  auto error = copyIn(backup.path, target, hooks);
  if (!error) return {};
  logLine("saves: restore failed, putting the rollback back: " + error.message);
  if (auto again = copyIn(rollback.value.path, target, hooks)) {
    logLine("saves: rollback failed: " + again.message);
    return {trf("복원하지 못했고 원래 세이브도 되돌리지 못했습니다. {0} 백업에서 다시 복원합니다.", {rollback.value.name})};
  }
  return {trf("복원하지 못해 원래 세이브로 되돌렸습니다. {0}", {error.message})};
}
} // namespace foil::platform
