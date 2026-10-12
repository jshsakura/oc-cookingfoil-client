#pragma once
// Backup metadata and sync conflict rules, ported from oc-save-keeper
// (MIT, include/core/MetadataLogic.hpp and SyncLogic.hpp). The key=value
// format and field names match save-keeper so either app reads the other's
// files.
#include "core/Types.hpp"
#include <cstdint>
#include <ctime>
#include <string>
namespace foil::saves {
struct BackupMeta {
  uint64_t titleId = 0;
  std::string titleName, backupName, revisionId, deviceId, deviceLabel, userId, userName, source;
  std::time_t createdAt = 0;
  int devicePriority = 100;
  int64_t size = 0;
  bool isAutoBackup = false, isFavorite = false;
};
// Escapes CR, LF and only those backslashes that would otherwise read back as
// an escape (\n, \r, \\). A value without such characters is written exactly
// as save-keeper writes it.
std::string escapeMetaValue(const std::string &value);
// Only \n, \r and \\ are special; any other backslash is kept as is, so a
// file save-keeper wrote without escaping parses the same.
std::string unescapeMetaValue(const std::string &value);
std::string serializeMeta(const BackupMeta &meta);
// Fails when neither title_id nor backup_name is present. Unknown keys and
// lines without '=' are ignored, CRLF is accepted.
Parsed<BackupMeta> parseMeta(const std::string &text);
Parsed<BackupMeta> readMetaFile(const std::string &path);
// Written atomically; a truthy Error means the file was not replaced.
Error writeMetaFile(const std::string &path, const BackupMeta &meta);
// Byte-for-byte copy.
Error copyMetaFile(const std::string &source, const std::string &destination);

enum class SyncPolicy { PreferPriority, PreferNewest, PreferLocalOnTie };
struct SyncDecision {
  bool useIncoming = false;
  std::string reason; // English, for the log
};
// save-keeper's rules: a different user is never taken automatically, then
// the higher device priority wins (PreferPriority), then the newer backup.
SyncDecision decideSync(SyncPolicy policy, const BackupMeta *local, const BackupMeta &incoming);
} // namespace foil::saves
