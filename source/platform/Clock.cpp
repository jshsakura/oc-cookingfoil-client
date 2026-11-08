#include "platform/Clock.hpp"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <vector>
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
namespace {
std::string hex(unsigned value) {
  char text[16];
  std::snprintf(text, sizeof text, "0x%X", value);
  return text;
}
} // namespace
#ifdef __SWITCH__
namespace {
// time:s and time:a each may set only some clocks (firmware 19.0.1: time:s
// the network clock, time:a the local and user clocks, both refusing the
// rest with 0x274): each clock is set on whichever lets it. Both are opened
// on their own sessions; the one libnx opened at start (time:u) only reads.
// The user clock (what the console shows) is the local clock while automatic
// correction is off. With it on (firmware 23.0.1) the local clock refuses
// too: correction goes off first, as System Settings does before a date is
// set by hand.
enum : u32 { UserClock = 0, NetworkClock = 1, LocalClock = 4 }; // time:s GetStandard*SystemClock
enum : u32 { IsAutomaticCorrection = 100, SetAutomaticCorrection = 101 }; // time:s
enum : u32 { SetCurrentTime = 1 };              // ISystemClock
Result setOne(Service &time, u32 which, u64 seconds) {
  Service clock;
  Result rc = serviceDispatch(&time, which, .out_num_objects = 1, .out_objects = &clock);
  if (R_FAILED(rc)) return rc;
  rc = serviceDispatchIn(&clock, SetCurrentTime, seconds);
  serviceClose(&clock);
  return rc;
}
} // namespace
ClockSet setClock(int64_t unixSeconds) {
  ClockSet result;
  const auto before = int64_t(std::time(nullptr));
  Service admin{}, system{};
  const Result openedAdmin = smGetService(&admin, "time:a"), openedSystem = smGetService(&system, "time:s");
  std::vector<Service *> services;
  if (R_SUCCEEDED(openedAdmin)) services.push_back(&admin);
  if (R_SUCCEEDED(openedSystem)) services.push_back(&system);
  if (services.empty()) {
    result.detail = "time:a " + hex(openedAdmin) + " time:s " + hex(openedSystem);
    return result;
  }
  // The first that lets it, or the last refusal.
  const auto set = [&](u32 which) {
    Result rc = 0;
    for (auto *time : services)
      if (R_SUCCEEDED(rc = setOne(*time, which, u64(unixSeconds)))) break;
    return rc;
  };
  const Result network = set(NetworkClock);
  Result local = set(LocalClock);
  std::string automatic;
  if (R_FAILED(local)) {
    for (auto *time : services) {
      u8 on = 0;
      if (R_FAILED(serviceDispatchOut(time, IsAutomaticCorrection, on)) || !on) continue;
      const u8 disabled = 0;
      const Result off = serviceDispatchIn(time, SetAutomaticCorrection, disabled);
      automatic = " automatic off " + hex(off);
      if (R_SUCCEEDED(off)) {
        local = set(LocalClock);
        break;
      }
    }
  }
  const Result user = set(UserClock);
  for (auto *time : services) serviceClose(time);
  const auto after = int64_t(std::time(nullptr));
  result.ok = unixSeconds > 0 && std::llabs(after - unixSeconds) < 60;
  result.detail = std::string(R_SUCCEEDED(openedAdmin) ? "" : "time:a " + hex(openedAdmin) + " ") + "network " + hex(network) +
                  automatic + " local " + hex(local) + " user " + hex(user) + " clock " + std::to_string(before) + " -> " +
                  std::to_string(after) + " target " + std::to_string(unixSeconds);
  return result;
}
std::string localTime(int64_t unixSeconds) {
  TimeCalendarTime at{};
  TimeCalendarAdditionalInfo info{};
  if (R_FAILED(timeToCalendarTimeWithMyRule(u64(unixSeconds), &at, &info))) return std::to_string(unixSeconds);
  char text[32];
  std::snprintf(text, sizeof text, "%04d-%02d-%02d %02d:%02d", at.year, at.month, at.day, at.hour, at.minute);
  return text;
}
#else
ClockSet setClock(int64_t) { return {false, "the PC preview has no console clock " + hex(0)}; }
std::string localTime(int64_t unixSeconds) {
  const std::time_t at = std::time_t(unixSeconds);
  std::tm local{};
  localtime_r(&at, &local);
  char text[32];
  std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", &local);
  return text;
}
#endif
} // namespace foil::platform
