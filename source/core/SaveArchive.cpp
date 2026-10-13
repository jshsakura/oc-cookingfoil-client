// Ported from oc-save-keeper (MIT), source/core/SaveManager.cpp
// exportBackupArchive and importBackupArchive. Copyright (c) jshsakura.
#include "core/Json.hpp"
#include "core/SaveArchive.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#include "core/Zip.hpp"
#include <algorithm>
#include <filesystem>
namespace foil::saves {
#ifndef FOIL_NO_MINIZIP
namespace {
namespace fs = std::filesystem;
bool exists(const std::string &path) {
  std::error_code ec;
  return fs::exists(path, ec);
}
// A name that stays one folder under backups/<TID>.
bool plainName(const std::string &name) {
  return !name.empty() && name.size() <= 128 && name[0] != '.' && name.find("..") == std::string::npos &&
         name.find_first_of("/\\:") == std::string::npos &&
         std::none_of(name.begin(), name.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}
} // namespace
#endif

Error exportArchive(const LocalBackup &backup, const BackupMeta &fallback, const std::string &zipPath) {
#ifdef FOIL_NO_MINIZIP
  (void)backup, (void)fallback, (void)zipPath;
  return {tr("This build cannot zip saves.")};
#else
  if (!exists(backup.path)) return {tr("Could not find the backup folder.")};
  const auto metaPath = backup.path + ".meta";
  if (!exists(metaPath)) {
    if (auto error = writeMetaFile(metaPath, fallback)) return error;
  } else if (auto meta = readMetaFile(metaPath); !meta.error && meta.value.deviceId.empty() && !fallback.deviceId.empty()) {
    // Every upload says which console made it: Dropbox mixes all of them.
    meta.value.deviceId = fallback.deviceId;
    meta.value.deviceLabel = fallback.deviceLabel;
    meta.value.devicePriority = fallback.devicePriority;
    if (auto error = writeMetaFile(metaPath, meta.value)) return error;
  }
  std::error_code ec;
  makeDirectories(std::filesystem::path(fs::path(zipPath).parent_path()).string());
  ZipArchive zip;
  if (!zip.create(zipPath)) return {trf("Could not create the archive: {0}", {zipPath})};
  const bool added = zip.addDirectory(backup.path) && zip.addFile(metaPath, ArchiveMetaEntry);
  const bool closed = zip.close();
  if (added && closed) return {};
  std::remove(zipPath.c_str());
  logLine("saves: archive failed: " + zip.error());
  return {trf("Could not create the archive: {0}", {zipPath})};
#endif
}

Parsed<LocalBackup> importArchive(const std::string &root, uint64_t titleId, const std::string &zipPath,
                                  const std::string &name, std::time_t now) {
  Parsed<LocalBackup> result;
#ifdef FOIL_NO_MINIZIP
  (void)root, (void)titleId, (void)zipPath, (void)name, (void)now;
  result.error.message = tr("This build cannot zip saves.");
  return result;
#else
  const auto epoch = std::to_string(static_cast<long long>(now));
  const auto staging = root + "/temp/import_" + titleHex(titleId) + "_" + epoch;
  removeTree(staging);
  if (!unzipToDirectory(zipPath, staging)) {
    removeTree(staging);
    result.error.message = tr("Could not unpack the downloaded backup.");
    return result;
  }
  const auto stagedMeta = staging + "/" + ArchiveMetaEntry;
  auto incoming = readMetaFile(stagedMeta);
  std::remove(stagedMeta.c_str());
  const bool hasMeta = !incoming.error;
  // save-keeper names the import after the backup it was, else cloud_<now>.
  auto backupName = hasMeta && plainName(incoming.value.backupName) ? incoming.value.backupName
                    : plainName(name)                                ? name
                                                                     : "cloud_" + epoch;
  const auto folder = root + "/backups/" + titleHex(titleId);
  if (exists(folder + "/" + backupName)) backupName += "_" + epoch;
  const auto target = folder + "/" + backupName;
  std::error_code ec;
  makeDirectories(std::filesystem::path(folder).string());
  fs::rename(staging, target, ec);
  if (ec) {
    removeTree(staging);
    result.error.message = tr("Could not move the downloaded backup into the backups folder.");
    return result;
  }
  BackupMeta meta = hasMeta ? incoming.value : BackupMeta{};
  if (!hasMeta) {
    meta.titleId = titleId;
    meta.createdAt = now;
    meta.deviceId = "unknown";
    meta.deviceLabel = "Unknown device";
    meta.size = directorySize(target);
  }
  meta.backupName = meta.revisionId = backupName;
  meta.source = "cloud";
  meta.isAutoBackup = false;
  if (auto error = writeMetaFile(target + ".meta", meta)) logLine("saves: " + error.message);
  auto &backup = result.value;
  backup.path = target;
  backup.name = backupName;
  backup.meta = meta;
  backup.hasMeta = true;
  backup.timestamp = meta.createdAt ? meta.createdAt : now;
  backup.size = directorySize(target);
  return result;
#endif
}
} // namespace foil::saves
