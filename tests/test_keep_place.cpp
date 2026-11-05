// A new list, a refresh and a look at another screen leave the player where
// they were: the same game, filters and shelf.
#include "Test.hpp"
#include "core/Json.hpp"
#include "core/State.hpp"
#include "net/Api.hpp"
#include "ui/Application.hpp"
#include "ui/Images.hpp"
#include "MockServer.hpp"
#include <algorithm>
#include <string>
using foil::ui::Action;
using foil::ui::Screen;
namespace {
std::vector<foil::Item> withFirstFresh(std::vector<foil::Item> items) {
  auto fresh = items.front();
  fresh.titleId = fresh.baseId = "0100FFFFFFFF0000";
  fresh.url = "https://example.com/fresh.nsp";
  fresh.name = "Fresh";
  items.insert(items.begin(), fresh);
  return items;
}
} // namespace
int runKeepPlaceTests(SDL_Renderer *renderer) {
  int failures = 0;
  const auto parsed = foil::net::parseSections(foil::readFile("host/fixtures/sections.json"));
  CHECK(!parsed.error);
  const auto move = [](foil::ui::Application &app, Action a, int times) {
    for (int i = 0; i < times; ++i) app.action(a);
  };
  {
    // A new catalog under the list: the same game stays under the cursor.
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setItems(parsed.value);
    app.screen(Screen::Library);
    move(app, Action::Right, 3);
    const auto name = app.selectedName();
    CHECK(app.cursor() == 3);
    app.setItems(withFirstFresh(parsed.value));
    CHECK(app.current() == Screen::Library);
    CHECK(app.selectedName() == name);
    // The game gone: the cursor stays near, inside the list.
    auto shorter = parsed.value;
    shorter.resize(2);
    app.setItems(shorter);
    CHECK(app.cursor() < int(app.libraryCount()));
  }
  {
    // The filter chosen in the popup outlives a new catalog.
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setItems(parsed.value);
    app.screen(Screen::Library);
    const auto all = app.libraryCount();
    // The first chip that narrows this list: the popup's grid has two columns.
    const auto pick = [&](int chip) {
      app.action(Action::Queue);
      move(app, Action::Down, chip / 2);
      move(app, Action::Right, chip % 2);
      app.action(Action::Accept);
      app.action(Action::Queue);
    };
    for (int chip = 1; chip < 12 && app.libraryCount() == all; ++chip) {
      pick(chip);
      if (app.libraryCount() == all) pick(chip);
    }
    const auto filtered = app.libraryCount();
    CHECK(filtered < all);
    app.setItems(parsed.value);
    CHECK(app.libraryCount() == filtered);
  }
  {
    // The menu to another screen and back: the same game on the plain list,
    // the same shelf on 홈.
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setItems(parsed.value);
    app.screen(Screen::Library);
    move(app, Action::Right, 4);
    app.action(Action::Back);
    CHECK(app.onMenu());
    app.action(Action::Up);
    app.action(Action::Accept); // 홈
    CHECK(app.current() == Screen::Home);
    app.action(Action::Back);
    app.action(Action::Down);
    app.action(Action::Accept); // 모든 게임
    CHECK(app.current() == Screen::Library);
    CHECK(app.cursor() == 4);
    app.action(Action::Back);
    app.action(Action::Up);
    app.action(Action::Accept);
    CHECK(app.current() == Screen::Home);
    app.action(Action::Down);
    app.action(Action::Right);
    const int shelf = app.shelf(), column = app.cursor();
    CHECK(shelf == 2 && column == 1);
    app.action(Action::Back);
    app.action(Action::Down);
    app.action(Action::Accept);
    app.action(Action::Back);
    app.action(Action::Up);
    app.action(Action::Accept);
    CHECK(app.current() == Screen::Home);
    CHECK(app.shelf() == shelf && app.cursor() == column);
  }
  {
    // A cursor past a list that shrank opens the last game, not what lies
    // beyond the list.
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setItems(parsed.value);
    app.screen(Screen::Library);
    app.previewCursor(9999);
    app.action(Action::Accept);
    CHECK(app.current() == Screen::Detail);
    CHECK(!app.detailGame().empty());
  }
  {
    // The page of a game through a new catalog: on its game, its index moved;
    // gone from the list, back to where it came from.
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setItems(parsed.value);
    app.screen(Screen::Library);
    move(app, Action::Right, 2);
    app.action(Action::Accept);
    const auto game = app.detailGame();
    CHECK(app.current() == Screen::Detail && !game.empty());
    const auto rows = app.detailRows();
    app.setItems(withFirstFresh(parsed.value));
    CHECK(app.current() == Screen::Detail);
    CHECK(app.detailGame() == game);
    CHECK(app.detailRows() == rows);
    std::vector<foil::Item> rest;
    for (const auto &item : parsed.value)
      if (item.titleId != game && item.baseId != game) rest.push_back(item);
    app.setItems(rest);
    CHECK(app.current() == Screen::Library);
  }
  {
    // - on a game's page with an unchanged list (a 304), and with a new one:
    // the page stays.
    Mock mock;
    const auto body = foil::readFile("host/fixtures/sections.json");
    mock.answer = [&](const std::string &request) -> Reply {
      if (request.rfind("GET /api/library/sections", 0) == 0) {
        if (request.find("If-None-Match") != std::string::npos) return {304, "ETag: \"v1\"\r\n", ""};
        return {200, "ETag: \"v1\"\r\n", body};
      }
      if (request.rfind("GET /api/pair/status", 0) == 0) return {404, "", "{}"};
      return {404, "", "{}"};
    };
    foil::Server server;
    server.url = "http://127.0.0.1:" + std::to_string(mock.port);
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setSession(std::make_unique<foil::net::Session>(server, foil::State{}, ""));
    const auto settle = [&] {
      for (int frame = 0; frame < 600 && (frame < 2 || app.asking()); ++frame) {
        SDL_Delay(5);
        app.render();
      }
    };
    app.refresh();
    settle();
    CHECK(app.groupCount() == 13);
    app.screen(Screen::Library);
    app.action(Action::Accept);
    const auto game = app.detailGame();
    CHECK(app.current() == Screen::Detail && !game.empty());
    settle();
    app.refresh();
    settle();
    CHECK(app.current() == Screen::Detail);
    CHECK(app.detailGame() == game);
  }
  {
    // Patches through their frame's list: the screen, its filter and search
    // show what an action reads anew.
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setItems(parsed.value);
    app.screen(Screen::Patches);
    app.render();
    CHECK(app.current() == Screen::Patches);
  }
  {
    // Many new covers in one frame: the first is made, the others follow in
    // the next frames, and all of them arrive.
    foil::ui::Images images(renderer);
    const auto bytes = foil::readFile("host/fixtures/art/0100001019F6E000.jpg");
    for (int i = 0; i < 4; ++i) images.provide("cover" + std::to_string(i), bytes);
    images.decode(100);
    images.textureBudget(0);
    const auto frame = [&] {
      for (int i = 0; i < 4; ++i) images.draw("cover" + std::to_string(i), 0, 0, 40, 40);
      images.decode(0);
    };
    frame();
    CHECK(images.textureCount() == 1);
    frame();
    CHECK(images.textureCount() == 2);
    frame();
    frame();
    CHECK(images.textureCount() == 4);
    frame(); // none new: nothing to spread
    CHECK(images.textureCount() == 4);
  }
  {
    // The waiting updates follow the installed list and the queue at once,
    // though the list is kept between frames.
    auto items = withFirstFresh(parsed.value);
    const std::string base = "0100FFFFFFFF0000";
    foil::Item update = items.front();
    update.type = foil::Word("update");
    update.version = 65536;
    update.url = "https://example.com/update.nsp";
    update.titleId = "0100FFFFFFFF0800";
    update.baseId = base;
    items.push_back(update);
    foil::ui::Application app(renderer, "", "", "host/fixtures/art");
    CHECK(app.open());
    app.setItems(items);
    foil::InstalledGame game;
    game.titleId = base;
    game.name = "Game";
    app.previewInstalled({{true, {{base, 0}}}, {game}});
    const auto before = app.pendingCount();
    CHECK(before == 1);
    CHECK(app.pendingCount() == before);
    app.setItems(withFirstFresh(parsed.value)); // the update gone from the server
    CHECK(app.pendingCount() == 0);
    app.setItems(items);
    CHECK(app.pendingCount() == before);
    app.previewQueued(update.url); // in the queue: not asked for again
    CHECK(app.pendingCount() == 0);
  }
  return failures;
}
