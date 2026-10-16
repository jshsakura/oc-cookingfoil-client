#include "platform/Focus.hpp"
#ifdef __SWITCH__
#include <switch.h>
#endif
namespace foil::platform {
namespace {
std::function<void()> callback;
#ifdef __SWITCH__
AppletHookCookie cookie;
bool hooked = false;
#endif
} // namespace
void onLeavingScreen(std::function<void()> leaving) {
  callback = std::move(leaving);
#ifdef __SWITCH__
  if (hooked) return;
  // Runs inside appletMainLoop when the focus changes, before the app is
  // suspended for it.
  appletHook(&cookie, [](AppletHookType hook, void *) {
    if (hook == AppletHookType_OnFocusState && appletGetFocusState() != AppletFocusState_InFocus && callback)
      callback();
  }, nullptr);
  hooked = true;
#endif
}
void stopWatchingScreen() {
#ifdef __SWITCH__
  if (hooked) appletUnhook(&cookie);
  hooked = false;
#endif
  callback = nullptr;
}
} // namespace foil::platform
