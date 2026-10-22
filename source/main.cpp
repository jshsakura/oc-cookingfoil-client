#include "platform/Storage.hpp"
#include "net/Socket.hpp"
#include "net/Api.hpp"
#include "ui/Application.hpp"
#include "core/Json.hpp"
#include "ui/Runtime.hpp"
#include "core/Log.hpp"
#include "core/I18n.hpp"
#include "platform/Identity.hpp"
#include "platform/Focus.hpp"
// Included for the build's version stamp: a new version rebuilds the files
// that include it, so "start <version>" and the update marker say this build.
#include "core/Version.hpp"
#include <cmath>
#include <vector>
#include <curl/curl.h>
#include <switch.h>
static void openSearchKeyboard(foil::ui::Application &ui) {
  SwkbdConfig keyboard;
  if (R_FAILED(swkbdCreate(&keyboard, 0)))
    return;
  swkbdConfigMakePresetDefault(&keyboard);
  // The default preset is QWERTY only. All lets the player switch to Korean.
  swkbdConfigSetType(&keyboard, SwkbdType_All);
  swkbdConfigSetGuideText(&keyboard, foil::tr("Enter a game name"));
  swkbdConfigSetInitialText(&keyboard, ui.query().c_str());
  char text[512]{};
  if (R_SUCCEEDED(swkbdShow(&keyboard, text, sizeof(text))))
    ui.search(text);
  swkbdClose(&keyboard);
}
// A field the app asked for (the server form); OK hands the text back,
// cancel leaves the field as it was.
static void openTextKeyboard(foil::ui::Application &ui, const foil::ui::Application::TextRequest &request) {
  SwkbdConfig keyboard;
  if (R_FAILED(swkbdCreate(&keyboard, 0)))
    return;
  if (request.password) {
    // Typed hidden, on the Latin keyboard a password is made of.
    swkbdConfigMakePresetPassword(&keyboard);
  } else {
    swkbdConfigMakePresetDefault(&keyboard);
    swkbdConfigSetType(&keyboard, SwkbdType_All);
  }
  swkbdConfigSetGuideText(&keyboard, request.guide.c_str());
  swkbdConfigSetInitialText(&keyboard, request.initial.c_str());
  swkbdConfigSetStringLenMax(&keyboard, u32(request.maxLength));
  std::vector<char> text(size_t(request.maxLength) * 4 + 1); // UTF-8, up to 4 bytes a character
  if (R_SUCCEEDED(swkbdShow(&keyboard, text.data(), text.size())))
    ui.enterText(text.data());
  swkbdClose(&keyboard);
}
// Every button pressed since the last frame, from the controllers' recent
// samples (they keep 17): reading only the newest state lost a quick tap
// whenever a frame ran long.
struct PadHistory {
  u64 seen[2][3] = {};
  bool primed[2][3] = {};
};
static u64 newPresses(PadHistory &history) {
  static const HidNpadIdType ids[] = {HidNpadIdType_Handheld, HidNpadIdType_No1};
  u64 down = 0;
  for (int i = 0; i < 2; ++i) {
    const u32 styles = hidGetNpadStyleSet(ids[i]);
    for (int style = 0; style < 3; ++style) { // FullKey, Handheld, JoyDual: style bits 0, 1, 2
      if (!(styles & (1u << style)))
        continue;
      HidNpadCommonState states[17];
      const size_t count = style == 0   ? hidGetNpadStatesFullKey(ids[i], states, 17)
                           : style == 1 ? hidGetNpadStatesHandheld(ids[i], states, 17)
                                        : hidGetNpadStatesJoyDual(ids[i], states, 17);
      if (!count)
        continue;
      // Newest first: each sample after the last frame against the one before it.
      if (history.primed[i][style])
        for (size_t k = 0; k + 1 < count && states[k].sampling_number > history.seen[i][style]; ++k)
          down |= states[k].buttons & ~states[k + 1].buttons;
      history.seen[i][style] = states[0].sampling_number;
      history.primed[i][style] = true;
    }
  }
  return down;
}
int main(int argc, char **argv) {
  // TCP receive windows above libnx's defaults (64 KB growing to 256 KB).
  // A socket starts at tcp_rx_buf_size and the stack grows it as a transfer
  // needs, up to tcp_rx_buf_max_size, as long as nobody sets SO_RCVBUF
  // (that pins it; it pinned every download at 1 MB until 0.2.171). With 4
  // MB as the starting size it made no sockets at all as an application
  // (every request failed with curl 7), so the start stays at 1 MB, the
  // values 0.2.155 ran with. The transfer memory is sb_efficiency times the
  // largest buffers, taken from the heap.
  // An update waiting beside the app goes in first, before the app's own file
  // is opened (romfs, fonts); then the loader starts the new build.
  const std::string self = argc > 0 && argv[0] && std::string(argv[0]).rfind("sdmc:/", 0) == 0
                               ? std::string(argv[0])
                               : std::string("sdmc:/switch/cookingfoil/cookingfoil.nro");
  std::string updateNote;
  const bool rolledBack = foil::platform::rollBackUpdate(self, updateNote);
  const bool swapped = !rolledBack && foil::platform::finishUpdate(self, updateNote);
  if (swapped) { // the new build says it came, and from which one
    std::string ignored;
    foil::atomicWrite(self + ".updated", COOKINGFOIL_VERSION, ignored);
  }
  if ((rolledBack || swapped) && envHasNextLoad() && R_SUCCEEDED(envSetNextLoad(self.c_str(), self.c_str())))
    return 0;
  SocketInitConfig sockets = *socketGetDefaultInitConfig();
  const bool application = appletGetAppletType() == AppletType_Application;
  sockets.tcp_rx_buf_size = 0x100000;
  sockets.tcp_rx_buf_max_size = application ? 0x1000000 : 0x400000;
  sockets.sb_efficiency = application ? 4 : 2;
  foil::net::setReceiveWindow(int(sockets.tcp_rx_buf_max_size));
  if (R_FAILED(socketInitialize(&sockets)))
    return 1;
  const bool nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  // Randomness for TLS (platform/TlsPlatform.cpp) and the Dropbox sign-in.
  const bool csrng = R_SUCCEEDED(csrngInitialize());
  if (R_FAILED(romfsInit())) {
    socketExit();
    return 1;
  }
  curl_global_init(CURL_GLOBAL_DEFAULT);
  // The app's keys are bound to this console (core/State).
  foil::setIdentitySource(foil::platform::consoleIdentity);
  foil::net::setConsoleName(foil::platform::consoleNickname());
  foil::openLog("sdmc:/switch/cookingfoil/log.txt");
  if (!updateNote.empty() && !swapped) foil::logLine(updateNote); // a swap that went well says nothing
  int result = 0;
  {
    foil::ui::Runtime app;
    if (!app.open(1280, 720, false))
      result = 1;
    else {
      foil::ui::Application ui(app.renderer(),
                               "sdmc:/switch/cookingfoil/config.json",
                               "sdmc:/switch/cookingfoil/cookingfoil.db");
      if (!ui.open())
        return 1;
      // hbmenu and forwarders pass the .nro's path; an update replaces it.
      if (argc > 0 && argv[0])
        ui.setSelfPath(argv[0]);
      PadState pad;
      PadHistory history;
      padConfigureInput(1, HidNpadStyleSet_NpadStandard);
      padInitializeDefault(&pad);
      uint32_t repeatAt = 0, stickAt = 0;
      float touchX = 0, touchY = 0, lastX = 0, lastY = 0;
      bool dragging = false;
      // Past its startup and running a moment: a build just updated to is
      // kept, on screen or not (closing from HOME soon after is common).
      constexpr uint32_t UpdateConfirmMs = 2000;
      const uint32_t startedAt = SDL_GetTicks();
      bool confirmed = false;
      // Under the HOME Menu the downloads go on (as an application).
      foil::platform::keepRunningInBackground();
      while (appletMainLoop()) {
        if (!confirmed && SDL_GetTicks() - startedAt >= UpdateConfirmMs) {
          confirmed = true;
          foil::platform::confirmUpdate(self);
        }
        // Not on screen: nothing is drawn or read from the pads; the queue
        // still moves on (the next download starts when one ends).
        if (!foil::platform::onScreen()) {
          ui.poll();
          if (ui.shouldQuit())
            break;
          history = {}; // what was pressed elsewhere is not for this app
          svcSleepThread(50'000'000);
          continue;
        }
        padUpdate(&pad);
        auto keys = padGetButtonsDown(&pad) | newPresses(history);
        if ((keys & HidNpadButton_Plus) && ui.requestQuit())
          break;
        if (keys & HidNpadButton_Minus)
          ui.refreshAll();
        SDL_Event event;
        bool quit = false;
        constexpr float TapSlop = 14;
        while (SDL_PollEvent(&event)) {
          if (event.type == SDL_QUIT)
            quit = true;
          // Touch coordinates are normalized; the UI is laid out in 1280x720.
          const float tx = event.tfinger.x * 1280, ty = event.tfinger.y * 720;
          if (event.type == SDL_FINGERDOWN) {
            touchX = tx;
            touchY = lastY = ty;
            lastX = tx;
            dragging = false;
            ui.fingerDown(tx, ty);
          } else if (event.type == SDL_FINGERMOTION) {
            if (!dragging && (std::abs(ty - touchY) > TapSlop || std::abs(tx - touchX) > TapSlop)) {
              dragging = true;
              ui.dragBegin(touchX, touchY, tx - touchX, ty - touchY);
            }
            if (dragging)
              ui.drag(tx - lastX, ty - lastY);
            lastX = tx;
            lastY = ty;
          } else if (event.type == SDL_FINGERUP) {
            if (dragging)
              ui.dragEnd();
            else
              ui.touch(tx, ty);
          }
        }
        if (quit && ui.requestQuit())
          break;
        if (keys & HidNpadButton_StickR) ui.scrollToTop(); // the right stick pressed in: to the top
        const u64 up = HidNpadButton_Up | HidNpadButton_StickLUp,
                  down = HidNpadButton_Down | HidNpadButton_StickLDown,
                  left = HidNpadButton_Left | HidNpadButton_StickLLeft,
                  right = HidNpadButton_Right | HidNpadButton_StickLRight;
        const auto held = padGetButtons(&pad) & (up | down | left | right);
        constexpr uint32_t RepeatDelayMs = 250, RepeatEveryMs = 70;
        if (keys & (up | down | left | right))
          repeatAt = SDL_GetTicks() + RepeatDelayMs;
        else if (held && SDL_GetTicks() >= repeatAt) {
          keys |= held;
          repeatAt = SDL_GetTicks() + RepeatEveryMs;
        } else if (!held)
          repeatAt = 0;
        using foil::ui::Action;
        if (keys & up)
          ui.action(Action::Up);
        if (keys & down)
          ui.action(Action::Down);
        if (keys & left)
          ui.action(Action::Left);
        if (keys & right)
          ui.action(Action::Right);
        // The right stick moves the screen itself, as a finger does; how far
        // it leans sets the speed.
        {
          const HidAnalogStickState stick = padGetStickPos(&pad, 1);
          constexpr float Dead = 0.15f;
          const float lean = -stick.y / 32767.f;
          const uint32_t now = SDL_GetTicks();
          if (std::abs(lean) > Dead)
            ui.stick((lean - (lean > 0 ? Dead : -Dead)) / (1 - Dead), std::min(0.05f, (now - stickAt) / 1000.f));
          stickAt = now;
        }
        if (keys & HidNpadButton_A)
          ui.action(Action::Accept);
        if (keys & HidNpadButton_B)
          ui.action(Action::Back);
        if (keys & HidNpadButton_L)
          ui.action(Action::Previous);
        if (keys & HidNpadButton_R)
          ui.action(Action::Next);
        if (keys & HidNpadButton_ZR)
          ui.action(Action::Preview);
        if (keys & HidNpadButton_ZL)
          ui.action(Action::Extra);
        if (keys & HidNpadButton_Y)
          ui.action(ui.current() == foil::ui::Screen::Settings ? Action::Reload
                                                               : Action::Queue);
        if (keys & HidNpadButton_X && (ui.current() == foil::ui::Screen::Detail ||
                                         ui.current() == foil::ui::Screen::Installed ||
                                         ui.current() == foil::ui::Screen::Retro ||
                                         ui.current() == foil::ui::Screen::Downloads ||
                                         ui.current() == foil::ui::Screen::Saves ||
                                         ui.current() == foil::ui::Screen::Updates ||
                                         ui.current() == foil::ui::Screen::Patches ||
                                         ui.current() == foil::ui::Screen::Homebrew))
          ui.action(Action::Search);
        if ((keys & HidNpadButton_X &&
             ui.current() == foil::ui::Screen::Library) ||
            ui.takeSearchRequest())
          openSearchKeyboard(ui);
        if (const auto request = ui.takeTextRequest())
          openTextKeyboard(ui, *request);
        ui.render();
        if (ui.shouldQuit())
          break;
      }
      if (!confirmed) foil::platform::confirmUpdate(self); // closed on purpose: it ran
    }
  }
  foil::net::endConnections();
  curl_global_cleanup();
  romfsExit();
  if (csrng)
    csrngExit();
  if (nifm)
    nifmExit();
  socketExit();
  return result;
}
