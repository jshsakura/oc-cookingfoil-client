#include "platform/Identity.hpp"
#include <cstdio>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
DeviceIdentity consoleIdentity() {
#ifdef __SWITCH__
  // The device ID fused into the console (spl), and nothing else: another
  // source would make another fingerprint, which reads as another console.
  // It does not leave this function.
  std::string value;
  if (R_SUCCEEDED(splInitialize())) {
    u64 id = 0;
    if (R_SUCCEEDED(splGetConfig(SplConfigItem_DeviceId, &id)) && id) {
      char text[17];
      std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(id));
      value = std::string("device-id ") + text;
    }
    splExit();
  }
  auto identity = deriveIdentity(value);
  value.assign(value.size(), '\0');
  return identity;
#else
  return deriveIdentity("pc preview");
#endif
}
} // namespace foil::platform
