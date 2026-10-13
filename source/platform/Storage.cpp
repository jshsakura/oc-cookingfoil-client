#include "platform/Storage.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
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
  return "";
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
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANS:
    case SetLanguage_ZHHANT: code = "zh"; break;
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
  // Awoo and TinWoo do: decompressing and re-encrypting a compressed game
  // runs on the CPU.
  appletSetCpuBoostMode(on ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
#else
  (void)on;
#endif
}
bool finishUpdate(const std::string &self, std::string &note) {
  const std::string part = self + ".new", old = self + ".old";
  std::error_code ec;
  if (!std::filesystem::exists(part, ec)) return false;
  std::filesystem::remove(old, ec);
  ec.clear();
  // The card does not rename over an existing file: the old one goes aside
  // first, and comes back if the new one cannot take its place.
  std::filesystem::rename(self, old, ec);
  const bool hadOld = !ec;
  ec.clear();
  std::filesystem::rename(part, self, ec);
  if (ec) {
    note = "update swap failed: " + ec.message();
    std::error_code ignored;
    if (hadOld) std::filesystem::rename(old, self, ignored);
    return false;
  }
  std::filesystem::remove(old, ec);
  note = "update swapped in: " + self;
  return true;
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
