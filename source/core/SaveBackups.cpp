// Folder rules ported from oc-save-keeper (MIT), source/core/SaveManager.cpp
// and source/fs/FileUtil.cpp. Copyright (c) jshsakura.
#include "core/SaveBackups.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>
namespace foil::saves {
namespace {
constexpr size_t CopyBuffer = 0x10000;
bool isDirectory(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool exists(const std::string &path) {
  struct stat st {};
  return stat(path.c_str(), &st) == 0;
}
// Every name in a folder but . and .., read before anything is changed.
std::vector<std::string> entries(const std::string &path) {
  std::vector<std::string> names;
  std::unique_ptr<DIR, int (*)(DIR *)> dir(opendir(path.c_str()), closedir);
  if (!dir) return names;
  while (auto *entry = readdir(dir.get()))
    if (std::strcmp(entry->d_name, ".") && std::strcmp(entry->d_name, ".."))
      names.push_back(entry->d_name);
  return names;
}
std::string readLine(const std::string &path) {
  auto text = readFile(path);
  text = text.substr(0, text.find_first_of("\r\n"));
  return text;
}
// A commit into a console save; nothing when the destination is the SD card.
Error commitNow(const CopyHooks &hooks, int64_t &pending, const std::string &path) {
  if (!hooks.commit) return {};
  if (!hooks.commit()) return {trf("Could not write to the save: {0}", {path})};
  pending = 0;
  return {};
}
Error copyFile(const std::string &from, const std::string &to, const CopyHooks &hooks,
               int64_t &pending, uint64_t &copied) {
  std::unique_ptr<FILE, int (*)(FILE *)> in(std::fopen(from.c_str(), "rb"), std::fclose);
  if (!in) return {trf("Could not read the file: {0}", {from})};
  std::unique_ptr<FILE, int (*)(FILE *)> out(std::fopen(to.c_str(), "wb"), std::fclose);
  writeBuffer(out.get());
  if (!out) return {trf("Could not create file: {0}", {to})};
  std::vector<char> buffer(CopyBuffer);
  long written = 0;
  for (;;) {
    const size_t got = std::fread(buffer.data(), 1, buffer.size(), in.get());
    if (!got) {
      if (std::ferror(in.get())) return {trf("Could not read the file: {0}", {from})};
      break;
    }
    // A console save holds only `journal` uncommitted bytes: close, commit
    // and reopen at the same place before going past it (JKSV's way).
    if (hooks.commit && commitDue(pending, int64_t(got), hooks.journal)) {
      if (std::fclose(out.release()) != 0 || !hooks.commit())
        return {trf("Could not write to the save: {0}", {to})};
      out.reset(std::fopen(to.c_str(), "rb+"));
      if (!out || std::fseek(out.get(), written, SEEK_SET) != 0)
        return {trf("Could not create file: {0}", {to})};
      pending = 0;
    }
    if (std::fwrite(buffer.data(), 1, got, out.get()) != got)
      return {trf("Could not write the file: {0}", {to})};
    written += long(got);
    pending += int64_t(got);
    copied += got;
    if (hooks.progress) hooks.progress(copied);
  }
  if (std::fclose(out.release()) != 0) return {trf("Could not write the file: {0}", {to})};
  return commitNow(hooks, pending, to);
}
Error copyInto(const std::string &from, const std::string &to, const CopyHooks &hooks,
               int64_t &pending, uint64_t &copied) {
  if (!makeDirectories(to)) return {trf("Could not create the folder: {0}", {to})};
  if (auto error = commitNow(hooks, pending, to)) return error;
  if (!isDirectory(from)) return {trf("Could not read the folder: {0}", {from})};
  for (const auto &name : entries(from)) {
    if (hooks.commit && name == JksvMetaName) continue;
    const auto source = from + "/" + name, target = to + "/" + name;
    struct stat st {};
    if (stat(source.c_str(), &st) != 0) return {trf("Could not read the file: {0}", {source})};
    Error error;
    if (S_ISDIR(st.st_mode)) error = copyInto(source, target, hooks, pending, copied);
    else if (S_ISREG(st.st_mode)) error = copyFile(source, target, hooks, pending, copied);
    if (error) return error;
  }
  return {};
}
} // namespace

std::string keeperRoot(const std::string &sd) { return sd + "/switch/oc-save-keeper"; }
std::string titleHex(uint64_t titleId) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(titleId));
  return text;
}
std::string userIdOf(const std::array<uint64_t, 2> &user) {
  if (!user[0] && !user[1]) return "device";
  return titleHex(user[1]) + titleHex(user[0]);
}
bool deviceBackup(const LocalBackup &backup) { return backup.hasMeta && backup.meta.userId == "device"; }
Error kindMismatch(bool deviceSource, bool deviceTarget) {
  if (deviceSource == deviceTarget) return {};
  return {deviceSource ? tr("This is for the shared save, so it can't go into an account's save. Pick the shared save.")
                       : tr("This is for an account's save, so it can't go into the shared save. Pick an account.")};
}
Error restoreMismatch(const LocalBackup &backup, bool deviceTarget) {
  return kindMismatch(deviceBackup(backup), deviceTarget);
}
std::string backupStamp(std::time_t when) {
  std::tm local {};
#ifdef _WIN32
  localtime_s(&local, &when);
#else
  localtime_r(&when, &local);
#endif
  char name[32];
  std::strftime(name, sizeof(name), "%Y%m%d_%H%M%S", &local);
  return name;
}

std::vector<LocalBackup> listBackups(const std::string &root, uint64_t titleId) {
  std::vector<LocalBackup> result;
  const auto folder = root + "/backups/" + titleHex(titleId);
  for (const auto &name : entries(folder)) {
    if (name[0] == '.') continue;
    LocalBackup backup;
    backup.name = name;
    backup.path = folder + "/" + name;
    struct stat st {};
    if (stat(backup.path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    backup.timestamp = st.st_mtime;
    backup.size = directorySize(backup.path);
    auto meta = readMetaFile(backup.path + ".meta");
    if (!meta.error) {
      backup.meta = std::move(meta.value);
      backup.hasMeta = true;
      if (backup.meta.createdAt) backup.timestamp = backup.meta.createdAt;
    }
    result.push_back(std::move(backup));
  }
  // Within one second freshBackupName counts up (_2, _3 ... _10), so the
  // longer, then larger, name is the newer one.
  std::sort(result.begin(), result.end(), [](const auto &a, const auto &b) {
    if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
    if (a.name.size() != b.name.size()) return a.name.size() > b.name.size();
    return a.name > b.name;
  });
  return result;
}
int countBackups(const std::string &root, uint64_t titleId) {
  const auto folder = root + "/backups/" + titleHex(titleId);
  int count = 0;
  for (const auto &name : entries(folder))
    if (name[0] != '.' && isDirectory(folder + "/" + name)) ++count;
  return count;
}
std::string freshBackupName(const std::string &root, uint64_t titleId, std::time_t when) {
  // One past the highest count already used in that second, so a name freed
  // by the trash is never reused (it would sort as the oldest).
  const auto stamp = backupStamp(when);
  int used = 0;
  for (auto name : entries(root + "/backups/" + titleHex(titleId))) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".meta") == 0) name.resize(name.size() - 5);
    if (name == stamp) used = std::max(used, 1);
    else if (name.rfind(stamp + "_", 0) == 0) used = std::max(used, std::atoi(name.c_str() + stamp.size() + 1));
  }
  return used ? stamp + "_" + std::to_string(used + 1) : stamp;
}
Error moveToTrash(const std::string &root, const LocalBackup &backup, std::time_t now) {
  const auto slash = backup.path.find_last_of('/');
  const auto parent = backup.path.substr(0, slash);
  const auto title = parent.substr(parent.find_last_of('/') + 1);
  const auto trash = root + "/trash/" + title;
  if (!makeDirectories(trash)) return {tr("Could not create the trash folder.")};
  const auto target = trash + "/" + backup.name + "_" + std::to_string(static_cast<long long>(now));
  if (std::rename(backup.path.c_str(), target.c_str()) != 0)
    return {tr("Could not move the backup to the trash.")};
  // The folder is what save-keeper lists; its companions follow it.
  if (exists(backup.path + ".meta") && std::rename((backup.path + ".meta").c_str(), (target + ".meta").c_str()) != 0)
    logLine("saves: could not move " + backup.path + ".meta to trash");
  if (exists(backup.path + ".zip") && std::rename((backup.path + ".zip").c_str(), (target + ".zip").c_str()) != 0)
    logLine("saves: could not move " + backup.path + ".zip to trash");
  return {};
}
std::vector<LocalBackup> overLimit(const std::vector<LocalBackup> &backups, int limit) {
  std::vector<LocalBackup> out;
  int user = 0, rollback = 0;
  for (const auto &b : backups) // newest first
    if (++(b.meta.isAutoBackup ? rollback : user) > limit) out.push_back(b);
  return out;
}

std::string deviceLabel(const std::string &id) {
  if (id.empty()) return "Unknown";
  const auto dash = id.find('-');
  const auto compact = dash == std::string::npos ? id : id.substr(dash + 1);
  return compact.substr(0, compact.find('-'));
}
Parsed<Device> loadDevice(const std::string &root, std::time_t now, unsigned clockTicks) {
  Parsed<Device> result;
  const auto config = root + "/config", settingsPath = config + "/settings.json";
  Error parseError;
  auto settings = parseJson(readFile(settingsPath), parseError);
  // save-keeper treats an unreadable settings.json as empty and rewrites it.
  if (!settings || !json_object_is_type(settings.get(), json_type_object))
    settings.reset(json_object_new_object());
  auto &device = result.value;
  device.id = stringField(settings.get(), "device_id");
  if (device.id.empty()) device.id = readLine(config + "/device_id.txt");
  bool changed = false;
  if (device.id.size() < 8) {
    char generated[24];
    std::snprintf(generated, sizeof(generated), "dev-%08X-%04X", static_cast<unsigned>(now & 0xFFFFFFFF),
                  clockTicks & 0xFFFF);
    device.id = generated;
    changed = true;
  }
  int priority = int(integerField(settings.get(), "device_priority", 0));
  if (priority <= 0) {
    const auto text = readLine(config + "/device_priority.txt");
    priority = text.empty() ? 100 : std::max(1, std::atoi(text.c_str()));
    changed = true;
  }
  device.priority = priority;
  device.label = deviceLabel(device.id);
  if (stringField(settings.get(), "device_id") != device.id) changed = true;
  if (!changed) return result;
  json_object_object_add(settings.get(), "device_id", json_object_new_string(device.id.c_str()));
  json_object_object_add(settings.get(), "device_priority", json_object_new_int(device.priority));
  std::string error;
  if (!makeDirectories(config) ||
      !atomicWrite(settingsPath,
                   std::string(json_object_to_json_string_ext(settings.get(), JSON_C_TO_STRING_PRETTY)) + "\n",
                   error))
    result.error.message = tr("Could not save the device details.");
  return result;
}

bool commitDue(int64_t pending, int64_t chunk, int64_t journal) {
  return journal > 0 && pending + chunk >= journal;
}
SaveSizes creationSizes(int64_t size, int64_t sizeMax, int64_t journal, int64_t journalMax) {
  SaveSizes sizes;
  sizes.data = size > 0 ? size : std::max<int64_t>(sizeMax, 0);
  sizes.journal = journal > 0 ? journal : journalMax > 0 ? journalMax : DefaultJournal;
  return sizes;
}
std::optional<SaveSizes> parseJksvMeta(const std::string &bytes) {
  // Packed little-endian: magic u32 at 0, then save_data_size and
  // journal_size as s64 at 61 and 69, 86 bytes in all.
  constexpr uint32_t Magic = 0x56534B4A;
  constexpr size_t DataAt = 61, JournalAt = 69, Size = 86;
  if (bytes.size() < Size) return std::nullopt;
  const auto number = [&](size_t at, size_t width) {
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value |= uint64_t(uint8_t(bytes[at + i])) << (8 * i);
    return value;
  };
  if (number(0, 4) != Magic) return std::nullopt;
  SaveSizes sizes{int64_t(number(DataAt, 8)), int64_t(number(JournalAt, 8))};
  if (sizes.data <= 0 || sizes.journal <= 0) return std::nullopt;
  return sizes;
}
std::optional<SaveSizes> readJksvMeta(const std::string &backupPath) {
  const auto path = backupPath + "/" + JksvMetaName;
  return exists(path) ? parseJksvMeta(readFile(path)) : std::nullopt;
}
int64_t restoreBytes(const std::string &backupPath) {
  struct stat st {};
  const auto meta = backupPath + "/" + JksvMetaName;
  const int64_t skipped = stat(meta.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? int64_t(st.st_size) : 0;
  return directorySize(backupPath) - skipped;
}
bool fitsSave(int64_t bytes, int64_t room) { return room <= 0 || bytes <= room; }
int64_t directorySize(const std::string &path) {
  int64_t total = 0;
  for (const auto &name : entries(path)) {
    const auto full = path + "/" + name;
    struct stat st {};
    if (stat(full.c_str(), &st) != 0) continue;
    total += S_ISDIR(st.st_mode) ? directorySize(full) : int64_t(st.st_size);
  }
  return total;
}
Error copyTree(const std::string &from, const std::string &to, const CopyHooks &hooks) {
  int64_t pending = 0;
  uint64_t copied = 0;
  auto error = copyInto(from, to, hooks, pending, copied);
  if (!error && hooks.commit && !hooks.commit()) error.message = tr("Could not write to the save.");
  return error;
}
Error clearTree(const std::string &path) {
  for (const auto &name : entries(path)) {
    const auto full = path + "/" + name;
    if (isDirectory(full)) {
      if (auto error = removeTree(full)) return error;
    } else if (unlink(full.c_str()) != 0) {
      return {trf("Could not delete the file: {0}", {full})};
    }
  }
  return {};
}
Error removeTree(const std::string &path) {
  if (auto error = clearTree(path)) return error;
  if (rmdir(path.c_str()) != 0) return {trf("Could not delete the folder: {0}", {path})};
  return {};
}
} // namespace foil::saves
