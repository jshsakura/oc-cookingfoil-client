#pragma once
#include <cstdint>
#include <optional>
#include <string>
namespace foil::platform {
bool applicationMode();
// The console's system version as "18.1.0"; empty off the console.
std::string firmwareVersion();
std::optional<uint64_t> freeSpace(const std::string &target);
std::optional<uint64_t> totalSpace(const std::string &target);
// The console's system language as ko, en, ja or zh; empty when unknown or
// none of those.
std::string systemLanguage();
// While on, the console does not dim the screen or fall asleep on its own
// (used during downloads).
void keepAwake(bool on);
// Puts the downloaded build `part` in place of `target`, the running app's
// own file. The app's RomFS keeps that file open, so it is closed for the
// swap and opened again after; `error` gets the system's reason on failure.
bool replaceSelf(const std::string &part, const std::string &target, std::string &error);
// Starts `nro` again once this app exits (the homebrew loader's next load);
// false where that is not possible (off the console, or no loader).
bool relaunch(const std::string &nro);
// Lets the console handle HOME and sleep while the main thread is busy;
// false once the console asks the app to close.
bool serviceApplet();
} // namespace foil::platform
