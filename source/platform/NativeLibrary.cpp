#include "platform/Installed.hpp"
#include "platform/Storage.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
#ifdef __SWITCH__
namespace {
Error failure(const char *action, Result rc) {
  char code[20];
  std::snprintf(code, sizeof(code), "%08X", rc);
  return {trf("{0} 코드 {1}", {action, code})};
}
std::string hex(uint64_t id) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(id));
  return text;
}
template <size_t N> std::string bounded(const char (&text)[N]) {
  return std::string(text, std::find(text, text + N, '\0'));
}
struct NsSession {
  Result rc = nsInitialize();
  ~NsSession() { if (R_SUCCEEDED(rc)) nsExit(); }
};
struct NcmSession {
  Result rc = ncmInitialize();
  ~NcmSession() { if (R_SUCCEEDED(rc)) ncmExit(); }
};
struct Database {
  NcmContentMetaDatabase db{};
  bool opened = false;
  ~Database() { if (opened) ncmContentMetaDatabaseClose(&db); }
};
struct Reader {
  FsSaveDataInfoReader reader{};
  bool opened = false;
  ~Reader() { if (opened) fsSaveDataInfoReaderClose(&reader); }
};
Parsed<std::vector<NcmApplicationContentMetaKey>> keys(Database &db, NcmContentMetaType type) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    s32 total = 0, written = 0;
    auto rc = ncmContentMetaDatabaseListApplication(&db.db, &total, &written, nullptr, 0, type);
    if (R_FAILED(rc)) return {{}, failure(tr("설치된 콘텐츠를 조회하지 못했습니다."), rc)};
    if (total < 0 || total > 65536) return {{}, {tr("설치 목록의 크기를 확인하지 못했습니다.")}};
    std::vector<NcmApplicationContentMetaKey> result(total);
    const int expected = total;
    rc = ncmContentMetaDatabaseListApplication(&db.db, &total, &written, result.data(), expected, type);
    if (R_FAILED(rc)) return {{}, failure(tr("설치된 콘텐츠를 읽지 못했습니다."), rc)};
    if (total == expected && written == expected) return {std::move(result), {}};
  }
  return {{}, {tr("설치 목록이 바뀌고 있습니다. 잠시 뒤 새로고침합니다.")}};
}
// Reading each game's control data from NS takes long enough that a large
// library showed placeholders for seconds on every launch. Name, publisher,
// version, size and icon are kept on the SD card and reused while the game's
// NS record is unchanged; a new language or an update reads it again.
const std::string InstalledCache = "sdmc:/switch/cookingfoil/cache/installed";
struct CachedGame {
  uint64_t updated = 0, bytes = 0;
  std::string name, publisher, version;
  bool icon = false;
};
std::map<std::string, CachedGame> loadInstalledCache(const std::string &language) {
  std::map<std::string, CachedGame> cache;
  Error error;
  auto root = parseJson(readFile(InstalledCache + "/index.json"), error);
  if (error || stringField(root.get(), "language") != language) return cache;
  auto *games = field(root.get(), "games");
  if (!json_object_is_type(games, json_type_object)) return cache;
  json_object_object_foreach(games, id, entry) {
    if (!json_object_is_type(entry, json_type_object)) continue;
    CachedGame g;
    g.updated = uint64_t(integerField(entry, "updated"));
    g.bytes = uint64_t(integerField(entry, "bytes"));
    g.name = stringField(entry, "name");
    g.publisher = stringField(entry, "publisher");
    g.version = stringField(entry, "version");
    g.icon = integerField(entry, "icon") != 0;
    cache[id] = std::move(g);
  }
  return cache;
}
void saveInstalledCache(const std::string &language, const std::map<std::string, CachedGame> &cache) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "language", json_object_new_string(language.c_str()));
  auto *games = json_object_new_object();
  for (const auto &[id, g] : cache) {
    auto *entry = json_object_new_object();
    json_object_object_add(entry, "updated", json_object_new_int64(int64_t(g.updated)));
    json_object_object_add(entry, "bytes", json_object_new_int64(int64_t(g.bytes)));
    json_object_object_add(entry, "name", json_object_new_string(g.name.c_str()));
    json_object_object_add(entry, "publisher", json_object_new_string(g.publisher.c_str()));
    json_object_object_add(entry, "version", json_object_new_string(g.version.c_str()));
    json_object_object_add(entry, "icon", json_object_new_int(g.icon));
    json_object_object_add(games, id.c_str(), entry);
  }
  json_object_object_add(root.get(), "games", games);
  std::string ignored;
  atomicWrite(InstalledCache + "/index.json", json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN), ignored);
}
uint64_t currentApplication(Error &error) {
  auto rc = pmdmntInitialize();
  if (R_FAILED(rc)) { error = failure(tr("실행 중인 게임을 확인하지 못했습니다."), rc); return 0; }
  u64 pid = 0, id = 0;
  rc = pmdmntGetApplicationProcessId(&pid);
  pmdmntExit();
  if (R_FAILED(rc)) { error = failure(tr("실행 중인 게임을 확인하지 못했습니다."), rc); return 0; }
  // Opened from the album with no game in the background: nothing runs.
  if (!pid) return 0;
  rc = pminfoInitialize();
  if (R_FAILED(rc)) { error = failure(tr("실행 중인 게임을 확인하지 못했습니다."), rc); return 0; }
  rc = pminfoGetProgramId(&id, pid);
  pminfoExit();
  if (R_FAILED(rc) || !id) error = failure(tr("실행 중인 게임을 확인하지 못했습니다."), rc);
  return id & ~0xFFFULL;
}
} // namespace
#endif
Parsed<InstalledLibrary> readInstalled() {
#ifdef __SWITCH__
  NsSession ns;
  if (R_FAILED(ns.rc)) return {{}, failure(tr("게임 목록 서비스를 열지 못했습니다."), ns.rc)};
  NcmSession ncm;
  if (R_FAILED(ncm.rc)) return {{}, failure(tr("콘텐츠 서비스를 열지 못했습니다."), ncm.rc)};
  InstalledLibrary result;
  std::map<uint64_t, InstalledGame> games;
  for (auto storage : {NcmStorageId_BuiltInUser, NcmStorageId_SdCard}) {
    Database db;
    auto rc = ncmOpenContentMetaDatabase(&db.db, storage);
    if (R_FAILED(rc)) return {{}, failure(tr("저장 위치의 설치 목록을 읽지 못했습니다."), rc)};
    db.opened = true;
    for (auto type : {NcmContentMetaType_Application, NcmContentMetaType_Patch, NcmContentMetaType_AddOnContent}) {
      auto entries = keys(db, type);
      if (entries.error) return {{}, entries.error};
      for (const auto &entry : entries.value) {
        if (entry.key.install_type != NcmContentInstallType_Full) continue;
        result.installed.titles.push_back({hex(entry.key.id), entry.key.version});
        if (type == NcmContentMetaType_AddOnContent) continue;
        const auto baseId = type == NcmContentMetaType_Application ? entry.key.id
                           : entry.application_id ? entry.application_id : entry.key.id & ~0xFFFULL;
        auto &g = games[baseId];
        g.applicationId = baseId;
        if (type == NcmContentMetaType_Application) {
          g.titleId = hex(entry.key.id);
          const auto location = storage == NcmStorageId_SdCard ? tr("SD 카드") : tr("본체");
          if (std::find(g.storage.begin(), g.storage.end(), location) == g.storage.end())
            g.storage.push_back(location);
        }
        if (type != NcmContentMetaType_AddOnContent)
          g.version = std::max(g.version, entry.key.version);
      }
    }
  }
  // NS records provide cached control metadata; NCM above decides whether the
  // base actually exists. Archived records and update-only leftovers stay out.
  std::map<uint64_t, uint64_t> recorded;
  for (int offset = 0; offset < 65536;) {
    NsApplicationRecord records[64]{};
    s32 count = 0;
    const auto rc = nsListApplicationRecord(records, 64, offset, &count);
    if (R_FAILED(rc) || count < 0 || count > 64)
      return {{}, failure(tr("본체의 게임 기록을 읽지 못했습니다."), rc)};
    for (int i = 0; i < count; ++i) recorded[records[i].application_id] = records[i].last_updated;
    offset += count;
    if (count < 64) break;
    if (offset >= 65536) return {{}, {tr("본체의 게임 기록이 너무 많습니다.")}};
  }
  const auto language = systemLanguage();
  const auto cached = loadInstalledCache(language);
  std::map<std::string, CachedGame> fresh;
  std::error_code ignored;
  makeDirectories(std::filesystem::path(InstalledCache).string());
  for (auto &[id, g] : games) {
    if (g.titleId.empty() || g.storage.empty()) continue;
    g.name = g.titleId;
    const auto record = recorded.find(id);
    if (record != recorded.end()) {
      g.lastUpdated = record->second;
      const auto iconPath = InstalledCache + "/" + g.titleId + ".jpg";
      const auto hit = cached.find(g.titleId);
      if (hit != cached.end() && hit->second.updated == g.lastUpdated && !hit->second.name.empty()) {
        const auto &c = hit->second;
        g.name = c.name;
        g.publisher = c.publisher;
        g.displayVersion = c.version;
        g.bytes = c.bytes;
        if (c.icon) g.iconBytes = readFile(iconPath);
        // A missing icon file reads the control data again next time.
        if (!c.icon || !g.iconBytes.empty()) {
          fresh[g.titleId] = c;
          result.games.push_back(std::move(g));
          continue;
        }
      }
      // Four 0x20 entries: storage id, padding, then app, patch and add-on bytes.
      NsApplicationOccupiedSize occupied{};
      if (R_SUCCEEDED(nsCalculateApplicationOccupiedSize(id, &occupied)))
        for (int entry = 0; entry < 4; ++entry) {
          u64 sizes[3]{};
          std::memcpy(sizes, occupied.unk_x0 + entry * 0x20 + 8, sizeof(sizes));
          g.bytes += sizes[0] + sizes[1] + sizes[2];
        }
      auto control = std::make_unique<NsApplicationControlData>();
      u64 size = 0;
      const auto rc = nsGetApplicationControlData(NsApplicationControlSource_Storage, id, control.get(), sizeof(*control), &size);
      if (R_SUCCEEDED(rc) && size >= sizeof(NacpStruct)) {
        NacpLanguageEntry *language = nullptr;
        if (R_SUCCEEDED(nacpGetLanguageEntry(&control->nacp, &language)) && language) {
          const auto name = bounded(language->name);
          if (!name.empty()) g.name = name;
          g.publisher = bounded(language->author);
        }
        g.displayVersion = bounded(control->nacp.display_version);
        // Every installed game keeps its icon in memory; an oversized one is
        // dropped and the card falls back to the shop cover or a letter tile.
        constexpr uint64_t MaxIconBytes = 256 * 1024;
        const auto iconSize = std::min<uint64_t>(size - sizeof(NacpStruct), sizeof(control->icon));
        if (iconSize <= MaxIconBytes)
          g.iconBytes.assign(reinterpret_cast<const char *>(control->icon), iconSize);
        // Only a read that succeeded is remembered.
        CachedGame c{g.lastUpdated, g.bytes, g.name, g.publisher, g.displayVersion, false};
        std::string error;
        c.icon = !g.iconBytes.empty() && atomicWrite(iconPath, g.iconBytes, error);
        fresh[g.titleId] = std::move(c);
      }
    }
    result.games.push_back(std::move(g));
  }
  // Icons of games no longer installed go with them.
  for (const auto &[id, c] : cached)
    if (!fresh.count(id)) std::filesystem::remove(InstalledCache + "/" + id + ".jpg", ignored);
  saveInstalledCache(language, fresh);
  result.games = sortInstalled(std::move(result.games), InstalledSort::Name);
  result.installed.complete = true;
  return {std::move(result), {}};
#else
  return {{}, {tr("설치된 게임 조회는 본체에서 사용할 수 있습니다.")}};
#endif
}
Parsed<std::vector<SaveEntry>> readSaves(uint64_t applicationId) {
#ifdef __SWITCH__
  Reader reader;
  auto rc = fsOpenSaveDataInfoReader(&reader.reader, FsSaveDataSpaceId_User);
  if (R_FAILED(rc)) return {{}, failure(tr("세이브 목록을 열지 못했습니다."), rc)};
  reader.opened = true;
  const bool accounts = R_SUCCEEDED(accountInitialize(AccountServiceType_System));
  struct Accounts { bool initialized; ~Accounts() { if (initialized) accountExit(); } } closer{accounts};
  std::vector<SaveEntry> result;
  const auto nameOf = [&](AccountUid uid) {
    std::string name = trf("계정 {0}", {hex(uid.uid[0]) + hex(uid.uid[1])});
    AccountProfile profile{};
    if (accounts && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
      AccountProfileBase base{};
      if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &base)) && !bounded(base.nickname).empty())
        name = bounded(base.nickname);
      accountProfileClose(&profile);
    }
    return name;
  };
  for (;;) {
    FsSaveDataInfo entries[32]{};
    s64 count = 0;
    rc = fsSaveDataInfoReaderRead(&reader.reader, entries, 32, &count);
    if (R_FAILED(rc) || count < 0 || count > 32) return {{}, failure(tr("세이브 목록을 읽지 못했습니다."), rc)};
    for (int i = 0; i < count; ++i) {
      const auto &v = entries[i];
      const bool account = v.save_data_type == FsSaveDataType_Account && accountUidIsValid(&v.uid);
      const bool shared = v.save_data_type == FsSaveDataType_Device && !accountUidIsValid(&v.uid);
      if (v.application_id != applicationId || (!account && !shared) ||
          v.save_data_space_id != FsSaveDataSpaceId_User || v.save_data_rank != 0) continue;
      SaveEntry save;
      save.applicationId = applicationId; save.saveId = v.save_data_id;
      save.type = v.save_data_type;
      save.user = {v.uid.uid[0], v.uid.uid[1]}; save.bytes = v.size;
      save.userName = shared ? std::string(tr("공용 세이브")) : nameOf(v.uid);
      result.push_back(std::move(save));
    }
    if (count < 32) break;
  }
  // Every account (and the shared save) the game could have but does not
  // yet, as JKSV lists them: restoring there creates the save first. The
  // game's control data says which kinds it uses.
  NacpStruct nacp{};
  bool control = false;
  if (R_SUCCEEDED(nsInitialize())) {
    auto data = std::make_unique<NsApplicationControlData>();
    u64 size = 0;
    control = R_SUCCEEDED(nsGetApplicationControlData(NsApplicationControlSource_Storage, applicationId, data.get(),
                                                      sizeof(*data), &size)) &&
              size >= sizeof(NacpStruct);
    if (control) nacp = data->nacp;
    nsExit();
  }
  const auto has = [&](int type, const std::array<uint64_t, 2> &user) {
    return std::any_of(result.begin(), result.end(), [&](const SaveEntry &s) { return s.type == type && s.user == user; });
  };
  const auto missing = [&](int type, const std::array<uint64_t, 2> &user, const std::string &name) {
    SaveEntry save;
    save.applicationId = applicationId;
    save.type = type;
    save.user = user;
    save.userName = name;
    save.missing = true;
    result.push_back(std::move(save));
  };
  if (control && accounts && nacp.user_account_save_data_size > 0) {
    AccountUid uids[ACC_USER_LIST_SIZE]{};
    s32 total = 0;
    if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total)))
      for (s32 i = 0; i < total; ++i)
        if (accountUidIsValid(&uids[i]) && !has(FsSaveDataType_Account, {uids[i].uid[0], uids[i].uid[1]}))
          missing(FsSaveDataType_Account, {uids[i].uid[0], uids[i].uid[1]}, nameOf(uids[i]));
  }
  if (control && nacp.device_save_data_size > 0 && !has(FsSaveDataType_Device, {0, 0}))
    missing(FsSaveDataType_Device, {0, 0}, tr("공용 세이브"));
  return {std::move(result), {}};
#else
  (void)applicationId;
  return {{}, {tr("세이브 조회는 본체에서 사용할 수 있습니다.")}};
#endif
}
Error removeInstalled(const RemovalRequest &request) {
#ifdef __SWITCH__
  Error error;
  const auto running = currentApplication(error);
  if (error) return error;
  auto library = readInstalled();
  if (library.error) return library.error;
  auto saves = request.kind == RemovalKind::Save ? readSaves(request.applicationId) : Parsed<std::vector<SaveEntry>>{};
  if (saves.error) return saves.error;
  error = validateRemoval(request, library.value, saves.value, running, false);
  if (error) return error;
  NsSession ns;
  if (R_FAILED(ns.rc)) return failure(tr("삭제 서비스를 열지 못했습니다."), ns.rc);
  // Entity removal deletes software content. Save deletion is a separate,
  // explicitly selected account operation, never DeleteApplicationCompletely.
  const auto rc = request.kind == RemovalKind::Game
      ? nsDeleteApplicationEntity(request.applicationId)
      : nsDeleteSaveData(FsSaveDataSpaceId_User, request.save.saveId);
  if (R_FAILED(rc)) return failure(tr("삭제하지 못했습니다."), rc);
  return {};
#else
  (void)request;
  return {tr("호스트에서는 게임과 세이브를 삭제하지 않습니다.")};
#endif
}
#ifdef __SWITCH__
namespace {
struct Storage {
  NcmContentStorage cs{};
  bool opened = false;
  ~Storage() { if (opened) ncmContentStorageClose(&cs); }
};
std::string key(const NcmContentId &id) { return std::string(reinterpret_cast<const char *>(id.c), sizeof(id.c)); }
// Every content id the storage's installed records point to, the records'
// own meta content included.
Parsed<std::vector<std::string>> referenced(Database &db) {
  s32 total = 0, written = 0;
  auto rc = ncmContentMetaDatabaseList(&db.db, &total, &written, nullptr, 0, NcmContentMetaType_Unknown, 0, 0,
                                       UINT64_MAX, NcmContentInstallType_Full);
  if (R_FAILED(rc) || total < 0 || total > 65536)
    return {{}, failure(tr("설치된 콘텐츠를 조회하지 못했습니다."), rc)};
  std::vector<NcmContentMetaKey> metas(total);
  rc = ncmContentMetaDatabaseList(&db.db, &total, &written, metas.data(), s32(metas.size()), NcmContentMetaType_Unknown,
                                  0, 0, UINT64_MAX, NcmContentInstallType_Full);
  if (R_FAILED(rc) || written != s32(metas.size()))
    return {{}, failure(tr("설치된 콘텐츠를 읽지 못했습니다."), rc)};
  std::vector<std::string> ids;
  NcmContentInfo infos[32];
  for (const auto &meta : metas)
    for (s32 offset = 0;; offset += written) {
      written = 0;
      rc = ncmContentMetaDatabaseListContentInfo(&db.db, &written, infos, 32, &meta, offset);
      if (R_FAILED(rc))
        return {{}, failure(tr("설치된 콘텐츠를 읽지 못했습니다."), rc)};
      for (s32 i = 0; i < written; ++i)
        ids.push_back(key(infos[i].content_id));
      if (written < 32)
        break;
    }
  return {std::move(ids), {}};
}
} // namespace
#endif
Parsed<CleanupReport> cleanOrphans(bool remove, CleanupProgress *progress) {
#ifdef __SWITCH__
  Parsed<CleanupReport> result;
  NcmSession ncm;
  if (R_FAILED(ncm.rc)) return {{}, failure(tr("콘텐츠 서비스를 열지 못했습니다."), ncm.rc)};
  for (const auto id : {NcmStorageId_SdCard, NcmStorageId_BuiltInUser}) {
    if (progress) {
      progress->stage = id == NcmStorageId_SdCard ? 1 : 2;
      progress->done = progress->total = 0;
    }
    Storage storage;
    Database db;
    // No SD card (or an unreadable one) simply has nothing to clean.
    if (R_FAILED(ncmOpenContentStorage(&storage.cs, id)))
      continue;
    storage.opened = true;
    if (R_FAILED(ncmOpenContentMetaDatabase(&db.db, id)))
      continue;
    db.opened = true;
    auto used = referenced(db);
    if (used.error) return {{}, used.error};
    s32 count = 0;
    if (R_FAILED(ncmContentStorageGetContentCount(&storage.cs, &count)) || count < 0 || count > 262144)
      return {{}, {tr("저장 위치의 콘텐츠를 읽지 못했습니다.")}};
    std::vector<NcmContentId> contents(count);
    s32 written = 0;
    if (count && (R_FAILED(ncmContentStorageListContentId(&storage.cs, contents.data(), count, &written, 0)) ||
                  written != count))
      return {{}, {tr("저장 위치의 콘텐츠를 읽지 못했습니다.")}};
    std::vector<std::string> stored;
    for (const auto &content : contents)
      stored.push_back(key(content));
    const auto orphans = orphanContents(stored, used.value);
    if (!orphans)
      return {{}, {tr("설치 기록을 읽지 못해 정리를 멈췄습니다.")}};
    if (progress) progress->total = int(orphans->size());
    for (const auto &orphan : *orphans) {
      if (progress) ++progress->done;
      NcmContentId content{};
      std::memcpy(content.c, orphan.data(), sizeof(content.c));
      s64 size = 0;
      ncmContentStorageGetSizeFromContentId(&storage.cs, &size, &content);
      if (remove && R_FAILED(ncmContentStorageDelete(&storage.cs, &content)))
        continue;
      ++result.value.files;
      result.value.bytes += uint64_t(std::max<s64>(0, size));
      if (progress) progress->bytes = result.value.bytes;
    }
    NcmPlaceHolderId holders[64];
    s32 holderCount = 0;
    if (R_SUCCEEDED(ncmContentStorageListPlaceHolder(&storage.cs, holders, 64, &holderCount)) && holderCount > 0) {
      for (s32 i = 0; i < holderCount; ++i) {
        s64 size = 0;
        if (R_SUCCEEDED(ncmContentStorageGetSizeFromPlaceHolderId(&storage.cs, &size, &holders[i])))
          result.value.bytes += uint64_t(std::max<s64>(0, size));
      }
      if (!remove || R_SUCCEEDED(ncmContentStorageCleanupAllPlaceHolder(&storage.cs)))
        result.value.placeholders += holderCount;
    }
  }
  return result;
#else
  (void)remove;
  (void)progress;
  return {{}, {tr("남은 파일 정리는 본체에서 사용할 수 있습니다.")}};
#endif
}
} // namespace foil::platform
