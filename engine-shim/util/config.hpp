#pragma once
#include <string>
namespace inst::config {
inline const std::string appDir = "sdmc:/switch/cookingfoil",
                         appVersion = "0.1.0";
inline bool validateNCAs = true, soundEnabled = false, remoteLegacyMode = false;
inline const std::string appGitMeta = "", appVersionFull = "0.1.0";
} // namespace inst::config
