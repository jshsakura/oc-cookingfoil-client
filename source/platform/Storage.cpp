#include "platform/Storage.hpp"
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
  // Media playback state turns off auto-dimming and auto-sleep in both
  // applet and application mode, and is cleared when the app exits.
  appletSetMediaPlaybackState(on);
#else
  (void)on;
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
