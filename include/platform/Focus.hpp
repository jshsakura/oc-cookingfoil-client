#pragma once
// The app leaving the screen: HOME, the console going to sleep, another
// applet over it. The system suspends the app right after, so what must be
// on the card by then is written in `leaving`, on the main thread, before
// the suspension. Nothing on the host.
#include <functional>
namespace foil::platform {
void onLeavingScreen(std::function<void()> leaving);
void stopWatchingScreen();
} // namespace foil::platform
