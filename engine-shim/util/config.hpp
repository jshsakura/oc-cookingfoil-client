#pragma once
#include <string>
namespace inst::config {
inline const std::string appDir = "sdmc:/switch/cookingfoil",
                         appVersion = "0.1.0";
// NCA signatures are not checked: a broken download fails on its hash
// (nca_writer's check against the NCA's id) instead.
inline bool validateNCAs = false, soundEnabled = false, remoteLegacyMode = false;
// CookingFoil: the file came changed by a server plugin (hashesChange): its
// NCAs no longer hash to their ids, which is expected, not a broken resume.
inline bool hashesChange = false;
inline const std::string appGitMeta = "", appVersionFull = "0.1.0";
} // namespace inst::config
