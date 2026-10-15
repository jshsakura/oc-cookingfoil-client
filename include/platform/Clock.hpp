#pragma once
#include <cstdint>
#include <string>
namespace foil::platform {
struct ClockSet {
  bool ok = false;
  std::string detail; // what each step answered, for the log
};
// Sets the console's clock to `unixSeconds` (UTC), asked for by the player:
// the network clock, which the console follows with internet time sync on,
// and the user clock, which it follows with sync off. The region and time
// zone stay as they are. OK when the clock the app reads then shows it.
// Nothing off the console.
ClockSet setClock(int64_t unixSeconds);
// "2026-10-15 09:30" in the console's own time zone.
std::string localTime(int64_t unixSeconds);
} // namespace foil::platform
