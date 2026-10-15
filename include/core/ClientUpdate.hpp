#pragma once
// The app updating itself: what the server offers (/api/client/latest) and
// whether it is newer than this build.
#include "core/Types.hpp"
#include <optional>
#include <string>
namespace foil::update {
struct Release {
  std::string version, url, sha256; // sha256: 64 lowercase hex digits
  uint64_t size = 0;
};
// Compares dotted numbers ("0.1.10" > "0.1.9"); anything after '-' or '+'
// is ignored. Negative, zero or positive like strcmp.
int compareVersions(const std::string &a, const std::string &b);
// URLs are absolute or server paths joined to `base`; nullopt when anything
// needed is missing or out of range.
std::optional<Release> parseRelease(const std::string &json, const std::string &base);
// Lowercase hex of the SHA-256 of `bytes`.
std::string sha256Hex(const std::string &bytes);
} // namespace foil::update
