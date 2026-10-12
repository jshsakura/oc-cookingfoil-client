// Metadata and sync tests ported from oc-save-keeper (MIT):
// test_sync_logic, test_metadata_logic, test_metadata_file_logic.
#include "Test.hpp"
#include "core/SaveMeta.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>
using foil::saves::BackupMeta;
using foil::saves::SyncPolicy;
namespace {
std::string tempPath(const char *tag) {
  static std::atomic<int> counter{0};
  return (std::filesystem::temp_directory_path() /
          ("foil_meta_" + std::to_string(getpid()) + "_" + std::to_string(counter++) + "_" + tag))
      .string();
}
void writeRaw(const std::string &path, const std::string &text) {
  std::ofstream(path, std::ios::binary) << text;
}
std::string readRaw(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
BackupMeta syncMeta(const char *user, const char *device, int priority, std::time_t createdAt) {
  BackupMeta meta;
  meta.userId = user;
  meta.deviceId = device;
  meta.devicePriority = priority;
  meta.createdAt = createdAt;
  return meta;
}
BackupMeta fullMeta() {
  BackupMeta meta;
  meta.titleId = 0x01006A800016E000ULL;
  meta.titleName = "The Legend of Zelda: BOTW";
  meta.backupName = "20260311_090000";
  meta.revisionId = "rev-42";
  meta.deviceId = "dev-abc";
  meta.deviceLabel = "OLED Main";
  meta.userId = "user-1";
  meta.userName = "TestUser";
  meta.source = "cloud";
  meta.createdAt = static_cast<std::time_t>(1710000000);
  meta.devicePriority = 200;
  meta.size = 123456;
  return meta;
}
int syncTests() {
  int failures = 0;
  using foil::saves::decideSync;
  {
    const auto local = syncMeta("user-a", "device-a", 100, 1000);
    const auto incoming = syncMeta("user-b", "device-b", 200, 2000);
    const auto d = decideSync(SyncPolicy::PreferPriority, &local, incoming);
    CHECK(!d.useIncoming);
    CHECK(d.reason == "Different user save detected; manual restore only");
  }
  {
    const auto local = syncMeta("user-a", "device-a", 100, 3000);
    const auto incoming = syncMeta("user-a", "device-b", 200, 1000);
    const auto d = decideSync(SyncPolicy::PreferPriority, &local, incoming);
    CHECK(d.useIncoming);
    CHECK(d.reason == "Different device save accepted by higher priority");
  }
  {
    const auto local = syncMeta("user-a", "device-a", 100, 1000);
    const auto incoming = syncMeta("user-a", "device-a", 100, 2000);
    const auto d = decideSync(SyncPolicy::PreferPriority, &local, incoming);
    CHECK(d.useIncoming);
    CHECK(d.reason == "Incoming backup is newer");
  }
  {
    const auto local = syncMeta("user-a", "device-a", 100, 1000);
    const auto incoming = syncMeta("user-a", "device-a", 100, 1000);
    const auto d = decideSync(SyncPolicy::PreferNewest, &local, incoming);
    CHECK(!d.useIncoming);
    CHECK(d.reason == "Timestamps match; keeping local backup");
    CHECK(!decideSync(SyncPolicy::PreferLocalOnTie, &local, incoming).useIncoming);
  }
  {
    // Newest policy ignores priority.
    const auto local = syncMeta("user-a", "device-a", 900, 1000);
    const auto incoming = syncMeta("user-a", "device-b", 100, 2000);
    CHECK(decideSync(SyncPolicy::PreferNewest, &local, incoming).useIncoming);
    CHECK(!decideSync(SyncPolicy::PreferPriority, &local, incoming).useIncoming);
  }
  CHECK(decideSync(SyncPolicy::PreferPriority, nullptr, syncMeta("u", "d", 0, 0)).useIncoming);
  return failures;
}
int metadataTests() {
  int failures = 0;
  using foil::saves::parseMeta;
  using foil::saves::serializeMeta;
  {
    const auto input = fullMeta();
    const auto parsed = parseMeta(serializeMeta(input));
    CHECK(!parsed.error);
    const auto &p = parsed.value;
    CHECK(p.titleId == input.titleId && p.titleName == input.titleName && p.backupName == input.backupName);
    CHECK(p.revisionId == input.revisionId && p.deviceId == input.deviceId && p.deviceLabel == input.deviceLabel);
    CHECK(p.userId == input.userId && p.userName == input.userName && p.source == input.source);
    CHECK(p.createdAt == input.createdAt && p.devicePriority == input.devicePriority && p.size == input.size);
  }
  {
    const auto parsed = parseMeta("title_id=42\nbad_line_without_delimiter\nbackup_name=slot-a\n"
                                  "unknown_field=ignore-me\nuser_id=user-1\n");
    CHECK(!parsed.error);
    CHECK(parsed.value.titleId == 42 && parsed.value.backupName == "slot-a" && parsed.value.userId == "user-1");
  }
  CHECK(parseMeta("title_name=No identity\ndevice_id=dev-x\n").error);
  {
    const auto parsed = parseMeta("backup_name=legacy-slot\n");
    CHECK(!parsed.error && parsed.value.titleId == 0 && parsed.value.backupName == "legacy-slot");
  }
  {
    auto input = fullMeta();
    input.backupName = "auto_pre_restore_20260311_100000";
    input.isAutoBackup = true;
    CHECK(parseMeta(serializeMeta(input)).value.isAutoBackup);
  }
  CHECK(!parseMeta("title_id=42\nbackup_name=manual-backup\n").value.isAutoBackup);
  CHECK(parseMeta("backup_name=test\nis_auto_backup=1\n").value.isAutoBackup);
  CHECK(parseMeta("backup_name=test\nisAutoBackup=true\n").value.isAutoBackup);
  CHECK(!parseMeta("backup_name=test\nis_auto_backup=0\n").value.isAutoBackup);
  CHECK(!parseMeta("title_id=42\nbackup_name=manual-backup\n").value.isFavorite);
  CHECK(parseMeta("backup_name=test\nis_favorite=1\n").value.isFavorite);
  CHECK(!parseMeta("backup_name=test\nis_favorite=0\n").value.isFavorite);
  CHECK(parseMeta("backup_name=test\nisFavorite=true\n").value.isFavorite);
  {
    BackupMeta input;
    input.titleId = 0x01006A800016E000ULL;
    input.backupName = "favorite-backup";
    input.isFavorite = true;
    CHECK(parseMeta(serializeMeta(input)).value.isFavorite);
    CHECK(serializeMeta(input).find("is_favorite=1") != std::string::npos);
  }
  CHECK(parseMeta("").value.devicePriority == 100);
  return failures;
}
int fileTests() {
  int failures = 0;
  using namespace foil::saves;
  {
    const auto path = tempPath("roundtrip");
    const auto input = fullMeta();
    CHECK(!writeMetaFile(path, input));
    const auto parsed = readMetaFile(path);
    CHECK(!parsed.error);
    CHECK(parsed.value.titleId == input.titleId && parsed.value.backupName == input.backupName);
    CHECK(parsed.value.devicePriority == input.devicePriority && parsed.value.size == input.size);
    std::remove(path.c_str());
  }
  CHECK(readMetaFile(tempPath("missing")).error);
  {
    const auto path = tempPath("crlf");
    writeRaw(path, "title_id=42\r\nbroken_line\r\nbackup_name=slot-crlf\r\nuser_id=user-1\r\n");
    const auto parsed = readMetaFile(path);
    CHECK(!parsed.error);
    CHECK(parsed.value.titleId == 42 && parsed.value.backupName == "slot-crlf" && parsed.value.userId == "user-1");
    std::remove(path.c_str());
  }
  CHECK(writeMetaFile("/proc/foil-no-such-dir/x.meta", fullMeta()));
  {
    const auto source = tempPath("copy-src"), destination = tempPath("copy-dst");
    const std::string input = "title_id=42\nbackup_name=slot-a\r\nuser_id=user-a\n";
    writeRaw(source, input);
    CHECK(!copyMetaFile(source, destination));
    CHECK(readRaw(destination) == input);
    std::remove(source.c_str());
    std::remove(destination.c_str());
  }
  CHECK(copyMetaFile(tempPath("missing-src"), tempPath("dst")));
  {
    const auto source = tempPath("copy-src2");
    writeRaw(source, "title_id=7\nbackup_name=test\n");
    CHECK(copyMetaFile(source, "/proc/foil-no-such-dir/target.meta"));
    std::remove(source.c_str());
  }
  return failures;
}
int escapeTests() {
  int failures = 0;
  using namespace foil::saves;
  const char *values[] = {"a\nb", "c\rd", "back\\slash", "\\n literal", "trail\\", "x\\\\n", "\\\\", "\r\n\\r"};
  for (const char *value : values) {
    CHECK(unescapeMetaValue(escapeMetaValue(value)) == value);
    CHECK(escapeMetaValue(value).find_first_of("\r\n") == std::string::npos);
    auto meta = fullMeta();
    meta.titleName = value;
    meta.userName = value;
    const auto parsed = parseMeta(serializeMeta(meta));
    CHECK(parsed.value.titleName == value && parsed.value.userName == value);
    CHECK(parsed.value.size == meta.size && parsed.value.isFavorite == meta.isFavorite);
  }
  // A newline in a value must not inject a key.
  auto meta = fullMeta();
  meta.titleName = "Evil\nuser_id=attacker";
  const auto parsed = parseMeta(serializeMeta(meta));
  CHECK(parsed.value.userId == "user-1");
  CHECK(parsed.value.titleName == meta.titleName);
  // Backslashes not followed by n, r or a backslash are written verbatim.
  CHECK(escapeMetaValue("C:\\saves\\data") == "C:\\saves\\data");
  CHECK(escapeMetaValue("a\\nb") == "a\\\\nb");
  return failures;
}
int compatibilityTests() {
  int failures = 0;
  using namespace foil::saves;
  // Exactly what save-keeper's serializeBackupMetadata writes for fullMeta().
  const std::string saveKeeper = "title_id=72174692027785216\n"
                                 "title_name=The Legend of Zelda: BOTW\n"
                                 "backup_name=20260311_090000\n"
                                 "revision_id=rev-42\n"
                                 "device_id=dev-abc\n"
                                 "device_label=OLED Main\n"
                                 "user_id=user-1\n"
                                 "user_name=TestUser\n"
                                 "source=cloud\n"
                                 "created_at=1710000000\n"
                                 "device_priority=200\n"
                                 "size=123456\n"
                                 "is_auto_backup=0\n"
                                 "is_favorite=0\n";
  CHECK(serializeMeta(fullMeta()) == saveKeeper);
  const auto parsed = parseMeta(saveKeeper);
  CHECK(!parsed.error && serializeMeta(parsed.value) == saveKeeper);
  // save-keeper does not escape; lone backslashes and odd sequences survive.
  const auto raw = parseMeta("backup_name=s\ntitle_name=Dir\\Sub\\x \\t end\\\n");
  CHECK(raw.value.titleName == "Dir\\Sub\\x \\t end\\");
  CHECK(serializeMeta(raw.value).find("title_name=Dir\\Sub\\x \\t end\\\n") != std::string::npos);
  // Numbers parse like atoi/strtoull: garbage reads as zero.
  const auto numbers = parseMeta("title_id=abc\nbackup_name=s\ndevice_priority=x\nsize=12kb\n");
  CHECK(numbers.value.titleId == 0 && numbers.value.devicePriority == 0 && numbers.value.size == 12);
  return failures;
}
} // namespace
int runSaveMetaTests() {
  return syncTests() + metadataTests() + fileTests() + escapeTests() + compatibilityTests();
}
