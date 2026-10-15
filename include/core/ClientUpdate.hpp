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
  std::string signature; // Ed25519 over signedMessage(), 128 hex digits; empty when unsigned
};
// The release key's public half (Ed25519, 32 bytes as hex). Its private half
// is kept off this repository; scripts/sign-release.sh signs with it.
constexpr const char *ReleaseKey = "8ab0978418ee9b1a942e06cc5ca4d1b45a4491a212f28508a1a32b9f7a1fccf5";
// What a release's signature covers: the version, the size and the hash of
// the file, so a signed build cannot be passed off as another version and
// no other file passes as it.
std::string signedMessage(const Release &release);
// Whether the release carries a valid signature by `publicKey` (raw bytes;
// the release key when empty). Nothing unsigned is ever offered or put in
// place.
bool signatureValid(const Release &release, const std::string &publicKey = "");
// Compares dotted numbers ("0.1.10" > "0.1.9"); anything after '-' or '+'
// is ignored. Negative, zero or positive like strcmp.
int compareVersions(const std::string &a, const std::string &b);
// URLs are absolute or server paths joined to `base`; nullopt when anything
// needed is missing or out of range.
std::optional<Release> parseRelease(const std::string &json, const std::string &base);
// Lowercase hex of the SHA-256 of `bytes`.
std::string sha256Hex(const std::string &bytes);
} // namespace foil::update
