#include "Test.hpp"
#include "core/Json.hpp"
#include "ui/Application.hpp"
#include <filesystem>
#include <unistd.h>
int runUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  foil::ui::Fonts fonts(renderer);
  CHECK(fonts.open());
  CHECK(fonts.supports(0xD55C));
  CHECK(fonts.supports(0x65E5));
  CHECK(fonts.supports(0x4E2D));
  CHECK(fonts.measure("한글 日本 中文", 18) > 80);
  auto parsed =
      foil::net::parseSections(foil::readFile("host/fixtures/sections.json"));
  foil::ui::Application app(renderer, "", "", "host/fixtures/art");
  CHECK(app.open());
  app.setItems(parsed.value);
  CHECK(app.groupCount() == 13);
  {
    // Moving in the menu shows the screen under the cursor once it rests;
    // the cursor stays on the menu until A or Right.
    app.screen(foil::ui::Screen::Home);
    app.action(foil::ui::Action::Back);
    app.action(foil::ui::Action::Down);
    CHECK(app.current() == foil::ui::Screen::Home);
    SDL_Delay(300);
    app.render();
    CHECK(app.current() == foil::ui::Screen::Library);
    CHECK(app.onMenu());
    app.action(foil::ui::Action::Right);
    CHECK(!app.onMenu());
    CHECK(app.current() == foil::ui::Screen::Library);
  }
  {
    // Taps hit what the last frame drew: sidebar "설정", then a library card.
    app.screen(foil::ui::Screen::Home);
    app.render();
    app.touch(120, app.menuRowTop(foil::ui::MenuItems - 1) + foil::ui::MenuRowHeight / 2);
    CHECK(app.current() == foil::ui::Screen::Settings);
    app.screen(foil::ui::Screen::Library);
    app.render();
    app.touch(330, 250);
    CHECK(app.current() == foil::ui::Screen::Detail);
    app.render();
    app.touch(60, 30);
    CHECK(app.current() == foil::ui::Screen::Library);
  }
  app.screen(foil::ui::Screen::Library);
  CHECK(app.selectedName() == "Lantern Road");
  app.action(foil::ui::Action::Right);
  CHECK(app.selectedName() == "Paper Moon");
  // Up from the first row lands on the header's search box; A asks for a
  // keyboard once. Left from there is the filter button.
  CHECK(!app.takeSearchRequest());
  app.render();
  app.action(foil::ui::Action::Up);
  CHECK(app.headerFocus() == "search");
  app.action(foil::ui::Action::Left);
  CHECK(app.headerFocus() == "filter");
  app.action(foil::ui::Action::Right);
  app.action(foil::ui::Action::Accept);
  CHECK(app.headerFocus().empty());
  CHECK(app.current() == foil::ui::Screen::Library);
  CHECK(app.takeSearchRequest());
  CHECK(!app.takeSearchRequest());
  app.action(foil::ui::Action::Down);
  app.search("버터");
  CHECK(app.selectedName() == "버터 블록 대작전");
  app.action(foil::ui::Action::Accept);
  CHECK(app.current() == foil::ui::Screen::Detail);
  const auto bytes = app.selectedBytes();
  // L and R no longer change the storage target.
  app.action(foil::ui::Action::Next);
  CHECK(app.selectedTarget() == "sd");
  // The cursor starts on the install button; above it the storage cards,
  // where right and left pick one, then the DLC.
  // Right only moves the cursor; A picks the card under it.
  app.action(foil::ui::Action::Up);
  app.action(foil::ui::Action::Right);
  CHECK(app.selectedTarget() == "sd");
  app.action(foil::ui::Action::Accept);
  CHECK(app.selectedTarget() == "nand");
  app.action(foil::ui::Action::Left);
  app.action(foil::ui::Action::Accept);
  CHECK(app.selectedTarget() == "sd");
  app.action(foil::ui::Action::Up);
  app.action(foil::ui::Action::Accept);
  CHECK(app.selectedBytes() < bytes);
  CHECK(app.current() == foil::ui::Screen::Detail);
  foil::net::Detail detail;
  detail.screenshots = {"https://fixture.invalid/1.jpg", "https://fixture.invalid/2.jpg"};
  app.setDetail(detail);
  app.action(foil::ui::Action::Preview);
  CHECK(app.viewingScreenshots());
  const auto beforeGallery = app.selectedBytes();
  app.action(foil::ui::Action::Next);
  app.action(foil::ui::Action::Accept);
  CHECK(app.viewingScreenshots());
  CHECK(app.selectedBytes() == beforeGallery);
  app.action(foil::ui::Action::Back);
  CHECK(!app.viewingScreenshots());
  CHECK(app.current() == foil::ui::Screen::Detail);
  app.action(foil::ui::Action::Back);
  CHECK(app.current() == foil::ui::Screen::Library);
  CHECK(app.query() == "버터");
  app.search("");
  app.render();
  Uint32 pixel = 0;
  SDL_Rect at{252 * 3 / 2, 220 * 3 / 2, 1, 1};
  CHECK(SDL_RenderReadPixels(renderer, &at, SDL_PIXELFORMAT_RGBA32, &pixel,
                             sizeof(pixel)) == 0);
  auto *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
  Uint8 r, g, b;
  SDL_GetRGB(pixel, format, &r, &g, &b);
  SDL_FreeFormat(format);
  CHECK(r == foil::ui::Focus.r && g == foil::ui::Focus.g &&
        b == foil::ui::Focus.b);
  app.search("unknown item");
  CHECK(app.selectedName().empty());
  app.action(foil::ui::Action::Accept);
  CHECK(app.current() == foil::ui::Screen::Library);
  // A new remote session cannot expose the previous server's selection.
  foil::Server other;
  other.url = "https://other.example.com";
  app.search("");
  app.action(foil::ui::Action::Accept);
  CHECK(app.selectedBytes() > 0);
  app.setSession(
      std::make_unique<foil::net::Session>(other, foil::State{}, ""));
  CHECK(app.groupCount() == 0);
  CHECK(app.selectedBytes() == 0);
  CHECK(app.current() == foil::ui::Screen::Library);
  app.action(foil::ui::Action::Accept);
  CHECK(app.current() == foil::ui::Screen::Library);
  // Installed games are local and include titles outside the remote catalog.
  foil::InstalledGame installed;
  installed.applicationId = 0x0100E5E01C098000ULL;
  installed.titleId = "0100E5E01C098000";
  installed.name = "본체 게임";
  foil::SaveEntry save;
  save.applicationId = installed.applicationId;
  save.saveId = 42; save.user = {1, 2}; save.userName = "사용자 1";
  app.previewInstalled({{true, {{installed.titleId, 0}}}, {installed}}, {save});
  app.screen(foil::ui::Screen::Installed);
  CHECK(app.installedCount() == 1);
  app.action(foil::ui::Action::Accept);
  CHECK(app.managingGame());
  // A save opens its panel: back up now, its backups (none here), then delete.
  app.action(foil::ui::Action::Down);
  app.action(foil::ui::Action::Accept);
  CHECK(!app.confirmingRemoval());
  app.action(foil::ui::Action::Down);
  app.action(foil::ui::Action::Accept);
  CHECK(app.confirmingRemoval());
  // An A right after it opens (a double press) never deletes; B backs out.
  app.action(foil::ui::Action::Accept);
  CHECK(app.confirmingRemoval());
  app.action(foil::ui::Action::Back);
  CHECK(!app.confirmingRemoval());
  CHECK(app.managingGame());
  CHECK(app.installedCount() == 1);
  app.action(foil::ui::Action::Back); // the save panel
  CHECK(app.managingGame());
  app.action(foil::ui::Action::Back);
  CHECK(!app.managingGame());
  CHECK(app.current() == foil::ui::Screen::Installed);
  {
    // An account without a save yet: listed, but nothing to back up or
    // delete there (only a backup to restore, which creates it).
    foil::SaveEntry none;
    none.applicationId = installed.applicationId;
    none.user = {3, 4};
    none.userName = "사용자 2";
    none.missing = true;
    app.previewInstalled({{true, {{installed.titleId, 0}}}, {installed}}, {save, none});
    app.screen(foil::ui::Screen::Installed);
    app.action(foil::ui::Action::Accept);
    CHECK(app.managingGame());
    app.render();
    app.action(foil::ui::Action::Down);
    app.action(foil::ui::Action::Down);
    app.action(foil::ui::Action::Accept); // the save panel for 사용자 2
    app.render();
    app.action(foil::ui::Action::Accept); // "back up now" on a missing save
    CHECK(!app.confirmingRemoval());
    app.render();
    app.action(foil::ui::Action::Back);
    app.action(foil::ui::Action::Back);
    CHECK(!app.managingGame());
  }
  {
    // 내 게임 is one menu row for the console's games, their updates and DLC;
    // L R moves along 설치됨 · 업데이트 · DLC and wraps around.
    CHECK(foil::ui::MenuItems == 9);
    // 레트로 게임 is left out of the menu, and its room, when the server has none.
    app.previewFeatures({});
    const int withoutRetro = app.menuRowTop(foil::ui::menuIndex(foil::ui::Screen::Installed));
    app.previewFeatures({"retro"});
    CHECK(app.menuRowTop(foil::ui::menuIndex(foil::ui::Screen::Installed)) == withoutRetro + foil::ui::MenuRowPitch);
    CHECK(foil::ui::menuIndex(foil::ui::Screen::Updates) == foil::ui::menuIndex(foil::ui::Screen::Installed));
    CHECK(std::string(foil::ui::screenLabel(foil::ui::Screen::Installed)) == foil::tr("내 게임"));
    app.screen(foil::ui::Screen::Installed);
    app.render();
    CHECK(app.myGamesTab() == 0);
    app.action(foil::ui::Action::Next);
    CHECK(app.current() == foil::ui::Screen::Updates);
    CHECK(app.myGamesTab() == 1);
    app.render();
    app.action(foil::ui::Action::Next);
    CHECK(app.myGamesTab() == 2);
    app.render();
    app.action(foil::ui::Action::Next);
    CHECK(app.current() == foil::ui::Screen::Installed);
    CHECK(app.installedCount() == 1);
    app.action(foil::ui::Action::Previous);
    CHECK(app.myGamesTab() == 2);
    // Even with the cursor on the menu, L R keep moving the tabs.
    app.action(foil::ui::Action::Back);
    CHECK(app.onMenu());
    app.action(foil::ui::Action::Next);
    CHECK(app.current() == foil::ui::Screen::Installed);
    app.render();
  }
  {
    // 세이브 > 드롭박스: 연결하기 when not linked, the QR code while the phone
    // signs in (B gives up), then the games with their Dropbox backups.
    app.screen(foil::ui::Screen::Saves);
    app.action(foil::ui::Action::Next);
    app.action(foil::ui::Action::Next);
    CHECK(app.savesTab() == 2);
    app.previewDropbox(false);
    app.render();
    app.previewDropbox(false, {}, "https://www.dropbox.com/oauth2/authorize?client_id=k&state=s");
    app.render();
    app.action(foil::ui::Action::Next); // a sign-in in progress keeps the tab
    CHECK(app.savesTab() == 2);
    app.action(foil::ui::Action::Back);
    app.action(foil::ui::Action::Next);
    CHECK(app.savesTab() == 0);
    app.action(foil::ui::Action::Previous);
    CHECK(app.savesTab() == 2);
    app.previewDropbox(true, {{installed.applicationId, {{"20260311_090000", 1773219600, 1234}}}});
    app.render();
    app.action(foil::ui::Action::Accept);
    CHECK(app.dropboxPopupOpen());
    app.render();
    app.action(foil::ui::Action::Down);
    app.render();
    app.action(foil::ui::Action::Back);
    CHECK(!app.dropboxPopupOpen());
    // Up from the first game reaches the tabs; Down comes back to the games.
    app.action(foil::ui::Action::Up);
    CHECK(app.headerFocus() == "tab2");
    app.action(foil::ui::Action::Down);
    CHECK(app.headerFocus().empty());
    app.action(foil::ui::Action::Accept);
    CHECK(app.dropboxPopupOpen());
    app.render();
    app.action(foil::ui::Action::Back);
    CHECK(!app.dropboxPopupOpen());
    CHECK(app.current() == foil::ui::Screen::Saves);
  }
  char directory[] = "/tmp/cookingfoil-reload-XXXXXX";
  const auto root = mkdtemp(directory);
  CHECK(root != nullptr);
  if (root) {
    const auto configPath = std::string(root) + "/config.json";
    const auto statePath = std::string(root) + "/state.json";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[]})", error));
    foil::ui::Application reload(renderer, configPath, statePath, "host/fixtures/art");
    CHECK(reload.open());
    reload.setSession(std::make_unique<foil::net::Session>(other, foil::State{}, statePath));
    reload.setItems(parsed.value);
    CHECK(reload.groupCount() == 13);
    reload.action(foil::ui::Action::Reload);
    CHECK(reload.groupCount() == 0);
    CHECK(reload.current() == foil::ui::Screen::Settings);
    std::filesystem::remove_all(root);
  }
  return failures;
}
