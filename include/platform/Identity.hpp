#pragma once
#include "core/State.hpp"
namespace foil::platform {
// This console's identity for binding the app's keys (core/State): derived
// from the console's device ID, which is read here and kept nowhere. Empty
// when it could not be read; the PC preview uses a fixed stand-in.
DeviceIdentity consoleIdentity();
// The console's nickname from System Settings; empty when it could not be
// read. The PC preview has none.
std::string consoleNickname();
} // namespace foil::platform
