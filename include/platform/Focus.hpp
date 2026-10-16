#pragma once
// The app leaving the screen: HOME, the console going to sleep, another
// applet over it. The system may suspend the app right after (sleep always;
// HOME unless it keeps running, below), so what must be on the card by then
// is written in `leaving`, on the main thread, first. Nothing on the host.
#include <functional>
namespace foil::platform {
void onLeavingScreen(std::function<void()> leaving);
void stopWatchingScreen();
// Launched as an application, the app keeps running under the HOME Menu
// (downloads go on; only sleep still stops it). An applet (album) cannot.
void keepRunningInBackground();
// On screen now. While it is not, nothing is drawn (the main loop only
// keeps the queue moving). Always true on the host.
bool onScreen();
} // namespace foil::platform
