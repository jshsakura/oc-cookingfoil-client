#include "platform/Power.hpp"
#include <cstdlib>
#include <string>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
Battery battery() {
#ifdef __SWITCH__
  // psm answers from memory: cheap enough for the footer's once a second.
  u32 percent = 0;
  PsmChargerType charger = PsmChargerType_Unconnected;
  if (R_FAILED(psmGetBatteryChargePercentage(&percent))) return {};
  if (R_FAILED(psmGetChargerType(&charger))) charger = PsmChargerType_Unconnected;
  return {int(percent > 100 ? 100 : percent), charger != PsmChargerType_Unconnected};
#else
  // Off the console nothing is drawn, unless a test or a preview names a
  // charge: "76", or "15+" while charging.
  const char *named = std::getenv("FOIL_BATTERY");
  if (!named || !*named) return {};
  const std::string text = named;
  const int percent = std::atoi(text.c_str());
  return {percent < 0 ? 0 : percent > 100 ? 100 : percent, text.back() == '+'};
#endif
}
} // namespace foil::platform
