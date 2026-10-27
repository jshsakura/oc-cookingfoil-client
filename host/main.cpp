#include "core/Retro.hpp"
#include "core/AdminEdit.hpp"
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
void setHostDark(bool dark);
}
int main(int argc, char **argv) {
  std::string shot, fixture, url, configPath, appConfig, statePath = "out/cookingfoil.db", language, self, retroDetail;
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
    else if (arg == "--app-config")
      appConfig = value; // the app reads it and connects to every server itself, as on the console
    else if (arg == "--serve")
      servePort = std::atoi(value.c_str());
    else if (arg == "--state")
      statePath = value;
    else if (arg == "--theme")
      foil::platform::setHostDark(value == "dark"); // the console's theme in System Settings
    else if (arg == "--lang")
      language = value; // ko, en, ja or zh
    else if (arg == "--retro")
      retroDetail = value; // a retro game's page to open, in the server's /api/retro/games/:id shape
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
  foil::labels::Book labelBook;
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
    labelBook = foil::net::parseCatalog(foil::readFile(fixture + "/sections.json")).value.labels;
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
  foil::ui::Application ui(app.renderer(), appConfig, appConfig.empty() ? "" : statePath,
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
  if (!labelBook.empty())
    ui.setLabelBook(std::move(labelBook));
  if (!pending && appConfig.empty()) // with --app-config the app reads its own list
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
  // 관리자 편집 as an admin console sees it.
  if (servePort && !fixture.empty() && std::filesystem::exists(fixture + "/admin.json"))
    ui.previewAdminEdit(foil::admin::parseFields(foil::readFile(fixture + "/admin.json")));
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
  if (!retroDetail.empty())
    ui.previewRetroDetail(foil::retro::parseDetail(foil::readFile(retroDetail), "http://fixture"));
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
      {
        // A server plugin's option, worded and colored by the plugin.
        foil::net::FileOption option;
        option.id = "prepared";
        option.label = "이 본체에 맞춰 설치";
        option.confirmTitle = option.label;
        option.confirmBody = "서버가 이 본체에 맞게 파일을 준비한 뒤 설치합니다.\n"
                             "준비하는 동안 시간이 조금 더 걸릴 수 있습니다.";
        option.buttonColor = foil::net::parseRgb("#FFC23D");
        option.buttonStripe = foil::net::parseRgb("#FFB52A");
        option.cardIcon = "info";
        ui.previewFileOption(option);
        ui.render();
        if (!app.screenshot(shot + "/02-detail-option.png")) return 1;
        ui.previewRequestInstall();
        ui.render();
        if (!app.screenshot(shot + "/02-detail-option-confirm.png")) return 1;
        ui.action(foil::ui::Action::Back);
        ui.previewFileOption({});
        ui.render();
      }
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
      // The question at start about a download left unfinished, with a name
      // as long as the server has them.
      foil::ui::Application asked(app.renderer(), "", "", "host/fixtures/art");
      if (asked.open()) {
        foil::Item item;
        item.name = "FANTASY LIFE i : 빙글빙글 용과 시간을 훔치는 소녀 Nintendo Switch 2 Edition (FANTASY LIFE i: The Girl Who Steals Time)";
        item.url = "https://server.example/fantasy.nsp";
        item.titleId = item.baseId = "0100000000020000";
        asked.previewQueued(item, item.name, "");
        asked.previewOfferResume();
        asked.screen(foil::ui::Screen::Home);
        asked.render();
        if (!app.screenshot(shot + "/04-resume.png")) return 1;
      }
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
      // Dropbox also holds a library game that is not installed (the second;
      // every third is installed) and one the server does not know.
      const auto libraryGames = foil::groupCatalog(foil::net::parseSections(foil::readFile(fixture + "/sections.json")).value);
      foil::net::DropboxRevisions remote = {
          {save.applicationId, {{"20260311_090000", 1773219600, 1234567}, {"20260301_120000", 1772366400, 1200000}}},
          {0x0100000000009000ULL, {{"20260302_080000", 1772438400, 900000}}}};
      if (libraryGames.size() > 1)
        remote[std::stoull(libraryGames[1].id, nullptr, 16)] = {{"20260310_210000", 1773176400, 2400000}};
      ui.previewDropbox(true, std::move(remote));
      ui.render();
      if (!app.screenshot(shot + "/08-dropbox-linked.png")) return 1;
      ui.action(foil::ui::Action::Up); ui.render(); // the account row, A disconnects
      if (!app.screenshot(shot + "/08-dropbox-account.png")) return 1;
      // Newest backup first: the installed game, then the library game not installed.
      ui.action(foil::ui::Action::Down);
      ui.action(foil::ui::Action::Accept); ui.render();
      if (!app.screenshot(shot + "/08-dropbox-game.png")) return 1;
      ui.action(foil::ui::Action::Back);
      ui.action(foil::ui::Action::Down); ui.action(foil::ui::Action::Accept); ui.render();
      if (!app.screenshot(shot + "/08-dropbox-not-installed.png")) return 1;
      ui.action(foil::ui::Action::Back);
      // Y twice: this console's games added, to upload one not on Dropbox yet.
      ui.action(foil::ui::Action::Queue); ui.action(foil::ui::Action::Queue); ui.render();
      if (!app.screenshot(shot + "/08-dropbox-with-console.png")) return 1;
      ui.action(foil::ui::Action::Queue);
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
      ui.previewPairing({"https://server.example.com/admin/pair/" + std::string(32, 'a'),
                         "1234ABCD", 2000000000000LL});
      ui.render();
      if (!app.screenshot(shot + "/00-pairing.png"))
        return 1;
    }
    // 설정 > 언어 with three languages only a server has, and their window.
    ui.screen(foil::ui::Screen::Settings);
    ui.previewServerLanguages({{"ko", "한국어", ""}, {"vi", "Tiếng Việt", "en"}, {"id", "Bahasa Indonesia", ""},
                               {"pt-PT", "Português (Portugal)", "pt"}});
    ui.previewLanguagePopup(13);
    ui.render();
    if (!app.screenshot(shot + "/06-language.png")) return 1;
    ui.previewLanguageGet({"vi", "Tiếng Việt", "en"}, 41 << 10, 92 << 10);
    ui.render();
    if (!app.screenshot(shot + "/06-language-get.png")) return 1;
    ui.previewLanguageGet({"vi", "Tiếng Việt", "en"}, 0, 0, foil::tr("Nothing arrived for 5 seconds. Check the connection and try again."));
    ui.render();
    if (!app.screenshot(shot + "/06-language-failed.png")) return 1;
    ui.previewLanguageDone();
    // A first shortcut offered to start, with a download still waiting.
    ui.screen(foil::ui::Screen::Home);
    ui.previewLaunchOffer("Super Mario World · USA");
    ui.render();
    if (!app.screenshot(shot + "/08-launch-offer.png")) return 1;
    ui.previewLaunchOfferDone();
    // 연결 to PC: waiting, a PC on the cable, a copy, closing, and the
    // applet mode notice; the copy again on every theme.
    {
      using foil::platform::usb::Phase;
      const auto state = [](Phase phase, std::string file = "", int copied = 0) {
        foil::platform::usb::Status status;
        status.phase = phase;
        status.file = std::move(file);
        status.toConsole = true;
        status.moved = int64_t(3) << 30;
        status.copied = copied;
        status.cableSeen = phase != Phase::Waiting;
        return status;
      };
      const auto copying = state(Phase::Open, "Tears of the Kingdom.nsp", 2);
      const std::pair<const char *, foil::platform::usb::Status> states[] = {
          {"09-usb-waiting", state(Phase::Waiting)},
          {"09-usb-plugged", state(Phase::Plugged)},
          {"09-usb-open", state(Phase::Open, "", 5)},
          {"09-usb-copying", copying}};
      for (const auto &[name, status] : states) {
        ui.previewUsbDrive(status);
        ui.render();
        if (!app.screenshot(shot + "/" + name + ".png")) return 1;
      }
      ui.previewUsbStopping();
      ui.render();
      if (!app.screenshot(shot + "/09-usb-stopping.png")) return 1;
      ui.previewUsbStopping(true);
      ui.render();
      if (!app.screenshot(shot + "/09-usb-cleaning.png")) return 1;
      ui.previewUsbDone();
      ui.previewUsbApplet();
      ui.render();
      if (!app.screenshot(shot + "/09-usb-applet.png")) return 1;
      ui.previewUsbDone();
      for (int row = 1; row < 7; ++row) {
        ui.previewThemePopup(row);
        ui.previewThemeClosed();
        ui.previewUsbDrive(copying);
        ui.render();
        if (!app.screenshot(shot + "/09-usb-theme-0" + std::to_string(row) + ".png")) return 1;
        ui.previewUsbDone();
      }
      ui.previewThemePopup(0);
      ui.previewThemeClosed();
    }
    // 내 기기로 보내기: the card at the top right, on every theme.
    {
      const auto art = [](const char *id) { return std::string("https://fixture.invalid/icons/") + id + ".jpg"; };
      const std::vector<std::pair<std::string, std::string>> one{{"Lantern Road", art("01000EA014150000")}};
      const std::vector<std::pair<std::string, std::string>> three{
          {"Lantern Road", art("01000EA014150000")}, {"Pixel Harbor", art("010020B016EF4000")},
          {"Moss Kingdom", art("01002FC00412C000")}};
      ui.screen(foil::ui::Screen::Home);
      const std::pair<const char *, std::vector<std::pair<std::string, std::string>>> cards[] = {
          {"10-arrival-one", one}};
      for (const auto &[name, games] : cards) {
        ui.previewArrivals(games);
        for (int frame = 0; frame < 3; ++frame) ui.render(); // the covers load
        SDL_Delay(300);
        ui.render();
        if (!app.screenshot(shot + "/" + name + ".png")) return 1;
      }
      for (int row = 1; row < 7; ++row) {
        ui.previewThemePopup(row);
        ui.previewThemeClosed();
        ui.previewArrivals(three);
        ui.render();
        SDL_Delay(200);
        ui.render();
        if (!app.screenshot(shot + "/10-arrival-theme-0" + std::to_string(row) + ".png")) return 1;
      }
      ui.previewArrivals({});
      ui.previewThemePopup(0);
      ui.previewThemeClosed();
    }
    // 설정 > 테마 on each palette, and the home screen in it.
    for (int row = 0; row < 7; ++row) {
      const auto name = std::string(row < 10 ? "0" : "") + std::to_string(row);
      ui.screen(foil::ui::Screen::Settings);
      ui.previewThemePopup(row);
      ui.render();
      if (!app.screenshot(shot + "/07-theme-" + name + ".png")) return 1;
      ui.previewThemeClosed();
      ui.screen(foil::ui::Screen::Home);
      ui.render();
      if (!app.screenshot(shot + "/07-home-" + name + ".png")) return 1;
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
  foil::net::endConnections();
  curl_global_cleanup();
}
