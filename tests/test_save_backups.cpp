// Local backup folders in oc-save-keeper's layout, and the host stand-in for
// backup and restore.
#include "Test.hpp"
#include "core/SaveBackups.hpp"
#include "core/SaveArchive.hpp"
#include "platform/Saves.hpp"
#include "core/ShopSaves.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>
namespace fsys = std::filesystem;
namespace {
std::string scratch(const char *tag) {
  auto path = fsys::temp_directory_path() / ("foil_backups_" + std::to_string(getpid()) + "_" + tag);
  fsys::remove_all(path);
  fsys::create_directories(path);
  return path.string();
}
void write(const std::string &path, const std::string &text) {
  fsys::create_directories(fsys::path(path).parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
std::string read(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
// A backup folder as save-keeper leaves it: files plus a sibling .meta.
void keeperBackup(const std::string &root, uint64_t title, const std::string &name, std::time_t at,
                  bool rollback = false) {
  const auto path = root + "/backups/" + foil::saves::titleHex(title) + "/" + name;
  write(path + "/data.bin", name);
  foil::saves::BackupMeta meta;
  meta.titleId = title;
  meta.backupName = meta.revisionId = name;
  meta.createdAt = at;
  meta.isAutoBackup = rollback;
  foil::saves::writeMetaFile(path + ".meta", meta);
}
} // namespace
int runSaveBackupTests() {
  int failures = 0;
  using namespace foil::saves;
  constexpr uint64_t Title = 0x0100ABCD00010000ULL;
  CHECK(titleHex(Title) == "0100ABCD00010000");
  CHECK(userIdOf({0x2222, 0x1111}) == "00000000000011110000000000002222");
  CHECK(userIdOf({0, 0}) == "device");
  CHECK(deviceLabel("dev-1A2B3C4D-0042") == "1A2B3C4D");
  CHECK(deviceLabel("") == "Unknown");
  CHECK(backupStamp(0).size() == 15);
  CHECK(keeperRoot("sdmc:") == "sdmc:/switch/oc-save-keeper");

  // Listing: newest first by created_at, folders without .meta still shown.
  {
    const auto root = scratch("list");
    keeperBackup(root, Title, "20260101_000000", 100);
    keeperBackup(root, Title, "20260102_000000", 300);
    write(root + "/backups/" + titleHex(Title) + "/stray/file", "x");
    write(root + "/backups/" + titleHex(Title) + "/20260103_000000.zip", "not a folder");
    const auto list = listBackups(root, Title);
    CHECK(list.size() == 3);
    if (list.size() == 3) {
      CHECK(list[0].name == "stray"); // no meta: the folder's mtime, which is now
      CHECK(!list[0].hasMeta);
      CHECK(list[1].name == "20260102_000000");
      CHECK(list[1].hasMeta && list[1].meta.createdAt == 300);
      CHECK(list[2].size == 15);
    }
    CHECK(listBackups(root, Title + 0x1000).empty());
    fsys::remove_all(root);
  }
  // Two backups in one second get separate folders.
  {
    const auto root = scratch("fresh");
    const std::time_t at = 1700000000;
    const auto first = freshBackupName(root, Title, at);
    write(root + "/backups/" + titleHex(Title) + "/" + first + "/f", "1");
    const auto second = freshBackupName(root, Title, at);
    CHECK(second == first + "_2");
    write(root + "/backups/" + titleHex(Title) + "/" + first + "_5.meta", "");
    CHECK(freshBackupName(root, Title, at) == first + "_6");
    fsys::remove_all(root);
  }
  // Limit: user backups and rollback copies are counted apart.
  {
    std::vector<LocalBackup> list;
    for (int i = 0; i < 8; ++i) {
      LocalBackup b;
      b.name = std::to_string(i);
      b.meta.isAutoBackup = i % 2;
      list.push_back(b);
    }
    CHECK(overLimit(list, 3).size() == 2);
    CHECK(overLimit(list, 3)[0].name == "6");
    CHECK(overLimit(list, 3)[1].name == "7");
    CHECK(overLimit(list).empty());
  }
  // Trash: folder, .meta and .zip move together under trash/<TID>.
  {
    const auto root = scratch("trash");
    keeperBackup(root, Title, "20260101_000000", 100);
    write(root + "/backups/" + titleHex(Title) + "/20260101_000000.zip", "zip");
    const auto list = listBackups(root, Title);
    CHECK(list.size() == 1);
    if (!list.empty()) CHECK(!moveToTrash(root, list[0], 555));
    const auto trashed = root + "/trash/" + titleHex(Title) + "/20260101_000000_555";
    CHECK(fsys::is_directory(trashed));
    CHECK(fsys::exists(trashed + ".meta"));
    CHECK(fsys::exists(trashed + ".zip"));
    CHECK(listBackups(root, Title).empty());
    fsys::remove_all(root);
  }
  // Device identity: created once, kept, and other settings survive.
  {
    const auto root = scratch("device");
    write(root + "/config/settings.json", "{\"language\":\"ko\"}");
    const auto made = loadDevice(root, 0x1A2B3C4D, 0x42);
    CHECK(!made.error);
    CHECK(made.value.id == "dev-1A2B3C4D-0042");
    CHECK(made.value.priority == 100);
    const auto again = loadDevice(root, 999, 1);
    CHECK(again.value.id == "dev-1A2B3C4D-0042");
    CHECK(read(root + "/config/settings.json").find("\"language\"") != std::string::npos);
    fsys::remove_all(root);
    // save-keeper's older text files are picked up.
    write(root + "/config/device_id.txt", "dev-AAAAAAAA-0001\n");
    write(root + "/config/device_priority.txt", "7\n");
    const auto legacy = loadDevice(root, 1, 1);
    CHECK(legacy.value.id == "dev-AAAAAAAA-0001");
    CHECK(legacy.value.priority == 7);
    CHECK(read(root + "/config/settings.json").find("dev-AAAAAAAA-0001") != std::string::npos);
    fsys::remove_all(root);
  }
  // Copying commits at the journal size and after the copy.
  {
    const auto root = scratch("copy");
    write(root + "/from/a.bin", std::string(10000, 'a'));
    write(root + "/from/sub/b.bin", std::string(5000, 'b'));
    int commits = 0;
    uint64_t seen = 0;
    CopyHooks hooks;
    hooks.journal = 4096;
    hooks.commit = [&] { ++commits; return true; };
    hooks.progress = [&](uint64_t bytes) { seen = bytes; };
    CHECK(!copyTree(root + "/from", root + "/to", hooks));
    CHECK(read(root + "/to/a.bin") == std::string(10000, 'a'));
    CHECK(read(root + "/to/sub/b.bin") == std::string(5000, 'b'));
    CHECK(seen == 15000);
    CHECK(commits >= 3);
    CHECK(directorySize(root + "/to") == 15000);
    CHECK(!clearTree(root + "/to"));
    CHECK(fsys::is_directory(root + "/to") && fsys::is_empty(root + "/to"));
    hooks.commit = [] { return false; };
    CHECK(copyTree(root + "/from", root + "/to2", hooks));
    fsys::remove_all(root);
  }
  // Host flow: back up, change the save, restore, and the rollback copy.
  {
    const auto sd = scratch("flow");
    foil::platform::SaveJob job;
    job.sd = sd;
    job.save.applicationId = Title;
    job.save.user = {0x2222, 0x1111};
    job.save.userName = "Player";
    job.game.name = "Game";
    const auto live = sd + "/fake-saves/" + titleHex(Title) + "/" + userIdOf(job.save.user);
    CHECK(foil::platform::backupSave(job).error); // no save yet
    write(live + "/progress.dat", "level 1");
    const auto made = foil::platform::backupSave(job);
    CHECK(!made.error);
    CHECK(made.value.meta.userId == "00000000000011110000000000002222");
    CHECK(made.value.meta.source == "local");
    CHECK(!made.value.meta.isAutoBackup);
    CHECK(made.value.meta.deviceId.rfind("dev-", 0) == 0);
    write(live + "/progress.dat", "level 9");
    write(live + "/extra.dat", "later");
    CHECK(!foil::platform::restoreSave(job, made.value));
    CHECK(read(live + "/progress.dat") == "level 1");
    CHECK(!fsys::exists(live + "/extra.dat"));
    const auto list = listBackups(keeperRoot(sd), Title);
    CHECK(list.size() == 2);
    int rollbacks = 0;
    for (const auto &b : list) rollbacks += b.meta.isAutoBackup;
    CHECK(rollbacks == 1);
    // A seventh user backup sends the oldest user backup to the trash.
    for (int i = 0; i < 6; ++i) CHECK(!foil::platform::backupSave(job).error);
    int users = 0;
    for (const auto &b : listBackups(keeperRoot(sd), Title)) users += !b.meta.isAutoBackup;
    CHECK(users == MaxBackups);
    CHECK(fsys::is_directory(keeperRoot(sd) + "/trash/" + titleHex(Title)));
    LocalBackup missing;
    missing.path = sd + "/nowhere";
    CHECK(foil::platform::restoreSave(job, missing));
    fsys::remove_all(sd);
  }
  // Shop saves: the list, and one imported as a backup then restored.
  {
    const auto list = foil::shopsaves::parse(R"({"saves":[
      {"id":"0100152000022000-a1b2c3d4e5f6","titleId":"0100152000022000","name":"전 차종 언락","size":1024,
       "url":"/api/saves/0100152000022000-a1b2c3d4e5f6/download","game":{"name":"Mario Kart 8 Deluxe","names":{"ko":"마리오 카트 8 디럭스"}}},
      {"id":"bad/id","titleId":"0100152000022000","url":"/x"},
      {"id":"x-1","titleId":"nothex","url":"/x"}]})", "https://shop.example");
    CHECK(list.size() == 1);
    if (!list.empty()) {
      CHECK(list[0].url == "https://shop.example/api/saves/0100152000022000-a1b2c3d4e5f6/download");
      CHECK(foil::shopsaves::gameName(list[0], "ko") == "마리오 카트 8 디럭스");
      CHECK(foil::shopsaves::gameName(list[0], "en") == "Mario Kart 8 Deluxe");
    }
    CHECK(foil::shopsaves::pathAllowed("save/slot1.bin"));
    CHECK(!foil::shopsaves::pathAllowed("../x"));
    CHECK(!foil::shopsaves::pathAllowed("/abs"));
    CHECK(!foil::shopsaves::pathAllowed("a//b"));

    const auto sd = scratch("shop");
    foil::platform::SaveJob job;
    job.sd = sd;
    job.save.applicationId = Title;
    job.save.user = {0x2222, 0x1111};
    job.save.userName = "Player";
    job.game.name = "Game";
    const auto live = sd + "/fake-saves/" + titleHex(Title) + "/" + userIdOf(job.save.user);
    write(live + "/progress.dat", "mine");
    auto imported = foil::platform::importBackup(job, [](const std::string &dir) {
      write(dir + "/progress.dat", "unlocked");
      return foil::Error{};
    }, "shop");
    CHECK(!imported.error);
    CHECK(imported.value.meta.source == "shop");
    CHECK(!foil::platform::restoreSave(job, imported.value));
    CHECK(read(live + "/progress.dat") == "unlocked");
    // The player's own save was kept as the rollback copy.
    bool kept = false;
    for (const auto &b : listBackups(keeperRoot(sd), Title))
      kept |= b.meta.isAutoBackup && read(b.path + "/progress.dat") == "mine";
    CHECK(kept);
    // A failed download leaves no half backup behind.
    auto failed = foil::platform::importBackup(job, [](const std::string &dir) {
      write(dir + "/part.bin", "x");
      return foil::Error{"cut off"};
    }, "shop");
    CHECK(failed.error);
    CHECK(listBackups(keeperRoot(sd), Title).size() == 2);
    fsys::remove_all(sd);
  }
  // Dropbox's zip, save-keeper's format: the files plus .dropkeep.meta, and
  // back into backups/<TID>/<name> marked "cloud".
  {
    const auto root = scratch("archive");
    keeperBackup(root, Title, "20260311_090000", 1773219600);
    write(root + "/backups/" + titleHex(Title) + "/20260311_090000/sub/slot.dat", "deep");
    const auto backup = listBackups(root, Title).front();
    const auto zip = root + "/temp/up.zip";
#ifdef FOIL_NO_MINIZIP
    CHECK(exportArchive(backup, {}, zip));
    CHECK(importArchive(root, Title, zip, "x", 5).error);
#else
    CHECK(!exportArchive(backup, {}, zip));
    // Brought down on another console: a fresh root with nothing in it.
    const auto other = scratch("archive_other");
    auto got = importArchive(other, Title, zip, "ignored", 1773300000);
    CHECK(!got.error);
    CHECK(got.value.name == "20260311_090000");
    CHECK(read(got.value.path + "/data.bin") == "20260311_090000");
    CHECK(read(got.value.path + "/sub/slot.dat") == "deep");
    CHECK(!fsys::exists(got.value.path + "/.dropkeep.meta"));
    const auto meta = readMetaFile(got.value.path + ".meta");
    CHECK(!meta.error && meta.value.source == "cloud" && meta.value.createdAt == 1773219600);
    CHECK(meta.value.titleId == Title && meta.value.backupName == "20260311_090000");
    // The same name again does not overwrite: save-keeper adds _<now>.
    auto again = importArchive(other, Title, zip, "ignored", 1773300001);
    CHECK(!again.error && again.value.name == "20260311_090000_1773300001");
    CHECK(listBackups(other, Title).size() == 2);
    CHECK(!fsys::exists(other + "/temp/import_" + titleHex(Title) + "_1773300000"));
    // A backup without a .meta gets the fallback written before zipping.
    write(root + "/backups/" + titleHex(Title) + "/bare/file", "b");
    LocalBackup bare;
    bare.name = "bare";
    bare.path = root + "/backups/" + titleHex(Title) + "/bare";
    BackupMeta fallback;
    fallback.titleId = Title;
    fallback.backupName = fallback.revisionId = "bare";
    fallback.createdAt = 42;
    CHECK(!exportArchive(bare, fallback, root + "/temp/bare.zip"));
    CHECK(readMetaFile(bare.path + ".meta").value.createdAt == 42);
    // A .meta without a device gets this console's, so Dropbox can tell
    // uploads from different consoles apart; one with a device keeps it.
    BackupMeta device;
    device.deviceId = "dev-1A2B3C4D-0042";
    device.deviceLabel = "1A2B3C4D";
    CHECK(!exportArchive(backup, device, root + "/temp/dev.zip"));
    CHECK(readMetaFile(backup.path + ".meta").value.deviceId == "dev-1A2B3C4D-0042");
    CHECK(readMetaFile(backup.path + ".meta").value.backupName == "20260311_090000");
    device.deviceId = "dev-OTHER-0001";
    CHECK(!exportArchive(backup, device, root + "/temp/dev2.zip"));
    CHECK(readMetaFile(backup.path + ".meta").value.deviceId == "dev-1A2B3C4D-0042");
    // Not a zip: nothing is left behind.
    write(root + "/temp/junk.zip", "<html>");
    CHECK(importArchive(other, Title, root + "/temp/junk.zip", "junk", 7).error);
    CHECK(listBackups(other, Title).size() == 2);
    fsys::remove_all(other);
#endif
    fsys::remove_all(root);
  }
  return failures;
}
