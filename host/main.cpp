#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/State.hpp"
#include "platform/Identity.hpp"
#include "net/Api.hpp"
#include "ui/Application.hpp"
#include "ui/Runtime.hpp"
#include "host/WebView.hpp"
#include "core/Log.hpp"
#include <cstdlib>
#include <curl/curl.h>
#include <filesystem>
#include <iostream>
#include <optional>
namespace {
// Installed games and one save for the preview: every third catalog game.
// A game coming down and picks queued behind it, from progress.json: 진행 중
// as the screenshots and the live preview show it.
void previewDownloads(foil::ui::Application &ui, const std::string &fixture) {
  foil::Error error;
  auto json =
      foil::parseJson(foil::readFile(fixture + "/progress.json"), error);
  foil::install::Progress p;
  p.status = foil::install::Status::Running;
  p.name = foil::stringField(json.get(), "name");
  p.icon = foil::stringField(json.get(), "icon");
  p.target = "sd";
  p.kind = "update";
  p.phase = foil::stringField(json.get(), "phase");
  p.index = foil::integerField(json.get(), "index");
  p.count = foil::integerField(json.get(), "count");
  p.received = foil::integerField(json.get(), "received");
  p.total = foil::integerField(json.get(), "total");
  p.speed = json_object_get_double(foil::field(json.get(), "speed"));
  p.percent = p.total ? 100. * p.received / p.total : 0;
  const auto groups = foil::groupCatalog(
      foil::net::parseSections(foil::readFile(fixture + "/sections.json"))
          .value);
  // The game coming down, as a real pick leaves it in 다운로드: the parts
  // before the current one done, the current one running, the rest queued
  // first. Its page then reads 다운로드 중, not 설치하기.
  for (const auto &group : groups) {
    if (group.representative.name != p.name) continue;
    p.parts = foil::Selection(group).files();
    for (size_t i = 0; i < p.parts.size(); ++i) {
      if (i > p.index) {
        ui.previewQueued(p.parts[i], p.name, p.icon);
        continue;
      }
      foil::history::Entry entry;
      entry.id = 900000 + i;
      entry.install = {p.name, p.icon, "sd", {p.parts[i]}};
      entry.outcome = i < p.index ? foil::history::Outcome::Done : foil::history::Outcome::Running;
      entry.at = int64_t(std::time(nullptr));
      ui.previewRecord(std::move(entry));
    }
  }
  for (const auto &group : groups) {
    // A few picks queued behind it, more than fit (the list scrolls).
    if (group.representative.name != p.name && group.representative.name != "Circuit Garden")
      for (const auto &file : foil::Selection(group).files())
        ui.previewQueued(file, group.representative.name, group.representative.icon);
    if (group.representative.name == "Circuit Garden")
      p.completed.push_back(group.representative);
  }
  p.count = p.parts.size();
  ui.previewInstall(p);
}
std::pair<foil::InstalledLibrary, foil::SaveEntry> fixtureInstalled(const std::string &fixture) {
  foil::InstalledLibrary library;
  library.installed.complete = true;
  auto catalog = foil::groupCatalog(foil::net::parseSections(foil::readFile(fixture + "/sections.json")).value);
  for (size_t i = 0; i < catalog.size(); i += 3) {
    const auto &g = catalog[i];
    foil::InstalledGame game;
    game.titleId = g.id;
    game.applicationId = std::stoull(g.id, nullptr, 16);
    game.name = g.representative.name;
    game.storage = {i ? "본체" : "SD 카드"};
    game.bytes = (i * 7919 % 97 + 1) * 160ULL * 1024 * 1024;
    game.lastUpdated = i * 31 % 17;
    library.installed.titles.push_back({g.id, 0});
    library.games.push_back(std::move(game));
  }
  foil::SaveEntry save;
  save.applicationId = library.games.front().applicationId;
  save.saveId = 42; save.user = {1, 2}; save.userName = "사용자 1";
  save.bytes = 16 * 1024 * 1024;
  return {std::move(library), save};
}
} // namespace
namespace foil::platform {
void setHostSpace(uint64_t bytes);
}
int main(int argc, char **argv) {
  std::string shot, fixture, url, configPath, statePath = "out/cookingfoil.db", language, self;
  int servePort = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (i + 1 >= argc) {
      std::cerr << "Missing option value\n";
      return 2;
    }
    const std::string value = argv[++i];
    if (arg == "--shot")
      shot = value;
    else if (arg == "--fixture")
      fixture = value;
    else if (arg == "--url")
      url = value;
    else if (arg == "--config")
      configPath = value;
    else if (arg == "--serve")
      servePort = std::atoi(value.c_str());
    else if (arg == "--state")
      statePath = value;
    else if (arg == "--lang")
      language = value; // ko, en, ja or zh
    else if (arg == "--self")
      self = value; // as argv[0] on the console: sdmc:/switch/cookingfoil/cookingfoil.nro
    else {
      std::cerr << "Unknown option: " << arg << '\n';
      return 2;
    }
  }
  curl_global_init(CURL_GLOBAL_DEFAULT);
  foil::setDeviceIdentity(foil::platform::consoleIdentity());
  foil::openLog("out/host.log");
  std::vector<foil::Item> items;
  foil::net::NameTable genres;
  foil::Server server;
  std::unique_ptr<foil::net::Session> liveSession;
  if (!configPath.empty()) {
    const auto config = foil::loadConfig(configPath);
    if (config.error) {
      std::cerr << config.error.message << " (line " << config.error.line
                << ")\n";
      return 1;
    }
    for (const auto &s : config.value.servers)
      if (s.enabled) {
        server = s;
        break;
      }
  }
  if (!url.empty())
    server.url = foil::normalizeUrl(url);
  if (!server.url.empty()) {
    const auto state = foil::loadState(statePath);
    if (state.error) {
      std::cerr << state.error.message << '\n';
      return 1;
    }
    liveSession =
        std::make_unique<foil::net::Session>(server, state.value, statePath);
    auto &session = *liveSession;
    foil::Error error;
    const bool refreshed = session.refresh(error);
    if ((!refreshed && !session.pairingPending()) ||
        (refreshed && !session.refresh(error))) {
      std::cerr << error.message << '\n';
      // Served, the app opens anyway and shows how it meets the failure.
      if (!servePort)
        return 1;
    }
    items = session.takeItems();
    if (session.pairingPending())
      std::cout << "Device approval pending\n";
    else
      std::cout << "Server items: " << items.size()
                << " (ETag revalidation passed)\n";
  } else if (!fixture.empty()) {
    auto parsed =
        foil::net::parseSections(foil::readFile(fixture + "/sections.json"));
    if (parsed.error) {
      std::cerr << parsed.error.message << '\n';
      return 1;
    }
    items = std::move(parsed.value);
    genres = foil::net::parseNameTable(foil::readFile(fixture + "/sections.json"), "genres");
    std::cout << "Fixture items: " << items.size() << '\n';
  }
  const bool offscreen = !shot.empty() || servePort;
  if (offscreen)
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 0);
  foil::ui::Runtime app;
  if (!app.open(1920, 1080, offscreen)) {
    std::cerr << SDL_GetError() << '\n';
    return 1;
  }
  foil::ui::Application ui(app.renderer(), "", "",
                           fixture.empty() ? "" : "host/fixtures/art");
  if (!ui.open()) {
    std::cerr << "Font initialization failed\n";
    return 1;
  }
  ui.usePatchFolder(std::filesystem::path(statePath).parent_path().string() + "/sd");
  // Pictures kept as on the console, so 설정's image cache shows what it holds.
  if (servePort) ui.useCacheFolder(std::filesystem::path(statePath).parent_path().string() + "/cache");
  // A preview download writes nothing (it measures the speed), so the card
  // it installs to is this computer's free space where its SD folder lives.
  if (servePort) {
    std::error_code spaceError;
    const auto room = std::filesystem::space(std::filesystem::path(statePath).parent_path(), spaceError);
    if (!spaceError)
      foil::platform::setHostSpace(room.available);
  }
  const bool pending = liveSession && liveSession->pairingPending();
  if (liveSession)
    ui.setSession(std::move(liveSession));
  if (!genres.empty())
    ui.setGenreTable(std::move(genres));
  if (!pending)
    ui.setItems(std::move(items));
  if (!language.empty())
    ui.chooseLanguage(foil::languageFromCode(language, foil::Language::Korean));
  if (!fixture.empty())
    ui.previewStorage(212ULL * 1024 * 1024 * 1024, 20ULL * 1024 * 1024 * 1024);
  // Serving fixtures: 설치된 게임 gets the fixture games too.
  if (servePort && !fixture.empty()) {
    auto [library, save] = fixtureInstalled(fixture);
    ui.previewInstalled(std::move(library), {save});
  }
  // 홈브류 from the fixture's list, beside what the fake SD card holds.
  if (servePort && !fixture.empty() && std::filesystem::exists(fixture + "/homebrew.json"))
    ui.previewHomebrew(foil::homebrew::parse(foil::readFile(fixture + "/homebrew.json"), "http://fixture"));
  // 진행 중 with a game coming down, as in the screenshots.
  if (servePort && !fixture.empty() && std::filesystem::exists(fixture + "/progress.json"))
    previewDownloads(ui, fixture);
  // 패치's list from the fixture, so the screen shows patches, not a list
  // that never comes.
  if (servePort && !fixture.empty() && std::filesystem::exists(fixture + "/patches.json"))
    ui.previewPatches(foil::net::parsePatches(foil::readFile(fixture + "/patches.json")));
  // The app's own path as the console passes it: from the album with no
  // shortcut, the offer to make one (the fake SD card needs the .nro).
  if (!self.empty())
    ui.setSelfPath(self);
  ui.render();
  if (servePort)
    return foil::host::serveWeb(ui, app, servePort);
  if (offscreen) {
    std::filesystem::create_directories(shot);
    if (pending)
      return app.screenshot(shot + "/00-pairing.png") ? 0 : 1;
    for (const auto screen :
         {foil::ui::Screen::Home, foil::ui::Screen::Library}) {
      ui.screen(screen);
      ui.render();
      for (int frame = 0; frame < 200 && !ui.imagesReady(); ++frame) {
        SDL_Delay(5);
        ui.decode();
      }
      ui.render();
      if (!app.screenshot(shot + (screen == foil::ui::Screen::Home
                                      ? "/01-home.png"
                                      : "/03-library.png")))
        return 1;
    }
    if (ui.groupCount()) {
      ui.screen(foil::ui::Screen::Library);
      ui.action(foil::ui::Action::Accept);
      if (!fixture.empty())
        ui.setDetail(
            // A language may have its own fixture (detail.en.json).
            foil::net::parseDetail(foil::readFile(std::filesystem::exists(fixture + "/detail." + std::string(foil::languageCode()) + ".json")
                                                      ? fixture + "/detail." + std::string(foil::languageCode()) + ".json"
                                                      : fixture + "/detail.json"))
                .value);
      ui.action(foil::ui::Action::Down);
      ui.action(foil::ui::Action::Down);
      ui.action(foil::ui::Action::Search);
      ui.render();
      const auto deadline = SDL_GetTicks() + 65000;
      while ((ui.loading() || !ui.imagesReady()) && SDL_GetTicks() < deadline) {
        SDL_Delay(5);
        ui.poll();
        ui.decode();
      }
      ui.render();
      if (!app.screenshot(shot + "/02-detail.png"))
        return 1;
      if (!fixture.empty()) {
        ui.action(foil::ui::Action::Preview); ui.render();
        if (!app.screenshot(shot + "/02-gallery-unavailable.png")) return 1;
        ui.action(foil::ui::Action::Back);
      }
    }
    if (!fixture.empty()) {
      previewDownloads(ui, fixture);
      ui.screen(foil::ui::Screen::Downloads);
      ui.render();
      if (!app.screenshot(shot + "/04-downloads.png"))
        return 1;
    }
    if (!fixture.empty()) {
      auto [library, save] = fixtureInstalled(fixture);
      ui.previewInstall({});
      ui.previewDeviceBusy(true);
      ui.screen(foil::ui::Screen::Installed); ui.render();
      if (!app.screenshot(shot + "/07-installed-loading.png")) return 1;
      ui.previewDeviceBusy(false);
      ui.previewInstalled(std::move(library), {save});
      ui.screen(foil::ui::Screen::Installed); ui.render();
      if (!app.screenshot(shot + "/07-installed.png")) return 1;
      ui.action(foil::ui::Action::Next); ui.render();
      if (!app.screenshot(shot + "/07-installed-size.png")) return 1;
      ui.action(foil::ui::Action::Previous); ui.render();
      ui.screen(foil::ui::Screen::Updates); ui.render();
      if (!app.screenshot(shot + "/05-updates.png")) return 1;
      ui.screen(foil::ui::Screen::Installed);
      ui.action(foil::ui::Action::Accept); ui.render();
      if (!app.screenshot(shot + "/07-manage.png")) return 1;
      ui.action(foil::ui::Action::Down); ui.action(foil::ui::Action::Accept); ui.render();
      if (!app.screenshot(shot + "/07-save-confirm.png")) return 1;
      ui.action(foil::ui::Action::Back); ui.action(foil::ui::Action::Up);
      ui.action(foil::ui::Action::Accept); ui.render();
      if (!app.screenshot(shot + "/07-game-confirm.png")) return 1;
      ui.action(foil::ui::Action::Back); ui.action(foil::ui::Action::Back);
      // 세이브 > 드롭박스: not linked, the QR sign-in, linked, a game's popup.
      ui.screen(foil::ui::Screen::Saves);
      ui.action(foil::ui::Action::Next); ui.action(foil::ui::Action::Next);
      ui.previewDropbox(false); ui.render();
      if (!app.screenshot(shot + "/08-dropbox.png")) return 1;
      ui.previewDropbox(false, {}, "https://www.dropbox.com/oauth2/authorize?client_id=preview&state=preview");
      ui.render();
      if (!app.screenshot(shot + "/08-dropbox-sign-in.png")) return 1;
      // Dropbox also holds a shop game that is not installed (the second;
      // every third is installed) and one the shop does not know.
      const auto shopGames = foil::groupCatalog(foil::net::parseSections(foil::readFile(fixture + "/sections.json")).value);
      foil::net::DropboxRevisions remote = {
          {save.applicationId, {{"20260311_090000", 1773219600, 1234567}, {"20260301_120000", 1772366400, 1200000}}},
          {0x0100000000009000ULL, {{"20260302_080000", 1772438400, 900000}}}};
      if (shopGames.size() > 1)
        remote[std::stoull(shopGames[1].id, nullptr, 16)] = {{"20260310_210000", 1773176400, 2400000}};
      ui.previewDropbox(true, std::move(remote));
      ui.render();
      if (!app.screenshot(shot + "/08-dropbox-linked.png")) return 1;
      ui.action(foil::ui::Action::Accept); ui.render();
      if (!app.screenshot(shot + "/08-dropbox-not-installed.png")) return 1;
      ui.action(foil::ui::Action::Back);
      ui.action(foil::ui::Action::Down); ui.action(foil::ui::Action::Accept); ui.render();
      if (!app.screenshot(shot + "/08-dropbox-game.png")) return 1;
      ui.action(foil::ui::Action::Back);
    }
    ui.screen(foil::ui::Screen::Settings);
    ui.render();
    if (!app.screenshot(shot + "/06-settings.png"))
      return 1;
    ui.action(foil::ui::Action::Accept);
    ui.render();
    if (!app.screenshot(shot + "/06-server-add.png"))
      return 1;
    if (!fixture.empty()) {
      ui.action(foil::ui::Action::Back);
      ui.screen(foil::ui::Screen::Home);
      ui.previewPairing({"https://shop.example.com/admin/pair/" + std::string(32, 'a'),
                         "1234ABCD", 2000000000000LL});
      ui.render();
      if (!app.screenshot(shot + "/00-pairing.png"))
        return 1;
    }
    return 0;
  }
  bool running = true, typing = false;
  // A field the app asked for (the server form): typed straight into it,
  // Enter keeps it, Esc puts back what was there.
  std::optional<foil::ui::Application::TextRequest> field;
  std::string fieldText;
  while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      using foil::ui::Action;
      if (event.type == SDL_QUIT && ui.requestQuit())
        running = false;
      if (field) {
        if (event.type == SDL_TEXTINPUT &&
            int(foil::ui::utf8Characters(fieldText + event.text.text).size()) <= field->maxLength) {
          fieldText += event.text.text;
          ui.enterText(fieldText);
        } else if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_BACKSPACE) {
          auto chars = foil::ui::utf8Characters(fieldText);
          if (!chars.empty())
            chars.pop_back();
          fieldText.clear();
          for (const auto &c : chars)
            fieldText += c;
          ui.enterText(fieldText);
        } else if (event.type == SDL_KEYDOWN &&
                   (event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_ESCAPE)) {
          if (event.key.keysym.sym == SDLK_ESCAPE)
            ui.enterText(field->initial);
          field.reset();
          SDL_StopTextInput();
        }
        continue;
      }
      if (event.type == SDL_TEXTINPUT && typing)
        ui.search(ui.query() + event.text.text);
      if (event.type == SDL_MOUSEBUTTONUP && !typing)
        ui.touch(event.button.x, event.button.y);
      if (event.type == SDL_MOUSEWHEEL) {
        // The wheel is a short finger drag where the pointer is.
        int mx = 0, my = 0;
        SDL_GetMouseState(&mx, &my);
        ui.dragBegin(float(mx), float(my), 0, 1);
        ui.drag(0, event.wheel.y * 40.f);
        ui.dragEnd(0, 0);
      }
      if (event.type == SDL_KEYDOWN) {
        const auto key = event.key.keysym.sym;
        if (key == SDLK_PLUS || key == SDLK_F10) {
          if (ui.requestQuit())
            running = false;
        } else if (!typing && key == SDLK_x &&
                   ui.current() == foil::ui::Screen::Detail)
          ui.action(Action::Search);
        else if (!typing && (key == SDLK_x || key == SDLK_SLASH)) {
          typing = true;
          SDL_StartTextInput();
          ui.search("");
        } else if (key == SDLK_ESCAPE) {
          typing = false;
          SDL_StopTextInput();
          ui.action(Action::Back);
        } else if (key == SDLK_BACKSPACE && typing) {
          auto chars = foil::ui::utf8Characters(ui.query());
          if (!chars.empty())
            chars.pop_back();
          std::string q;
          for (const auto &c : chars)
            q += c;
          ui.search(q);
        } else if (key == SDLK_RETURN) {
          typing = false;
          SDL_StopTextInput();
          ui.action(Action::Accept);
        } else if (key == SDLK_UP)
          ui.action(Action::Up);
        else if (key == SDLK_DOWN)
          ui.action(Action::Down);
        else if (key == SDLK_LEFT)
          ui.action(Action::Left);
        else if (key == SDLK_RIGHT)
          ui.action(Action::Right);
        else if (key == SDLK_l)
          ui.action(Action::Previous);
        else if (key == SDLK_PAGEUP)
          ui.action(Action::ScrollUp);
        else if (key == SDLK_PAGEDOWN)
          ui.action(Action::ScrollDown);
        else if (key == SDLK_r)
          ui.action(Action::Next);
        else if (key == SDLK_p)
          ui.action(Action::Preview);
        else if (key == SDLK_y)
          ui.action(ui.current() == foil::ui::Screen::Settings ||
                            ui.current() == foil::ui::Screen::Installed
                        ? Action::Reload
                        : Action::Queue);
      }
    }
    if (ui.takeSearchRequest() && !typing) {
      typing = true;
      SDL_StartTextInput();
      ui.search(ui.query());
    }
    if (!typing && !field)
      if (auto request = ui.takeTextRequest()) {
        field = std::move(request);
        fieldText = field->initial;
        SDL_StartTextInput();
      }
    ui.render();
    if (ui.shouldQuit())
      running = false;
    SDL_Delay(16);
  }
  curl_global_cleanup();
}
