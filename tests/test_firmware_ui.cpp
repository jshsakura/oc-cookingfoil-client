// 패치's 펌웨어 tab: shown only when a server offers firmware, A asks first
// (warning when the version is older than the console's), and the download
// waits its turn in 다운로드's queue.
#include "Test.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "ui/Application.hpp"
#include <chrono>
#include <cstdlib>
#include <thread>
using foil::ui::Action;
using foil::ui::Screen;
int runFirmwareUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  foil::firmware::List list;
  CHECK(foil::firmware::parse(foil::readFile("host/fixtures/firmware.json"), "http://fixture", list));
  const auto settle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };
  {
    // No server offers firmware: L R go round the three patch tabs alone.
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.screen(Screen::Patches);
    for (int i = 0; i < 3; ++i) app.action(Action::Next);
    CHECK(app.patchTab() == 0);
    app.action(Action::Previous);
    CHECK(app.patchTab() == 2);
  }
  {
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.previewQueueOpen();
    // Something installing, so the firmware waits in the queue.
    foil::Item game;
    game.name = "A";
    game.url = "https://server.example/A.nsp";
    game.titleId = game.baseId = "0100000000010000";
    game.size = 1000;
    CHECK(!app.previewPick({game}, "A"));
    app.previewFirmware(list);
    app.screen(Screen::Patches);
    app.action(Action::Previous); // 설치됨 back to the last tab: 펌웨어
    CHECK(app.patchTab() == 3);
    app.render();
    app.action(Action::Right);
    // The unavailable version says so and opens nothing.
    for (int i = 0; i < 3; ++i) app.action(Action::Down);
    app.action(Action::Accept);
    CHECK(!app.firmwareAsking());
    // B closes the popup without queueing.
    app.action(Action::Down);
    app.action(Action::Accept);
    CHECK(app.firmwareAsking());
    app.render();
    app.action(Action::Back);
    CHECK(!app.firmwareAsking());
    // L R under the popup do not change the tab.
    app.action(Action::Accept);
    app.action(Action::Next);
    CHECK(app.patchTab() == 3 && app.firmwareAsking());
    settle();
    app.action(Action::Accept);
    CHECK(!app.firmwareAsking());
    const auto names = app.queuedNames();
    CHECK(std::count(names.begin(), names.end(), foil::trf("Firmware {0}", {"2.0.0"})) == 1);
    // Asked again: once in the queue.
    app.action(Action::Accept);
    settle();
    app.action(Action::Accept);
    const auto again = app.queuedNames();
    CHECK(std::count(again.begin(), again.end(), foil::trf("Firmware {0}", {"2.0.0"})) == 1);
    app.render();
  }
  return failures;
}
