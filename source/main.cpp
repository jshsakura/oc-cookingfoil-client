#include "ui/Application.hpp"
#include "ui/Runtime.hpp"
#include "core/Log.hpp"
#include "core/I18n.hpp"
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
  swkbdConfigSetGuideText(&keyboard, foil::tr("게임 이름을 입력합니다"));
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
int main(int argc, char **argv) {
  if (R_FAILED(socketInitializeDefault()))
    return 1;
  const bool nifm = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
  if (R_FAILED(romfsInit())) {
    socketExit();
    return 1;
  }
  curl_global_init(CURL_GLOBAL_DEFAULT);
  foil::openLog("sdmc:/switch/cookingfoil/log.txt");
  foil::logLine(std::string("start " COOKINGFOIL_VERSION " ") + (appletGetAppletType() == AppletType_Application ? "application" : "applet") + " mode");
  int result = 0;
  {
    foil::ui::Runtime app;
    if (!app.open(1280, 720, false))
      result = 1;
    else {
      foil::ui::Application ui(app.renderer(),
                               "sdmc:/switch/cookingfoil/config.json",
                               "sdmc:/switch/cookingfoil/state.json");
      if (!ui.open())
        return 1;
      // hbmenu and forwarders pass the .nro's path; an update replaces it.
      if (argc > 0 && argv[0])
        ui.setSelfPath(argv[0]);
      PadState pad;
      padConfigureInput(1, HidNpadStyleSet_NpadStandard);
      padInitializeDefault(&pad);
      uint32_t repeatAt = 0, scrollAt = 0, heartbeatAt = 0;
      auto shownScreen = ui.current();
      float touchX = 0, touchY = 0, lastY = 0;
      bool dragging = false;
      while (appletMainLoop()) {
        padUpdate(&pad);
        if ((padGetButtonsDown(&pad) & HidNpadButton_Plus) && ui.requestQuit())
          break;
        if (padGetButtonsDown(&pad) & HidNpadButton_Minus)
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
            dragging = false;
            ui.fingerDown();
          } else if (event.type == SDL_FINGERMOTION) {
            if (!dragging && (std::abs(ty - touchY) > TapSlop || std::abs(tx - touchX) > TapSlop)) {
              dragging = true;
              ui.dragBegin(touchX, touchY);
            }
            if (dragging)
              ui.drag(ty - lastY);
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
        auto keys = padGetButtonsDown(&pad);
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
        // Right stick scrolls continuously while held.
        constexpr uint32_t ScrollEveryMs = 50;
        const auto scroll = padGetButtons(&pad) &
                            (HidNpadButton_StickRUp | HidNpadButton_StickRDown);
        if (scroll && SDL_GetTicks() >= scrollAt) {
          ui.action(scroll & HidNpadButton_StickRUp ? Action::ScrollUp
                                                    : Action::ScrollDown);
          scrollAt = SDL_GetTicks() + ScrollEveryMs;
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
        // One line a minute; logLine appends used/total memory, so a slow leak
        // shows up in log.txt before it becomes a crash.
        constexpr uint32_t HeartbeatMs = 60000;
        if (SDL_GetTicks() - heartbeatAt >= HeartbeatMs) {
          heartbeatAt = SDL_GetTicks();
          foil::logLine(std::string("heartbeat ") + foil::ui::screenLabel(ui.current()));
        }
        // Each screen's memory on arrival, so the log shows which one grows
        // the heap (an album launch has little of it to spare).
        if (ui.current() != shownScreen) {
          shownScreen = ui.current();
          foil::logLine(std::string("screen ") + foil::ui::screenLabel(shownScreen));
        }
        if (ui.shouldQuit())
          break;
      }
    }
  }
  curl_global_cleanup();
  romfsExit();
  if (nifm)
    nifmExit();
  socketExit();
  return result;
}
