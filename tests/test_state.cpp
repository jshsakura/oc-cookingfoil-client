#include "Test.hpp"
#include "core/State.hpp"
#include "core/Sha256.hpp"
#include "core/Json.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <unistd.h>
// The app's state in cookingfoil.db: state.json moves in once with its
// device key, the credentials are bound to the console and sealed, and
// anything off gives way to a new key while the settings stay.
namespace {
bool sql(const std::string &path, const char *statement) {
  sqlite3 *db = nullptr;
  const bool ok = sqlite3_open(path.c_str(), &db) == SQLITE_OK && sqlite3_exec(db, statement, nullptr, nullptr, nullptr) == SQLITE_OK;
  sqlite3_close(db);
  return ok;
}
int count(const std::string &path, const char *query) {
  sqlite3 *db = nullptr;
  sqlite3_stmt *read = nullptr;
  int n = -1;
  if (sqlite3_open(path.c_str(), &db) == SQLITE_OK && sqlite3_prepare_v2(db, query, -1, &read, nullptr) == SQLITE_OK &&
      sqlite3_step(read) == SQLITE_ROW)
    n = sqlite3_column_int(read, 0);
  sqlite3_finalize(read);
  sqlite3_close(db);
  return n;
}
const std::string KeyA(64, 'a'), Shop = "https://shop.example.com";
} // namespace
int runStateTests() {
  int failures = 0;
  // HMAC-SHA256, RFC 4231 test case 2.
  CHECK(foil::hmacSha256("Jefe", "what do ya want for nothing?") ==
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // The console's value makes both, the same each time, and shows in neither.
  const auto one = foil::deriveIdentity("device-id 0123456789abcdef");
  const auto other = foil::deriveIdentity("device-id fedcba9876543210");
  CHECK(one.fingerprint.size() == 64 && one.sealKey.size() == 32);
  CHECK(one.fingerprint == foil::deriveIdentity("device-id 0123456789abcdef").fingerprint);
  CHECK(one.fingerprint != other.fingerprint && one.sealKey != other.sealKey);
  CHECK(one.fingerprint.find("0123456789abcdef") == std::string::npos);
  CHECK(foil::deriveIdentity("").fingerprint.empty());
  char temporary[] = "/tmp/cookingfoil-statedb-XXXXXX";
  const char *made = mkdtemp(temporary);
  CHECK(made);
  if (!made) return failures;
  const std::filesystem::path root(made);
  const auto db = (root / "cookingfoil.db").string(), legacy = (root / "state.json").string();
  foil::setDeviceIdentity(one);
  foil::takeRebound();
  foil::Error error;
  {
    // state.json of an earlier build: its key goes on (no approval again),
    // its server keys, settings and note too, and the file goes.
    foil::PendingInstall pending{"Paper Moon", "", "sd", {}};
    foil::Item file;
    file.url = Shop + "/a.nsp";
    pending.files.push_back(file);
    std::string written;
    CHECK(foil::atomicWrite(legacy,
                            "{\"deviceKey\":\"" + KeyA + "\",\"language\":\"ja\",\"installTarget\":\"nand\","
                            "\"ignoreFirmware\":false,\"makeShortcuts\":false,\"lastServer\":\"" + Shop + "\","
                            "\"accessKeys\":{\"" + Shop + "\":\"issued\"},\"pairSecrets\":{\"" + Shop + "\":\"" +
                            std::string(64, 'c') + "\"},\"pairMessages\":{\"" + Shop + "\":\"거실\"},\"queue\":" +
                            foil::serializePending(pending) + "}",
                            written));
    const auto moved = foil::loadState(db);
    CHECK(!moved.error && foil::takeRebound() == foil::Rebound::None);
    CHECK(moved.value.deviceKey == KeyA && moved.value.accessKeys.at(Shop) == "issued");
    CHECK(moved.value.pairSecrets.at(Shop) == std::string(64, 'c') && moved.value.pairMessages.at(Shop) == "거실");
    CHECK(moved.value.language == "ja" && moved.value.installTarget == "nand" && !moved.value.skipFirmwareCheck &&
          !moved.value.makeShortcuts && moved.value.skipSignatureCheck && moved.value.lastServer == Shop);
    CHECK(!std::filesystem::exists(legacy));
    // The install it held is there until it goes into the queue, once.
    CHECK(foil::parsePending(moved.value.queueJson).has_value());
    auto queued = foil::loadState(db).value;
    CHECK(queued.deviceKey == KeyA && foil::parsePending(queued.queueJson).has_value());
    queued.queueJson = "[]";
    CHECK(foil::saveState(db, queued, error));
    CHECK(foil::loadState(db).value.queueJson == "[]");
  }
  {
    // The card in another console: a new key, the server keys and secrets
    // gone, the settings and note kept; said once.
    foil::setDeviceIdentity(other);
    const auto elsewhere = foil::loadState(db).value;
    CHECK(foil::takeRebound() == foil::Rebound::Moved && foil::takeRebound() == foil::Rebound::None);
    CHECK(elsewhere.deviceKey.size() == 64 && elsewhere.deviceKey != KeyA);
    CHECK(elsewhere.accessKeys.empty() && elsewhere.pairSecrets.empty());
    CHECK(elsewhere.language == "ja" && elsewhere.pairMessages.at(Shop) == "거실");
    // Bound to this console now: the next start keeps it.
    CHECK(foil::loadState(db).value.deviceKey == elsewhere.deviceKey && foil::takeRebound() == foil::Rebound::None);
    // Back in the first console: another new key, not the old one.
    foil::setDeviceIdentity(one);
    CHECK(foil::loadState(db).value.deviceKey != elsewhere.deviceKey && foil::takeRebound() == foil::Rebound::Moved);
  }
  {
    // Credentials edited on a PC (a key put in, the fingerprint copied):
    // the seal no longer matches, so they all go.
    auto mine = foil::loadState(db).value;
    mine.accessKeys[Shop] = "issued";
    CHECK(foil::saveState(db, mine, error));
    CHECK(foil::loadState(db).value.accessKeys.at(Shop) == "issued" && foil::takeRebound() == foil::Rebound::None);
    CHECK(sql(db, "UPDATE credentials SET value='forged' WHERE kind='access'"));
    const auto edited = foil::loadState(db).value;
    CHECK(foil::takeRebound() == foil::Rebound::Changed);
    CHECK(edited.accessKeys.empty() && edited.deviceKey != mine.deviceKey && edited.language == "ja");
    // A device key swapped for another console's: the same.
    CHECK(sql(db, ("UPDATE credentials SET value='" + KeyA + "' WHERE kind='device'").c_str()));
    CHECK(foil::loadState(db).value.deviceKey != KeyA && foil::takeRebound() == foil::Rebound::Changed);
    // Settings are not sealed: changed on a PC, they are simply read.
    CHECK(sql(db, "UPDATE settings SET value='ko' WHERE key='language'"));
    CHECK(foil::loadState(db).value.language == "ko" && foil::takeRebound() == foil::Rebound::None);
  }
  {
    // The console's ID not read this run: the credentials are used unchecked
    // (not taken for another console's), and a save leaves them as bound.
    auto bound = foil::loadState(db).value;
    bound.accessKeys[Shop] = "issued";
    CHECK(foil::saveState(db, bound, error));
    foil::setDeviceIdentity({});
    auto unread = foil::loadState(db).value;
    CHECK(foil::takeRebound() == foil::Rebound::None);
    CHECK(unread.deviceKey == bound.deviceKey && unread.accessKeys.at(Shop) == "issued");
    unread.accessKeys[Shop] = "other";
    unread.language = "en";
    CHECK(foil::saveState(db, unread, error));
    // Read again next start: the same console, the same key.
    foil::setDeviceIdentity(one);
    const auto back = foil::loadState(db).value;
    CHECK(foil::takeRebound() == foil::Rebound::None);
    CHECK(back.deviceKey == bound.deviceKey && back.accessKeys.at(Shop) == "issued" && back.language == "en");
    // A source that failed once is asked again on the next load.
    int asked = 0;
    foil::setIdentitySource([&] { return ++asked == 1 ? foil::DeviceIdentity{} : one; });
    CHECK(foil::loadState(db).value.deviceKey == bound.deviceKey && foil::takeRebound() == foil::Rebound::None);
    CHECK(foil::loadState(db).value.deviceKey == bound.deviceKey && asked == 2);
    foil::setDeviceIdentity(one);
  }
  {
    // A first start that cannot read the console's ID: the key it makes is
    // kept in memory (the same on every load this run, with what it gets),
    // not written unbound, and bound by the first load that reads the ID.
    std::filesystem::remove(db);
    foil::setDeviceIdentity({});
    auto first = foil::loadState(db).value;
    CHECK(first.deviceKey.size() == 64 && foil::loadState(db).value.deviceKey == first.deviceKey);
    first.accessKeys[Shop] = "issued";
    first.language = "zh";
    CHECK(foil::saveState(db, first, error));
    CHECK(foil::loadState(db).value.accessKeys.at(Shop) == "issued");
    CHECK(count(db, "SELECT COUNT(*) FROM credentials") == 0 && count(db, "SELECT COUNT(*) FROM settings") > 0);
    foil::setDeviceIdentity(one);
    const auto boundNow = foil::loadState(db).value;
    CHECK(foil::takeRebound() == foil::Rebound::None);
    CHECK(boundNow.deviceKey == first.deviceKey && boundNow.accessKeys.at(Shop) == "issued" && boundNow.language == "zh");
    CHECK(foil::loadState(db).value.deviceKey == first.deviceKey && foil::takeRebound() == foil::Rebound::None);
    // Bound to this console: another reads it as moved.
    foil::setDeviceIdentity(other);
    CHECK(foil::loadState(db).value.deviceKey != first.deviceKey && foil::takeRebound() == foil::Rebound::Moved);
    // Moving state.json in without the ID: its key is used, the file stays
    // until a start that reads the ID moves it in for good.
    std::filesystem::remove(db);
    std::string written;
    CHECK(foil::atomicWrite(legacy, "{\"deviceKey\":\"" + KeyA + "\",\"accessKeys\":{\"" + Shop + "\":\"issued\"}}", written));
    foil::setDeviceIdentity({});
    CHECK(foil::loadState(db).value.deviceKey == KeyA && std::filesystem::exists(legacy));
    CHECK(foil::loadState(db).value.accessKeys.at(Shop) == "issued");
    foil::setDeviceIdentity(one);
    const auto movedNow = foil::loadState(db).value;
    CHECK(movedNow.deviceKey == KeyA && movedNow.accessKeys.at(Shop) == "issued" && !std::filesystem::exists(legacy));
    CHECK(foil::takeRebound() == foil::Rebound::None);
  }
  {
    // A state.json that cannot be read: kept as state.json.broken, a new
    // key, said once.
    std::filesystem::remove(db);
    std::string written;
    CHECK(foil::atomicWrite(legacy, "{\"deviceKey\":", written));
    const auto fresh = foil::loadState(db);
    CHECK(!fresh.error && fresh.value.deviceKey.size() == 64 && foil::takeRebound() == foil::Rebound::Recreated);
    CHECK(std::filesystem::exists(legacy + ".broken") && !std::filesystem::exists(legacy));
    // A first start (no state.json, no database) says nothing.
    std::filesystem::remove(db);
    CHECK(foil::loadState(db).value.deviceKey.size() == 64 && foil::takeRebound() == foil::Rebound::None);
    // A damaged database: set aside, a new key, said.
    std::string bad;
    CHECK(foil::atomicWrite(db, "not a database at all, just bytes", bad));
    CHECK(foil::loadState(db).value.deviceKey.size() == 64 && foil::takeRebound() == foil::Rebound::Recreated);
    CHECK(std::filesystem::exists(db + ".broken"));
  }
  foil::setDeviceIdentity(foil::deriveIdentity("unit tests"));
  foil::takeRebound();
  std::filesystem::remove_all(root);
  return failures;
}
