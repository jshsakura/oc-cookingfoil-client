#include "Test.hpp"
#include "platform/Power.hpp"
#include "ui/Painter.hpp"
#include "ui/Theme.hpp"
#include <cstdlib>
int runBatteryTests(SDL_Renderer *renderer) {
  using foil::ui::BatteryTone;
  using foil::ui::batteryTone;
  int failures = 0;
  // Color only when it matters: amber from 30%, red from 15%, green while
  // charging however low.
  CHECK(batteryTone(100, false) == BatteryTone::Plain);
  CHECK(batteryTone(31, false) == BatteryTone::Plain);
  CHECK(batteryTone(30, false) == BatteryTone::Low);
  CHECK(batteryTone(16, false) == BatteryTone::Low);
  CHECK(batteryTone(15, false) == BatteryTone::Empty);
  CHECK(batteryTone(0, false) == BatteryTone::Empty);
  CHECK(batteryTone(5, true) == BatteryTone::Charging);
  // Off the console the charge comes only from FOIL_BATTERY.
  unsetenv("FOIL_BATTERY");
  CHECK(foil::platform::battery().percent == -1);
  setenv("FOIL_BATTERY", "76", 1);
  CHECK(foil::platform::battery().percent == 76 && !foil::platform::battery().charging);
  setenv("FOIL_BATTERY", "15+", 1);
  CHECK(foil::platform::battery().percent == 15 && foil::platform::battery().charging);
  setenv("FOIL_BATTERY", "140", 1);
  CHECK(foil::platform::battery().percent == 100);
  unsetenv("FOIL_BATTERY");
  // Unknown takes no room; the bolt adds its own width before the body.
  foil::ui::Painter painter(renderer);
  CHECK(painter.battery(0, 690, -1, false, true, foil::ui::Surface) == 0);
  const float plain = painter.battery(0, 690, 50, false, true, foil::ui::Surface);
  const float charging = painter.battery(0, 690, 50, true, true, foil::ui::Surface);
  CHECK(plain > 0 && charging > plain);
  CHECK(painter.battery(0, 690, 50, false, false, foil::ui::Surface) < plain);
  if (failures == 0) std::puts("Battery tests passed");
  return failures;
}
