#pragma once
#include <string>
namespace inst::config {
inline const std::string appDir = "sdmc:/switch/cookingfoil",
                         appVersion = "0.1.0";
inline bool validateNCAs = true, soundEnabled = false, remoteLegacyMode = false;
// CookingFoil: the file came re-encrypted for this console (?rewrap=): its
// NCAs no longer hash to their ids, which is expected, not a broken resume.
inline bool rewrapped = false;
inline const std::string appGitMeta = "", appVersionFull = "0.1.0";
} // namespace inst::config
