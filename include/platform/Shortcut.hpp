#pragma once
// HOME Menu shortcuts made on the console (core/Forwarder builds them,
// this writes them with NCM and NS). Off the console a stand-in keeps them
// in memory so the screens can be tried and tested.
#include "core/Types.hpp"
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace foil::platform {
struct ShortcutRequest {
  std::string nroPath;   // forwarder::loaderPath() form: "/switch/app.nro"
  std::string extraArgs; // after the path in argv (a retro game's ROM)
  std::string name, author; // empty keeps the NACP's own
  std::string nacp, icon;   // the .nro's (forwarder::blankNacp() when it has none)
};
// Why shortcuts cannot be made here, judged once before anything is tried:
// the loader missing from the app's RomFS, or a console not booted into
// Atmosphere (its crypto service will not make the NCA header key). Empty
// when they can.
std::string shortcutBlocker();
inline bool canMakeShortcuts() { return shortcutBlocker().empty(); }
#ifndef __SWITCH__
// Off the console, for tests: the reason shortcutBlocker() gives.
void setShortcutBlocker(const std::string &reason);
#endif
// Makes the shortcut, replacing one made before for the same path; `id` gets
// its title id. The message, when it fails, is for the player.
Error makeShortcut(const ShortcutRequest &request, uint64_t &id);
Error removeShortcut(uint64_t id);
// The ones of `ids` the console has installed.
std::set<uint64_t> presentShortcuts(const std::vector<uint64_t> &ids);
} // namespace foil::platform
