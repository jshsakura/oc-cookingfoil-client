#include "platform/Storage.hpp"
#include "core/Json.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <cstring>
#include <string>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
bool applicationMode() {
#ifdef __SWITCH__
  return appletGetAppletType() == AppletType_Application;
#else
  return false;
#endif
}
std::string firmwareVersion() {
#ifdef __SWITCH__
  const u32 v = hosversionGet();
  return std::to_string(HOSVER_MAJOR(v)) + "." + std::to_string(HOSVER_MINOR(v)) + "." +
         std::to_string(HOSVER_MICRO(v));
#else
  // Off the console nothing blocks, unless a test or a preview names one.
  const char *named = std::getenv("FOIL_FIRMWARE");
  return named ? named : "";
#endif
}
std::string consoleRegion() {
#ifdef __SWITCH__
  // Read once: it never changes while the app runs, and every request to a
  // server says it.
  static const std::string region = [] {
    SetRegion code{};
    if (R_FAILED(setInitialize()) || R_FAILED(setGetRegionCode(&code))) return std::string();
    switch (code) {
    case SetRegion_JPN: return std::string("jpn");
    case SetRegion_USA: return std::string("usa");
    case SetRegion_EUR: return std::string("eur");
    case SetRegion_AUS: return std::string("aus");
    case SetRegion_HTK: return std::string("hkg-twn-kor");
    case SetRegion_CHN: return std::string("chn");
    default: return std::string();
    }
  }();
  return region;
#else
  const char *named = std::getenv("FOIL_REGION");
  return named ? named : "";
#endif
}
#ifndef __SWITCH__
namespace {
std::atomic<uint64_t> hostSpace{0};
} // namespace
// Off the console the storage has no size unless a test gives it one.
void setHostSpace(uint64_t bytes) { hostSpace = bytes; }
#endif
namespace {
std::optional<uint64_t> space(const std::string &target, bool total) {
#ifdef __SWITCH__
  FsFileSystem filesystem{};
  const auto rc =
      target == "nand"
          ? fsOpenContentStorageFileSystem(&filesystem, FsContentStorageId_User)
          : fsOpenSdCardFileSystem(&filesystem);
  if (R_FAILED(rc))
    return std::nullopt;
  s64 bytes = 0;
  const auto result = total ? fsFsGetTotalSpace(&filesystem, "/", &bytes)
                            : fsFsGetFreeSpace(&filesystem, "/", &bytes);
  fsFsClose(&filesystem);
  if (R_SUCCEEDED(result) && bytes >= 0)
    return uint64_t(bytes);
#else
  (void)target;
  (void)total;
  if (const uint64_t bytes = hostSpace)
    return bytes;
#endif
  return std::nullopt;
}
} // namespace
std::optional<uint64_t> freeSpace(const std::string &target) {
  return space(target, false);
}
std::optional<uint64_t> totalSpace(const std::string &target) {
  return space(target, true);
}
bool needsBigFile(uint64_t size) { return size > 0xFFFFFFFFull; }
bool createBigFile(const std::string &path, std::string &error) {
#ifdef __SWITCH__
  constexpr const char *Device = "sdmc:";
  if (path.rfind(Device, 0) != 0) {
    error = "not on the SD card";
    return false;
  }
  auto *filesystem = fsdevGetDeviceFileSystem("sdmc");
  if (!filesystem) {
    error = "no SD file system";
    return false;
  }
  const auto inner = path.substr(std::char_traits<char>::length(Device));
  if (inner.empty() || inner[0] != '/' || inner.size() >= FS_MAX_PATH) {
    error = "bad path";
    return false;
  }
  fsFsDeleteFile(filesystem, inner.c_str()); // an earlier file there, plain or split
  const Result rc = fsFsCreateFile(filesystem, inner.c_str(), 0, FsCreateOption_BigFile);
  if (R_FAILED(rc)) {
    char code[16];
    std::snprintf(code, sizeof code, "0x%x", unsigned(rc));
    error = code;
    return false;
  }
#else
  (void)path;
  (void)error;
#endif
  return true;
}
std::string systemLanguage() {
#ifdef __SWITCH__
  std::string code;
  if (R_FAILED(setInitialize()))
    return code;
  u64 raw = 0;
  SetLanguage lang{};
  if (R_SUCCEEDED(setGetSystemLanguage(&raw)) && R_SUCCEEDED(setMakeLanguage(raw, &lang))) {
    switch (lang) {
    case SetLanguage_KO: code = "ko"; break;
    case SetLanguage_JA: code = "ja"; break;
    case SetLanguage_ENUS:
    case SetLanguage_ENGB: code = "en"; break;
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS: code = "zh"; break;
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT: code = "zh-Hant"; break;
    case SetLanguage_ES:
    case SetLanguage_ES419: code = "es"; break;
    case SetLanguage_FR:
    case SetLanguage_FRCA: code = "fr"; break;
    case SetLanguage_DE: code = "de"; break;
    case SetLanguage_IT: code = "it"; break;
    case SetLanguage_PT:
    case SetLanguage_PTBR: code = "pt"; break;
    case SetLanguage_NL: code = "nl"; break;
    case SetLanguage_RU: code = "ru"; break;
    default: break;
    }
  }
  setExit();
  return code;
#else
  return {};
#endif
}
void keepAwake(bool on) {
#ifdef __SWITCH__
  // Media playback state keeps the screen from dimming in both modes; as an
  // application auto-sleep is also turned off outright, which media playback
  // alone does not do (a long download let the console fall asleep and drop
  // its connections). Both are cleared when the app exits.
  appletSetMediaPlaybackState(on);
  if (appletGetAppletType() == AppletType_Application)
    appletSetAutoSleepDisabled(on);
  // The system's own boost (CPU 1785 MHz) while downloading, as sphaira,
  // Awoo and TinWoo do: decompressing a compressed game
  // runs on the CPU.
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
#else
  (void)on;
#endif
}
bool wholeNro(const std::string &path) {
  std::unique_ptr<FILE, int (*)(FILE *)> in(std::fopen(path.c_str(), "rb"), std::fclose);
  if (!in) return false;
  unsigned char head[0x20] = {};
  if (std::fread(head, 1, sizeof head, in.get()) != sizeof head) return false;
  if (std::memcmp(head + 0x10, "NRO0", 4) != 0) return false;
  const uint64_t size = uint64_t(head[0x18]) | uint64_t(head[0x19]) << 8 | uint64_t(head[0x1A]) << 16 |
                        uint64_t(head[0x1B]) << 24;
  std::error_code ec;
  const auto onCard = std::filesystem::file_size(path, ec);
  return !ec && size >= 0x80 && onCard >= size;
}
bool finishUpdate(const std::string &self, std::string &note) {
  const std::string part = self + ".new", old = self + ".old";
  std::error_code ec;
  // The app found only as ".old" (a swap cut off half way): it comes back
  // before anything else.
  if (!std::filesystem::exists(self, ec) && wholeNro(old)) {
    std::filesystem::rename(old, self, ec);
    note = ec ? "app restore failed: " + ec.message() : "app restored from .old";
    ec.clear();
  }
  if (!std::filesystem::exists(part, ec)) return false;
  // Only a whole, checked build takes the app's place. A ".new" with a
  // resume note beside it is one an earlier version was still downloading
  // when it stopped: putting it in would leave no app to start.
  if (std::filesystem::exists(part + ".resume", ec) || !wholeNro(part)) {
    std::filesystem::remove(part, ec);
    std::filesystem::remove(part + ".resume", ec);
    note = "update not swapped in: the downloaded build was not whole";
    std::string ignored;
    atomicWrite(self + ".updatefailed", "partial", ignored);
    return false;
  }
  std::filesystem::remove(old, ec);
  ec.clear();
  // The card does not rename over an existing file: the old one goes aside
  // first, and comes back if the new one cannot take its place.
  std::filesystem::rename(self, old, ec);
  if (ec) {
    note = "update swap failed: the app's file could not be set aside: " + ec.message();
    std::string ignored;
    atomicWrite(self + ".updatefailed", "swap", ignored);
    return false;
  }
  std::filesystem::rename(part, self, ec);
  if (ec || !wholeNro(self)) {
    note = "update swap failed: " + (ec ? ec.message() : std::string("the new build did not land whole"));
    std::error_code ignored;
    std::filesystem::remove(self, ignored);
    ignored.clear();
    std::filesystem::rename(old, self, ignored);
    // The rename back refused too: a copy, so an app is there to start.
    if (ignored) std::filesystem::copy_file(old, self, std::filesystem::copy_options::overwrite_existing, ignored);
    if (!ignored) std::filesystem::remove(old, ignored);
    std::string unused;
    atomicWrite(self + ".updatefailed", "swap", unused);
    return false;
  }
  // The build it replaced stays as ".old" until the new one has run a while
  // (confirmUpdate); one that dies before that is undone (rollBackUpdate).
  std::string ignored;
  atomicWrite(self + ".trial", "0", ignored);
  note = "update swapped in: " + self;
  return true;
}
bool rollBackUpdate(const std::string &self, std::string &note) {
  const std::string trial = self + ".trial", old = self + ".old";
  std::error_code ec;
  if (!std::filesystem::exists(trial, ec)) return false;
  // The first start of a new build marks its trial; a second start that
  // finds it marked means the first never got as far as confirming.
  if (readFile(trial) != "1" || !wholeNro(old)) {
    std::string ignored;
    if (!wholeNro(old)) std::filesystem::remove(trial, ec);
    else atomicWrite(trial, "1", ignored);
    return false;
  }
  std::filesystem::remove(self, ec);
  ec.clear();
  std::filesystem::rename(old, self, ec);
  if (ec) {
    ec.clear();
    std::filesystem::copy_file(old, self, std::filesystem::copy_options::overwrite_existing, ec);
  }
  if (ec || !wholeNro(self)) {
    note = "update roll back failed: " + ec.message();
    return false;
  }
  std::filesystem::remove(old, ec);
  std::filesystem::remove(trial, ec);
  std::string ignored;
  // Which build stopped, and how many times in a row: twice and it is not
  // taken again (refusedBuild), so the console does not loop on it.
  const auto version = readFile(self + ".pending");
  std::filesystem::remove(self + ".pending", ec);
  if (!version.empty()) {
    const auto before = readFile(self + ".refused");
    const auto newline = before.find('\n');
    int times = 1;
    if (newline != std::string::npos && before.substr(0, newline) == version)
      times = std::atoi(before.c_str() + newline + 1) + 1;
    atomicWrite(self + ".refused", version + "\n" + std::to_string(times), ignored);
  }
  atomicWrite(self + ".rolledback", version.empty() ? "1" : version, ignored); // said on screen once the app is up
  note = "update rolled back: the new build stopped before it was up";
  return true;
}
void confirmUpdate(const std::string &self) {
  std::error_code ec;
  if (!std::filesystem::exists(self + ".trial", ec)) return;
  std::filesystem::remove(self + ".old", ec);
  std::filesystem::remove(self + ".trial", ec);
  std::filesystem::remove(self + ".pending", ec);
  std::filesystem::remove(self + ".refused", ec); // a build came up: the count starts over
}
void noteTrialVersion(const std::string &self, const std::string &version) {
  std::string ignored;
  atomicWrite(self + ".pending", version, ignored);
}
std::string refusedBuild(const std::string &self) {
  const auto record = readFile(self + ".refused");
  const auto newline = record.find('\n');
  if (newline == std::string::npos || std::atoi(record.c_str() + newline + 1) < 2) return "";
  return record.substr(0, newline);
}
std::string runEnvironment() {
#ifdef __SWITCH__
  std::string out;
  u64 api = 0;
  if (R_SUCCEEDED(splInitialize())) {
    // 65000: Atmosphere's own config item, its version in the top bytes.
    if (R_SUCCEEDED(splGetConfig(SplConfigItem(65000), &api)))
      out = "atmosphere " + std::to_string((api >> 56) & 0xFF) + "." + std::to_string((api >> 48) & 0xFF) + "." +
            std::to_string((api >> 40) & 0xFF);
    splExit();
  }
  u64 program = 0;
  if (R_SUCCEEDED(svcGetInfo(&program, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0))) {
    char id[17];
    std::snprintf(id, sizeof id, "%016llX", static_cast<unsigned long long>(program));
    out += std::string(out.empty() ? "" : "; ") + "program " + id;
  }
  out += std::string(out.empty() ? "" : "; ") + (appletGetAppletType() == AppletType_Application ? "application" : "applet") +
         "; firmware " + firmwareVersion();
  return out;
#else
  return "";
#endif
}
bool serviceApplet() {
#ifdef __SWITCH__
  return appletMainLoop();
#else
  return true;
#endif
}
bool relaunch(const std::string &nro) {
#ifdef __SWITCH__
  if (!envHasNextLoad()) return false;
  return R_SUCCEEDED(envSetNextLoad(nro.c_str(), nro.c_str()));
#else
  (void)nro;
  return false;
#endif
}
} // namespace foil::platform
