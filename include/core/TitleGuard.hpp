#pragma once
// What this app may install, whatever the signature check says: with it
// skipped (the default), nothing else stops a package from naming a system
// title, and installing over one can leave the console unable to start.
#include <cstdint>
#include <string>
namespace foil {
// Every id below this one is the system's (programs, data, applets, the
// HOME Menu at 0100000000001000); games and their updates and DLC sit above.
constexpr uint64_t FirstApplicationId = 0x0100000000010000ULL;
// Applications, their updates, DLC and data patches; never a system program,
// system data, a system update or a boot image.
inline bool installableType(uint8_t metaType) {
  return metaType == 0x80 || metaType == 0x81 || metaType == 0x82 || metaType == 0x84;
}
inline bool installableTitle(uint64_t titleId) { return titleId >= FirstApplicationId; }
inline bool installAllowed(uint64_t titleId, uint8_t metaType) {
  return installableTitle(titleId) && installableType(metaType);
}
// A catalog's Title ID ("0100…", 16 hex digits): false only when it names
// one this app must not install; an empty or unreadable one is left to the
// package's own record, checked as it installs.
inline bool catalogTitleAllowed(const std::string &hex) {
  if (hex.size() != 16 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return true;
  return installableTitle(std::stoull(hex, nullptr, 16));
}
} // namespace foil
