#include "platform/Clock.hpp"
#include <cstdio>
#include <cstdlib>
#include <ctime>
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
// time:s, the service with the right to set a clock, on its own session:
// the one libnx opened at start (time:u) only reads, and stays as it is.
enum : u32 { UserClock = 0, NetworkClock = 1 }; // time:s GetStandard*SystemClock
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
  Service time;
  const Result opened = smGetService(&time, "time:s");
  if (R_FAILED(opened)) {
    result.detail = "time:s " + hex(opened);
    return result;
  }
  const Result network = setOne(time, NetworkClock, u64(unixSeconds));
  const Result user = setOne(time, UserClock, u64(unixSeconds));
  serviceClose(&time);
  const auto after = int64_t(std::time(nullptr));
  result.ok = unixSeconds > 0 && std::llabs(after - unixSeconds) < 60;
  result.detail = "network " + hex(network) + " user " + hex(user) + " clock " + std::to_string(before) + " -> " +
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
