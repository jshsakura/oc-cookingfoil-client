#pragma once
#include "core/State.hpp"
namespace foil::platform {
// This console's identity for binding the app's keys (core/State): derived
// from the console's device ID, which is read here and kept nowhere. The
// PC preview uses a fixed stand-in.
DeviceIdentity consoleIdentity();
} // namespace foil::platform
