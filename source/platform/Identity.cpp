#include "platform/Identity.hpp"
#include <algorithm>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
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
} // namespace foil::platform
