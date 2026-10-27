#include "platform/Identity.hpp"
#include <algorithm>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
#ifndef __SWITCH__
namespace {
bool hostDark = false; // the PC preview's console theme, from --theme
}
#endif
DeviceIdentity consoleIdentity() {
#ifdef __SWITCH__
  // The device ID fused into the console (spl), and nothing else: another
  // source would make another fingerprint and key, which read as another
  // console. It does not leave this function.
  u64 id = 0;
  if (R_SUCCEEDED(splInitialize())) {
    if (R_FAILED(splGetConfig(SplConfigItem_DeviceId, &id)))
      id = 0;
    splExit();
  }
  auto identity = deriveIdentity(id);
  id = 0;
  return identity;
#else
  return deriveIdentity(0x5043207072657669ull); // "PC previ": a stand-in
#endif
}
std::string consoleNickname() {
#ifdef __SWITCH__
  std::string name;
  if (R_FAILED(setsysInitialize())) return name;
  SetSysDeviceNickName nick{};
  if (R_SUCCEEDED(setsysGetDeviceNickname(&nick))) {
    name.assign(nick.nickname, sizeof nick.nickname);
    name.resize(std::min(name.find('\0'), name.size()));
  }
  setsysExit();
  return name;
#else
  return "";
#endif
}
bool consoleDark() {
#ifdef __SWITCH__
  if (R_FAILED(setsysInitialize())) return false;
  ColorSetId id = ColorSetId_Light;
  const bool dark = R_SUCCEEDED(setsysGetColorSetId(&id)) && id == ColorSetId_Dark;
  setsysExit();
  return dark;
#else
  return hostDark;
#endif
}
#ifndef __SWITCH__
void setHostDark(bool dark) { hostDark = dark; }
#endif
} // namespace foil::platform
