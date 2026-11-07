#pragma once
namespace foil::platform {
// The console's charge, -1 when it has none to tell (the host without
// FOIL_BATTERY, or psm not answering).
struct Battery {
  int percent = -1;
  bool charging = false;
};
Battery battery();
} // namespace foil::platform
