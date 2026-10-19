// Writes the shortcuts core/Forwarder builds, the way sphaira's owo.cpp
// installs them (https://github.com/ITotalJustice/sphaira, GPL-3.0, by
// ITotalJustice): the NCAs straight into NCM content storage, the content
// meta into its database, then an application record with NS. No prod.keys
// and no ticket: the only key is the NCA header key, derived by the
// console's own crypto service from fixed sources (as sphaira and CyberFoil
// do), and the sections are not encrypted.
#include "platform/Shortcut.hpp"
#include "core/Forwarder.hpp"
#include "core/I18n.hpp"
#include "core/Log.hpp"
#ifdef __SWITCH__
#include <switch.h>
extern "C" {
#include "nx/ipc/ns_ext.h" // C, and its libnx includes carry no extern "C" of their own
}
#include "util/crypto.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#else
#include <mutex>
#endif
namespace foil::platform {
#ifdef __SWITCH__
// core/Forwarder writes these structures as byte offsets; they must match
// libnx's.
static_assert(offsetof(NacpStruct, startup_user_account) == 0x3025);
static_assert(offsetof(NacpStruct, screenshot) == 0x3034);
static_assert(offsetof(NacpStruct, play_log_policy) == 0x3037);
static_assert(offsetof(NacpStruct, presence_group_id) == 0x3038);
static_assert(offsetof(NacpStruct, display_version) == 0x3060);
static_assert(offsetof(NacpStruct, add_on_content_base_id) == 0x3070);
static_assert(offsetof(NacpStruct, save_data_owner_id) == 0x3078);
static_assert(offsetof(NacpStruct, user_account_save_data_size) == 0x3080);
static_assert(offsetof(NacpStruct, device_save_data_journal_size) == 0x3098);
static_assert(offsetof(NacpStruct, application_error_code_category) == 0x30A8);
static_assert(offsetof(NacpStruct, local_communication_id) == 0x30B0);
static_assert(offsetof(NacpStruct, logo_type) == 0x30F0);
static_assert(offsetof(NacpStruct, pseudo_device_id_seed) == 0x30F8);
static_assert(offsetof(NacpStruct, user_account_save_data_size_max) == 0x3148);
static_assert(offsetof(NacpStruct, device_save_data_journal_size_max) == 0x3160);
static_assert(offsetof(NacpStruct, play_log_query_capability) == 0x3210);
static_assert(offsetof(NacpStruct, required_network_service_license_on_launch) == 0x3213);
static_assert(sizeof(NacpStruct) == forwarder::NacpSize);
static_assert(offsetof(romfs_header, fileDataOff) == 0x48 && sizeof(romfs_header) == 0x50);
static_assert(sizeof(romfs_dir) == 0x18 && sizeof(romfs_file) == 0x20);
static_assert(sizeof(NcmContentMetaKey) == 0x10 && sizeof(NcmContentInfo) == 0x18);
static_assert(sizeof(NcmContentMetaHeader) == 8 && sizeof(NcmApplicationMetaExtendedHeader) == 0x10);
namespace {
// The loader (sphaira's hbl, built with the app) in this app's RomFS.
constexpr const char *LoaderMain = "romfs:/forwarder/main", *LoaderNpdm = "romfs:/forwarder/main.npdm";
// NCA header key sources (public, the same on every console): the console's
// crypto service turns them into the key. From sphaira's keys.cpp.
constexpr u8 HeaderKekSource[0x10] = {0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D,
                                     0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A};
constexpr u8 HeaderKeySource[0x20] = {0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2,
                                     0x5D, 0x19, 0x7B, 0xF5, 0xD0, 0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28,
                                     0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2};
constexpr Result NotFound = 0x410; // ns: no such application
std::string readFile(const char *path) {
  std::ifstream file(path, std::ios::binary);
  return file ? std::string(std::istreambuf_iterator<char>(file), {}) : std::string();
}
std::string code(Result rc) {
  char text[16];
  std::snprintf(text, sizeof(text), "0x%08X", rc);
  return text;
}
Error failure(const char *step, Result rc) {
  logLine(std::string("shortcut ") + step + " " + code(rc));
  return {trf("Installing the shortcut failed. Code {0}", {code(rc)})};
}
// The services a shortcut needs, closed in reverse on the way out.
struct Services {
  Result rc = 0;
  int open = 0;
  Services() {
    if (R_FAILED(rc = ncmInitialize())) return;
    ++open;
    if (R_FAILED(rc = nsextInitialize())) return;
    ++open;
    if (R_FAILED(rc = splCryptoInitialize())) return;
    ++open;
  }
  ~Services() {
    if (open > 2) splCryptoExit();
    if (open > 1) nsextExit();
    if (open > 0) ncmExit();
  }
};
bool present(u64 id) {
  s32 count = 0;
  return R_SUCCEEDED(nsCountApplicationContentMeta(id, &count)) && count > 0;
}
// sphaira's automatic debug mode: Atmosphere 1.8.0 moved ForceDebug, so the
// new bit is set from that version on.
bool atmosphereForceDebug() {
  if (R_FAILED(splInitialize())) return false;
  u64 version = 0;
  const bool ok = R_SUCCEEDED(splGetConfig(SplConfigItem(65000), &version)); // ExosphereApiVersion
  splExit();
  return ok && (version >> 40) >= MAKEHOSVERSION(1, 8, 0);
}
Result write(NcmContentStorage &storage, const forwarder::Nca &nca) {
  NcmContentId id{};
  std::copy_n(nca.hash.begin(), sizeof(id.c), id.c);
  NcmPlaceHolderId placeholder{};
  Result rc = ncmContentStorageGeneratePlaceHolderId(&storage, &placeholder);
  if (R_FAILED(rc)) return rc;
  ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
  if (R_FAILED(rc = ncmContentStorageCreatePlaceHolder(&storage, &id, &placeholder, s64(nca.bytes.size())))) return rc;
  rc = ncmContentStorageWritePlaceHolder(&storage, &placeholder, 0, nca.bytes.data(), nca.bytes.size());
  if (R_SUCCEEDED(rc)) {
    ncmContentStorageDelete(&storage, &id); // the same content from a shortcut made before
    rc = ncmContentStorageRegister(&storage, &id, &placeholder);
  }
  if (R_FAILED(rc)) ncmContentStorageDeletePlaceHolder(&storage, &placeholder);
  return rc;
}
} // namespace
std::string shortcutBlocker() {
  // Neither can change while the app runs, so they are asked once.
  static const std::string reason = [] {
    if (!std::ifstream(LoaderMain).good() || !std::ifstream(LoaderNpdm).good())
      return std::string(tr("This build lacks the loader shortcuts need. Download and install CookingFoil again to make them."));
    u8 kek[0x10]{};
    Result rc = splCryptoInitialize();
    if (R_SUCCEEDED(rc)) {
      rc = splCryptoGenerateAesKek(HeaderKekSource, 0, 0, kek);
      splCryptoExit();
    }
    if (R_FAILED(rc)) {
      logLine("shortcut check: header key " + code(rc));
      return std::string(tr("Shortcuts can be made only when the console is booted into Atmosphere. Restart into Atmosphere to make one."));
    }
    return std::string();
  }();
  return reason;
}
Error makeShortcut(const ShortcutRequest &request, uint64_t &id) {
  forwarder::Input input;
  input.nroPath = request.nroPath;
  input.extraArgs = request.extraArgs;
  input.name = request.name;
  input.author = request.author;
  input.nacp = request.nacp;
  input.icon = request.icon;
  input.hblMain = readFile(LoaderMain);
  input.hblNpdm = readFile(LoaderNpdm);
  if (input.hblMain.empty() || input.hblNpdm.empty())
    return {tr("This build lacks the loader shortcuts need. Download and install CookingFoil again to make them.")};
  if (input.icon.empty()) return {tr("The app file has no icon, so no shortcut can be made.")};
  Services services;
  if (R_FAILED(services.rc)) return failure("services", services.rc);
  // The NCA header key, from the console. Without it nothing can be written;
  // it fails only outside custom firmware, which the player can fix.
  u8 kek[0x10]{}, key[0x20]{};
  Result rc = splCryptoGenerateAesKek(HeaderKekSource, 0, 0, kek);
  if (R_SUCCEEDED(rc)) rc = splCryptoGenerateAesKey(kek, HeaderKeySource, key);
  if (R_SUCCEEDED(rc)) rc = splCryptoGenerateAesKey(kek, HeaderKeySource + 0x10, key + 0x10);
  if (R_FAILED(rc)) {
    logLine("shortcut header key " + code(rc));
    return {tr("Could not derive the NCA header key on the console. Shortcuts can be made only when booted into Atmosphere. Restart into Atmosphere and try again.")};
  }
  input.forceDebug = atmosphereForceDebug();
  input.cores = request.cores;
  input.storageId = NcmStorageId_SdCard;
  forwarder::Package package;
  std::string reason;
  const bool built = forwarder::build(input, [&key](uint8_t *header) {
    Crypto::AesXtr xts(key, true);
    xts.encrypt(header, header, forwarder::NcaHeaderSize, 0, 0x200);
  }, package, reason);
  if (!built) {
    logLine("shortcut build: " + reason);
    return {tr("Could not make a shortcut from the app file.")};
  }
  id = package.titleId;
  // Made before for this path (by this app, or by sphaira in either of its
  // ranges): removed first, so the new one replaces it.
  const u64 legacy = forwarder::titleId(input.nroPath, input.extraArgs, forwarder::LegacyTitleIdPrefix);
  for (const u64 old : {u64(id), legacy})
    if (present(old)) {
      rc = nsDeleteApplicationCompletely(old);
      if (R_FAILED(rc) && rc != NotFound) return failure("remove old", rc);
    }
  {
    NcmContentStorage storage{};
    if (R_FAILED(rc = ncmOpenContentStorage(&storage, NcmStorageId_SdCard))) return failure("storage", rc);
    for (const auto &nca : package.ncas)
      if (R_FAILED(rc = write(storage, nca))) break;
    ncmContentStorageClose(&storage);
    if (R_FAILED(rc)) return failure("write", rc);
  }
  NcmContentMetaKey metaKey{};
  std::copy_n(package.metaKey.data(), sizeof(metaKey), reinterpret_cast<char *>(&metaKey));
  {
    NcmContentMetaDatabase db{};
    if (R_FAILED(rc = ncmOpenContentMetaDatabase(&db, NcmStorageId_SdCard))) return failure("database", rc);
    rc = ncmContentMetaDatabaseSet(&db, &metaKey, package.metaData.data(), package.metaData.size());
    if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&db);
    ncmContentMetaDatabaseClose(&db);
    if (R_FAILED(rc)) return failure("meta", rc);
  }
  ContentStorageRecord record{};
  record.metaRecord = metaKey;
  record.storageId = NcmStorageId_SdCard;
  if (R_FAILED(rc = nsPushApplicationRecord(id, NsApplicationRecordType_Installed, &record, 1)))
    return failure("record", rc);
  refreshShortcutIcon(id);
  logLine("shortcut made " + forwarder::hexId(id) + " for " + input.nroPath);
  return {};
}
void refreshShortcutIcon(uint64_t id) {
  // ns application manager InvalidateApplicationControlCache, cmd 404, as
  // sphaira's ns.cpp.
  // From 3.0.0 libnx leaves that session unopened (0xE401, an invalid
  // handle): it is asked for its own, as CyberFoil's nsextInitialize does.
  Result rc = nsInitialize();
  if (R_SUCCEEDED(rc)) {
    if (hosversionBefore(3, 0, 0)) {
      rc = serviceDispatchIn(nsGetServiceSession_ApplicationManagerInterface(), 404, id);
    } else {
      Service manager{};
      if (R_SUCCEEDED(rc = nsGetApplicationManagerInterface(&manager))) {
        rc = serviceDispatchIn(&manager, 404, id);
        serviceClose(&manager);
      }
    }
    nsExit();
  }
  logLine("shortcut refresh " + forwarder::hexId(id) + " " + code(rc));
}
Error removeShortcut(uint64_t id) {
  Services services;
  if (R_FAILED(services.rc)) return failure("services", services.rc);
  const Result rc = nsDeleteApplicationCompletely(id);
  if (R_FAILED(rc) && rc != NotFound) return failure("remove", rc);
  logLine("shortcut removed " + forwarder::hexId(id));
  return {};
}
std::set<uint64_t> presentShortcuts(const std::vector<uint64_t> &ids) {
  std::set<uint64_t> out;
  if (ids.empty() || R_FAILED(nsInitialize())) return out;
  for (const auto id : ids)
    if (id && present(id)) out.insert(id);
  nsExit();
  return out;
}
#else
// Off the console: shortcuts are remembered for the run, so the screens and
// tests go through the same steps.
namespace {
std::mutex mutex;
std::set<uint64_t> made;
std::string blocker;
} // namespace
std::string shortcutBlocker() {
  std::lock_guard<std::mutex> lock(mutex);
  return blocker;
}
void setShortcutBlocker(const std::string &reason) {
  std::lock_guard<std::mutex> lock(mutex);
  blocker = reason;
}
Error makeShortcut(const ShortcutRequest &request, uint64_t &id) {
  if (auto reason = shortcutBlocker(); !reason.empty()) return {reason};
  if (request.icon.empty()) return {tr("The app file has no icon, so no shortcut can be made.")};
  if (request.nroPath.empty() || request.nacp.size() != forwarder::NacpSize)
    return {tr("Could not make a shortcut from the app file.")};
  id = forwarder::titleId(request.nroPath, request.extraArgs);
  std::lock_guard<std::mutex> lock(mutex);
  made.insert(id);
  return {};
}
void refreshShortcutIcon(uint64_t) {}
Error removeShortcut(uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex);
  made.erase(id);
  return {};
}
std::set<uint64_t> presentShortcuts(const std::vector<uint64_t> &ids) {
  std::lock_guard<std::mutex> lock(mutex);
  std::set<uint64_t> out;
  for (const auto id : ids)
    if (made.count(id)) out.insert(id);
  return out;
}
#endif
} // namespace foil::platform
