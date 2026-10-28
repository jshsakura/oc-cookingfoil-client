#include "install/EngineHooks.hpp"
#include "Test.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/SaveBackups.hpp"
#include "core/ServerSaves.hpp"
#include "ui/Application.hpp"
#include "ui/Images.hpp"
#include "MockServer.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <atomic>
#include <mutex>
#include <set>
namespace foil::install {
void setHostInstall(std::function<void(const Item &)> run);
}
namespace foil::platform {
void setHostSpace(uint64_t bytes);
}
// reload() from the drawing thread reads the card on a worker; frames apply
// it, as on the console.
static void settle(foil::ui::Application &app) {
  for (int frame = 0; frame < 400 && app.reloading(); ++frame) {
    SDL_Delay(5);
    app.render();
  }
  app.render();
}
int runUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  {
    // The dots of the small icons show at 1:1 (the toast's info mark is 22
    // wide in handheld): a dot drawn as a zero-length stroke vanished there.
    float sx = 1, sy = 1;
    SDL_RenderGetScale(renderer, &sx, &sy);
    SDL_RenderSetScale(renderer, 1, 1);
    foil::ui::Painter painter(renderer);
    auto *format = SDL_AllocFormat(SDL_PIXELFORMAT_RGBA32);
    struct Dot { int kind; float x, y; };
    for (const Dot dot : {Dot{33, 12, 8}, Dot{25, 12, 16}, Dot{39, 12, 20}, Dot{20, 12, 20}, Dot{21, 6, 6}, Dot{15, 7, 7},
                          Dot{14, 10, 16}, Dot{27, 18, 11}}) {
      SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
      SDL_RenderClear(renderer);
      painter.icon(dot.kind, 0, 0, {255, 255, 255, 255}, 22);
      Uint32 pixels[9] = {};
      const SDL_Rect at{int(dot.x * 22 / 24) - 1, int(dot.y * 22 / 24) - 1, 3, 3};
      CHECK(SDL_RenderReadPixels(renderer, &at, SDL_PIXELFORMAT_RGBA32, pixels, 3 * sizeof(Uint32)) == 0);
      Uint8 brightest = 0;
      for (const auto pixel : pixels) {
        Uint8 r, g, b;
        SDL_GetRGB(pixel, format, &r, &g, &b);
        brightest = std::max(brightest, r);
      }
      CHECK(brightest > 160);
    }
    SDL_FreeFormat(format);
    SDL_RenderSetScale(renderer, sx, sy);
  }
  {
    // 설치 불가 in every language: the install button (386 wide) keeps a margin.
    foil::ui::Painter painter(renderer);
    CHECK(painter.fonts.open());
    const auto before = foil::language();
    for (int n = 0; n < foil::LanguageCount; ++n) {
      foil::setLanguage(foil::Language(n));
      CHECK(painter.keyLabelWidth("A", foil::tr("Can't install|button"), 22, true) <= 386 - 32);
    }
    foil::setLanguage(before);
  }
  {
    // The picture cache folder keeps the newest files under three quarters
    // of the limit once over it, and leaves other files alone.
    const auto folder = std::filesystem::temp_directory_path() / "cookingfoil-trim-test";
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    const auto now = std::filesystem::file_time_type::clock::now();
    for (int i = 0; i < 5; ++i) {
      const auto path = folder / ("p" + std::to_string(i) + ".image");
      std::ofstream(path) << std::string(1000, 'x');
      std::filesystem::last_write_time(path, now - std::chrono::hours(10 - i));
    }
    std::ofstream(folder / "note.txt") << std::string(5000, 'x');
    foil::ui::trimImageCache(folder.string(), 3000);
    CHECK(!std::filesystem::exists(folder / "p0.image"));
    CHECK(!std::filesystem::exists(folder / "p2.image"));
    CHECK(std::filesystem::exists(folder / "p3.image"));
    CHECK(std::filesystem::exists(folder / "p4.image"));
    CHECK(std::filesystem::exists(folder / "note.txt"));
    foil::ui::trimImageCache(folder.string(), 2000); // at the limit: nothing goes
    CHECK(std::filesystem::exists(folder / "p3.image"));
    // 설정 shows the pictures' size (other files not counted) and empties it.
    CHECK(foil::ui::imageCacheBytes(folder.string()) == 2000);
    CHECK(foil::ui::imageCacheTally() == 2000); // counted by the trim, no walk needed after
    foil::ui::clearImageCache(folder.string());
    CHECK(foil::ui::imageCacheBytes(folder.string()) == 0 && foil::ui::imageCacheTally() == 0);
    CHECK(std::filesystem::exists(folder / "note.txt"));
    // The limit: 128 MB, a tenth of a nearly full card's room, never under 64 MB.
    CHECK(foil::ui::imageCacheLimit(std::nullopt) == 128ull << 20);
    CHECK(foil::ui::imageCacheLimit(100ull << 30) == 128ull << 20);
    CHECK(foil::ui::imageCacheLimit(1000ull << 20) == 100ull << 20);
    CHECK(foil::ui::imageCacheLimit(100ull << 20) == 64ull << 20);
    std::filesystem::remove_all(folder);
  }
  {
    // A picture loaded ahead (the next banner slide) is asked for but not
    // drawn. It must stay decoded while a full screen of others is drawn,
    // or it is dropped and fetched again every frame.
    foil::ui::Images images(renderer);
    const auto bytes = foil::readFile("host/fixtures/art/0100001019F6E000.jpg");
    CHECK(!bytes.empty());
    images.provide("next", bytes);
    for (int i = 0; i < 30; ++i)
      images.provide("shown" + std::to_string(i), bytes);
    bool kept = true;
    for (int frame = 0; frame < 5; ++frame) {
      images.request("next");
      for (int i = 0; i < 30; ++i)
        images.draw("shown" + std::to_string(i), 0, 0, 10, 10);
      images.decode(100);
      kept = kept && (frame == 0 || images.loaded("next"));
    }
    CHECK(kept);
  }
  foil::ui::Fonts fonts(renderer);
  CHECK(fonts.open());
  CHECK(fonts.supports(0xD55C));
  CHECK(fonts.supports(0x65E5));
  CHECK(fonts.supports(0x4E2D));
  CHECK(fonts.measure("한글 日本 中文", 18) > 80);
  auto parsed =
      foil::net::parseSections(foil::readFile("host/fixtures/sections.json"));
  {
    // An empty 홈 (no server yet) offers 설정 on its notice; Left still
    // goes to the menu, as from any page's first item.
    foil::ui::Application empty(renderer, "", "", "host/fixtures/art");
    CHECK(empty.open());
    empty.screen(foil::ui::Screen::Home);
    empty.action(foil::ui::Action::Right);
    CHECK(!empty.onMenu());
    empty.action(foil::ui::Action::Left);
    CHECK(empty.onMenu() && empty.current() == foil::ui::Screen::Home);
    empty.action(foil::ui::Action::Right);
    empty.action(foil::ui::Action::Accept);
    CHECK(empty.current() == foil::ui::Screen::Settings);
  }
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
    // The touched item keeps the focus; the screen's content does not take it.
    CHECK(app.onMenu());
    app.render();
    app.touch(120, app.menuRowTop(1) + foil::ui::MenuRowHeight / 2);
    CHECK(app.current() == foil::ui::Screen::Library);
    CHECK(app.onMenu());
    // A touched header tab takes the cursor from the menu, like anything
    // else touched: 내 게임's 업데이트 tab.
    app.render();
    app.touch(120, app.menuRowTop(foil::ui::menuIndex(foil::ui::Screen::Installed)) + foil::ui::MenuRowHeight / 2);
    CHECK(app.current() == foil::ui::Screen::Installed);
    CHECK(app.onMenu());
    app.render();
    app.touch(820, 48);
    CHECK(app.current() == foil::ui::Screen::Updates);
    CHECK(!app.onMenu());
    CHECK(app.headerFocus() == "tab1");
    app.screen(foil::ui::Screen::Library);
    app.render();
    app.touch(330, 250);
    CHECK(app.current() == foil::ui::Screen::Detail);
    app.render();
    // The way back is the arrow and its label, not the band past them.
    app.touch(220, 30);
    CHECK(app.current() == foil::ui::Screen::Detail);
    app.render();
    app.touch(60, 30);
    CHECK(app.current() == foil::ui::Screen::Library);
  }
  {
    // The SD card's free space on the footer opens 연결 to PC. The host runs
    // as an applet, so what opens is the applet notice saying why not (the
    // popup itself is in the shots); B closes it.
    app.screen(foil::ui::Screen::Home);
    for (int frame = 0; frame < 400 && app.footerPending(); ++frame) {
      app.render();
      SDL_Delay(5);
    }
    app.render();
    const auto chip = app.sdChip();
    CHECK(chip.w >= 120 && chip.h >= 44); // a finger-sized target, not just the icon
    app.touch(chip.x + chip.w / 2, chip.y + chip.h / 2);
    CHECK(app.usbAppletShown() && !app.usbDriveOpen());
    app.render();
    app.touch(chip.x + chip.w / 2, chip.y + chip.h / 2); // the notice takes the touch
    CHECK(app.usbAppletShown());
    app.action(foil::ui::Action::Back);
    CHECK(!app.usbAppletShown());
    // Without touch (docked): its row in 설정 opens it too.
    app.screen(foil::ui::Screen::Settings);
    app.render();
    app.action(foil::ui::Action::Right);
    for (int step = 0; step < 40; ++step) app.action(foil::ui::Action::Down);
    // The bug report is last; Connect to PC sits three rows above it.
    for (int step = 0; step < 3; ++step) app.action(foil::ui::Action::Up);
    app.render();
    app.action(foil::ui::Action::Accept);
    CHECK(app.usbAppletShown());
    app.action(foil::ui::Action::Back);
    CHECK(!app.usbAppletShown());
  }
  {
    // A finger glides the list and leaves the cursor where it was; any
    // button brings the view back to it.
    app.screen(foil::ui::Screen::Library);
    app.render();
    const auto before = app.selectedName();
    app.fingerDown();
    app.dragBegin(700, 400);
    app.drag(0, -120);
    app.render();
    CHECK(app.gliding());
    CHECK(app.glidePos() > 0.4f && app.glidePos() < 0.6f); // 120px of a row (CardRowStep)
    CHECK(app.selectedName() == before);
    // Pulled far past the top it stretches at most 90px, then springs back.
    app.drag(0, 2000);
    CHECK(app.glidePos() < 0 && app.glidePos() * foil::ui::CardRowStep >= -90.5f);
    app.dragEnd(0, 0);
    for (int i = 0; i < 60; ++i) { SDL_Delay(5); app.render(); }
    CHECK(app.glidePos() == 0);
    // A flick coasts; the touch that stops it opens nothing.
    app.dragBegin(700, 400);
    app.drag(0, -30);
    app.dragEnd(0, -3000);
    SDL_Delay(20);
    app.render();
    app.fingerDown();
    app.touch(330, 250);
    CHECK(app.current() == foil::ui::Screen::Library);
    app.action(foil::ui::Action::Right);
    CHECK(!app.gliding());
    // The right stick moves the list, not the cursor.
    app.render();
    const auto cursor = app.selectedName();
    app.stick(1, 0.2f);
    CHECK(app.gliding() && app.glidePos() > 0);
    CHECK(app.selectedName() == cursor);
    // Pushed past the top it stretches a little, as under a finger, and
    // springs back once the stick is let go.
    app.stick(-1, 3.0f);
    CHECK(app.glidePos() < 0 && app.glidePos() * foil::ui::CardRowStep >= -90.5f);
    for (int i = 0; i < 80; ++i) { SDL_Delay(5); app.render(); }
    CHECK(app.glidePos() == 0);
    app.action(foil::ui::Action::Left);
    CHECK(!app.gliding());
    // Nothing glides where nothing scrolls: a drag there moves no cursor.
    app.screen(foil::ui::Screen::Home);
    app.action(foil::ui::Action::Back);
    app.render();
    const bool menu = app.onMenu();
    app.dragBegin(120, 300);
    app.drag(0, -200);
    CHECK(app.onMenu() == menu && app.current() == foil::ui::Screen::Home);
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
  {
    // A card touched while the cursor sat on the menu: A on the page still
    // acts on the page, it does not go back to 홈.
    app.action(foil::ui::Action::Back);
    CHECK(app.current() == foil::ui::Screen::Library);
    app.action(foil::ui::Action::Left);
    CHECK(app.onMenu());
    app.render();
    app.touch(252 * 3 / 2 / 1.5f + 60, 220 * 3 / 2 / 1.5f + 60);
    app.render();
    CHECK(app.current() == foil::ui::Screen::Detail && !app.onMenu());
    app.action(foil::ui::Action::Down);
    CHECK(app.current() == foil::ui::Screen::Detail);
  }
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
  {
    // The game's patches join the rows to install: the one it needs starts
    // on, the other off; another game's patch stays out.
    const auto id = app.detailId();
    CHECK(!id.empty());
    auto withPatches = detail;
    foil::UserPatch optional, required, other;
    optional.id = "font";
    optional.titleId = id;
    optional.name = "한글 글꼴";
    optional.size = 1000;
    optional.kinds = {"romfs"};
    required.id = "boot";
    required.titleId = id;
    required.name = "실행 패치";
    required.size = 300;
    required.required = true;
    other.id = "other";
    other.titleId = "0100000000010000";
    other.size = 7;
    withPatches.patches = {optional, required, other};
    const auto before = app.selectedBytes();
    app.setDetail(withPatches);
    CHECK(app.selectedBytes() == before + 300);
    app.render();
    // From the install button: the storage cards, the needed patch, then
    // the optional one, which A switches on.
    for (int n = 0; n < 12; ++n) app.action(foil::ui::Action::Down);
    app.action(foil::ui::Action::Up);
    app.action(foil::ui::Action::Up);
    app.action(foil::ui::Action::Up);
    app.action(foil::ui::Action::Accept);
    CHECK(app.selectedBytes() == before + 1300);
    for (int n = 0; n < 3; ++n) app.action(foil::ui::Action::Down);
    app.action(foil::ui::Action::Accept);
    CHECK(app.confirmingInstall());
    app.render();
    app.action(foil::ui::Action::Back);
    CHECK(!app.confirmingInstall());
    CHECK(app.queuedPatches() == 0);
    // Made for newer firmware than the console's: A says why and nothing
    // installs, a point release past it too, whatever the server offers.
    setenv("FOIL_FIRMWARE", "18.1.0", 1);
    auto newer = withPatches;
    newer.requiredFirmware = "20.0.0";
    app.setDetail(newer);
    app.render();
    app.action(foil::ui::Action::Accept);
    CHECK(!app.confirmingInstall() && app.alert().find("20.0.0") != std::string::npos);
    app.action(foil::ui::Action::Back);
    CHECK(app.alert().empty());
    newer.requiredFirmware = "18.2.0";
    app.setDetail(newer);
    app.render();
    app.action(foil::ui::Action::Accept);
    CHECK(!app.confirmingInstall() && app.alert().find("18.2.0") != std::string::npos);
    app.action(foil::ui::Action::Back);
    unsetenv("FOIL_FIRMWARE");
    // Unsigned (integrity signature-fail): not blocked; A opens the install
    // confirm, which warns, and it can be tried.
    auto unsignedOne = withPatches;
    foil::net::FileCheck failed;
    failed.verdict = "modified";
    failed.reason = "signature-fail";
    for (const auto &name : app.pickedFileNames()) {
      failed.file = name;
      unsignedOne.integrity.push_back(failed);
    }
    app.setDetail(unsignedOne);
    app.render();
    // Unsigned is the usual case (faked ticket signatures): a plain button.
    CHECK(!app.installWarned());
    app.action(foil::ui::Action::Accept);
    CHECK(app.confirmingInstall() && !app.optionConfirm() && app.alert().empty());
    app.render();
    app.action(foil::ui::Action::Back);
    // The detail's signed[] words it; still plain.
    for (const auto &name : app.pickedFileNames()) unsignedOne.unsignedFiles.push_back({name, "서명 없음", "서버 문구"});
    app.setDetail(unsignedOne);
    app.render();
    CHECK(!app.installWarned());
    app.action(foil::ui::Action::Accept);
    CHECK(app.confirmingInstall() && app.alert().empty());
    app.render();
    app.action(foil::ui::Action::Back);
    // A server plugin's option: A opens its confirm, in its words, and the
    // files carry its query.
    foil::net::FileOption option;
    option.id = "fw";
    option.label = "펌웨어 버전 무시 설치";
    option.confirmBody = "경고 문구";
    option.query = {{"fw", "18.1.0"}};
    app.previewFileOption(option);
    app.render();
    CHECK(app.installWarned());
    app.action(foil::ui::Action::Accept);
    CHECK(app.optionConfirm());
    app.render();
    const auto planned = app.optionFiles();
    CHECK(!planned.empty() && planned.front().variant && planned.front().url.find("fw=18.1.0") != std::string::npos);
    app.action(foil::ui::Action::Back);
    CHECK(!app.confirmingInstall() && !app.optionConfirm());
    // Its words while the server prepares and when it refuses go with the
    // files, and so does a changed file's hash.
    option.progress = "준비 중";
    option.failed = "거절";
    option.hashesChange = true;
    app.previewFileOption(option);
    app.render();
    const auto changed = app.optionFiles();
    CHECK(!changed.empty() && changed.front().hashesChange && changed.front().variantProgress == "준비 중" &&
          changed.front().variantFailed == "거절");
    // A blocked option says why and installs nothing.
    option.blocked = true;
    option.blockedTitle = "막힘";
    option.blockedBody = "이유";
    app.previewFileOption(option);
    app.render();
    app.action(foil::ui::Action::Accept);
    CHECK(!app.confirmingInstall() && !app.optionConfirm() && app.alert() == "이유");
    CHECK(!app.optionFiles().empty() && !app.optionFiles().front().variant);
    app.action(foil::ui::Action::Back);
    app.previewFileOption({});
  }
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
    CHECK(std::string(foil::ui::screenLabel(foil::ui::Screen::Installed)) == foil::tr("My games"));
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
    {
      // The banner's tags in the website's order (#218), a bare score named.
      foil::Group g;
      foil::Item base;
      base.score = 81;
      base.size = 4402341888ULL;
      base.categories = {"rpg", "action", "puzzle"};
      base.players = 2;
      base.releaseDate = 20240715;
      base.priceRegular = "10,000원"; // no sale: the regular price takes its place
      g.base = base;
      g.representative = base;
      g.representative.languages = {"ko", "en"};
      g.update = base;
      g.dlc = {base};
      std::vector<std::string> texts;
      for (const auto &tag : app.heroTags(g)) texts.push_back(tag.text);
      CHECK((texts == std::vector<std::string>{foil::trf("Rated {0}", {"81"}), "eShop " + foil::priceText("10,000원"),
                                               foil::tr("Korean"), "4.1 GB", "rpg",
                                               "action", foil::trf("Up to {0}", {"2"}), foil::trf("Released {0}|tag", {"2024.07.15"}),
                                               foil::tr("Update included"), "DLC 1"}));
      CHECK(app.heroTags(g).front().score == 81);
      base.players = 1; // one player says nothing
      base.score = 0;   // no score, no tag
      g.base = base;
      CHECK(app.heroTags(g).front().text == "eShop " + foil::priceText("10,000원"));
      CHECK(app.heroTags(g).size() == 8); // the price stays
    }
    // The cursor passing through the menu and back leaves DLC as it was:
    // 내 게임 is the screen shown, whichever of its tabs.
    app.action(foil::ui::Action::Back);
    CHECK(app.onMenu());
    app.action(foil::ui::Action::Up);
    app.action(foil::ui::Action::Down);
    std::this_thread::sleep_for(std::chrono::milliseconds(300)); // past the menu's rest
    app.render();
    CHECK(app.myGamesTab() == 2);
    app.action(foil::ui::Action::Right);
    CHECK(app.current() == foil::ui::Screen::Updates && app.myGamesTab() == 2);
    // Even with the cursor on the menu, L R keep moving the tabs.
    app.action(foil::ui::Action::Back);
    CHECK(app.onMenu());
    app.action(foil::ui::Action::Next);
    CHECK(app.current() == foil::ui::Screen::Installed);
    app.render();
  }
  {
    // 세이브 > 이 기기: latest backup first by default, Y for the next order,
    // and A opens the game drawn under the cursor, whatever the order.
    foil::ui::Application saves(renderer, "", "", "host/fixtures/art");
    CHECK(saves.open());
    std::vector<foil::InstalledGame> games(3);
    const char *names[] = {"가 게임", "나 게임", "다 게임"};
    for (int i = 0; i < 3; ++i) {
      games[i].applicationId = 0x0100000000010000ULL + uint64_t(i) * 0x10000;
      games[i].titleId = foil::saves::titleHex(games[i].applicationId);
      games[i].name = names[i];
      games[i].lastUpdated = uint64_t(10 - i);
    }
    saves.previewInstalled({{true, {}}, games});
    saves.screen(foil::ui::Screen::Saves);
    saves.previewBackups({{games[2].applicationId, {1, "20260301_000000"}},
                          {games[1].applicationId, {5, "20260101_000000"}}});
    CHECK(saves.saveRowName(0) == "다 게임" && saves.saveRowName(1) == "나 게임" && saves.saveRowName(2) == "가 게임");
    saves.render();
    saves.action(foil::ui::Action::Right);
    saves.action(foil::ui::Action::Accept);
    CHECK(saves.managedName() == "다 게임");
    saves.action(foil::ui::Action::Back);
    saves.action(foil::ui::Action::Queue); // 이름순
    CHECK(saves.saveRowName(0) == "가 게임");
    saves.action(foil::ui::Action::Queue); // 업데이트순
    CHECK(saves.saveRowName(0) == "가 게임" && saves.saveRowName(2) == "다 게임");
    saves.action(foil::ui::Action::Queue); // 백업 많은순
    CHECK(saves.saveRowName(0) == "나 게임");
    saves.action(foil::ui::Action::Queue); // back to the first
    CHECK(saves.saveRowName(0) == "다 게임");
    // Only games with a save on the console or a backup on the card are
    // listed: 가 게임 has neither once the console says which have saves.
    saves.previewSavedGames({games[1].applicationId});
    CHECK(saves.saveRowName(0) == "다 게임" && saves.saveRowName(1) == "나 게임" && saves.saveRowName(2).empty());
    saves.previewSavedGames({games[0].applicationId});
    CHECK(saves.saveRowName(2) == "가 게임");
    saves.render();
  }
  {
    // The footer's 연결됨 follows the console's link: out of sleep with the
    // Wi-Fi down it says so until the link or the server comes back. A link
    // never read up (its connection test blocked) keeps 연결됨.
    foil::ui::Application link(renderer, "", "", "host/fixtures/art");
    CHECK(link.open());
    link.previewSignal(3);
    link.setItems(parsed.value);
    CHECK(link.connectionLabel() == "Connected");
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    link.previewSignal(-1);
    CHECK(link.connectionLabel() == "No internet");
    link.render();
    link.previewSignal(2);
    CHECK(link.connectionLabel() == "Connected");
    // The server answering after the drop wins over the console's check.
    link.previewSignal(-1);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    link.setItems(parsed.value);
    CHECK(link.connectionLabel() == "Connected");
    foil::ui::Application blocked(renderer, "", "", "host/fixtures/art");
    CHECK(blocked.open());
    blocked.previewSignal(-1);
    blocked.setItems(parsed.value);
    blocked.previewSignal(-1);
    CHECK(blocked.connectionLabel() == "Connected");
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
    // Up from the first game reaches the account row, then the tabs; Down
    // comes back. A on the account asks to disconnect; Y changes the view,
    // three of them, and opens nothing.
    app.action(foil::ui::Action::Up);
    CHECK(app.headerFocus().empty() && app.dropboxOnAccount());
    CHECK(app.savesOrder() == foil::tr("Latest backup|sort"));
    app.action(foil::ui::Action::Queue);
    CHECK(!app.dropboxPopupOpen() && app.savesOrder() == foil::tr("Name|sort"));
    app.action(foil::ui::Action::Queue);
    CHECK(app.savesOrder() == foil::tr("With console|sort"));
    app.action(foil::ui::Action::Queue);
    CHECK(app.savesOrder() == foil::tr("Latest backup|sort"));
    app.action(foil::ui::Action::Accept);
    CHECK(app.dropboxPopupOpen()); // 드롭박스 연결을 해제할까요?
    app.render();
    app.action(foil::ui::Action::Back);
    CHECK(!app.dropboxPopupOpen());
    app.action(foil::ui::Action::Up);
    CHECK(app.headerFocus() == "tab2");
    app.action(foil::ui::Action::Down);
    CHECK(app.headerFocus().empty() && app.dropboxOnAccount());
    app.action(foil::ui::Action::Down);
    CHECK(!app.dropboxOnAccount());
    // The right stick pressed in goes to the first row, not past it.
    app.scrollToTop();
    CHECK(app.dropboxOnAccount() && app.headerFocus().empty());
    app.action(foil::ui::Action::Down);
    CHECK(!app.dropboxOnAccount());
    app.action(foil::ui::Action::Accept);
    CHECK(app.dropboxPopupOpen());
    app.render();
    app.action(foil::ui::Action::Back);
    CHECK(!app.dropboxPopupOpen());
    CHECK(app.current() == foil::ui::Screen::Saves);
    // A game backed up on Dropbox but not installed is listed too; its popup
    // only takes backups down (no upload row, and restoring is 내 기기's).
    const uint64_t elsewhere = 0x0100000000009000ULL;
    app.previewDropbox(true, {{installed.applicationId, {{"20260311_090000", 1773219600, 1234}}},
                              {elsewhere, {{"20260313_090000", 1773392400, 4321}}}});
    app.render();
    const auto listed = app.dropboxListed();
    CHECK(listed.size() == 2);
    const auto found = std::find_if(listed.begin(), listed.end(), [&](const auto &r) { return r.applicationId == elsewhere; });
    CHECK(found != listed.end() && !found->installed && found->remote == 1);
    // Both have Dropbox backups, the newest first: the one the server does not
    // know (named by its title id), and the cursor stayed on the other.
    CHECK(!listed.empty() && listed.front().applicationId == elsewhere);
    CHECK(app.dropboxFocused() == installed.applicationId);
    app.action(foil::ui::Action::Up);
    CHECK(app.dropboxFocused() == elsewhere);
    app.action(foil::ui::Action::Accept);
    CHECK(app.dropboxPopupOpen());
    app.render();
    CHECK(app.dropboxAcceptLabel() == foil::tr("Download to this console"));
    app.action(foil::ui::Action::Up);
    CHECK(app.dropboxAcceptLabel() == foil::tr("Download to this console"));
    CHECK(!app.dropboxChoosingUpload());
    app.action(foil::ui::Action::Back);
    CHECK(!app.dropboxPopupOpen());
    // The installed game keeps its upload row first.
    app.action(foil::ui::Action::Down);
    CHECK(app.dropboxFocused() == installed.applicationId);
    app.action(foil::ui::Action::Accept);
    CHECK(app.dropboxAcceptLabel() == foil::tr("Back up|to Dropbox") || app.dropboxAcceptLabel().empty());
    app.action(foil::ui::Action::Back);
    CHECK(!app.dropboxPopupOpen());
  }
  {
    // One backup is still picked from a list, and a shared save's backup or
    // server save goes only into the shared save.
    char sdDir[] = "/tmp/cookingfoil-saves-XXXXXX";
    const char *sd = mkdtemp(sdDir);
    CHECK(sd != nullptr);
    if (sd) {
      app.usePatchFolder(sd);
      const auto backup = foil::saves::keeperRoot(sd) + "/backups/" + foil::saves::titleHex(installed.applicationId) +
                          "/20260312_090000";
      CHECK(foil::makeDirectories(backup));
      std::ofstream(backup + "/world.dat") << "day 1";
      foil::saves::BackupMeta meta;
      meta.titleId = installed.applicationId;
      meta.backupName = meta.revisionId = "20260312_090000";
      meta.userId = "device";
      meta.createdAt = 1773306000;
      CHECK(!foil::saves::writeMetaFile(backup + ".meta", meta));
      // 드롭박스: nothing there lists nothing until Y adds this console's
      // games; the upload row then opens the console's backups, even one.
      app.previewDropbox(true, {});
      CHECK(app.dropboxListed().empty());
      app.render();
      app.action(foil::ui::Action::Queue);
      app.action(foil::ui::Action::Queue);
      CHECK(app.dropboxListed().size() == 1);
      app.action(foil::ui::Action::Down); // from the account row, where the empty list left it
      CHECK(app.savesTab() == 2 && !app.dropboxOnAccount());
      app.action(foil::ui::Action::Accept);
      CHECK(app.dropboxPopupOpen());
      app.action(foil::ui::Action::Accept);
      CHECK(app.dropboxChoosingUpload());
      app.render();
      app.action(foil::ui::Action::Back);
      CHECK(!app.dropboxChoosingUpload() && app.dropboxPopupOpen());
      app.action(foil::ui::Action::Back);
      CHECK(!app.dropboxPopupOpen());
      // 내 기기: the shared save and an account's, each with its panel.
      foil::SaveEntry shared;
      shared.applicationId = installed.applicationId;
      shared.saveId = 43;
      shared.type = 3;
      shared.userName = "공용 세이브";
      app.previewInstalled({{true, {{installed.titleId, 0}}}, {installed}}, {shared, save});
      app.action(foil::ui::Action::Next);
      CHECK(app.savesTab() == 0);
      app.action(foil::ui::Action::Accept);
      CHECK(app.managingGame());
      app.action(foil::ui::Action::Accept); // the shared save's panel
      app.action(foil::ui::Action::Down);   // its one backup
      app.action(foil::ui::Action::Accept);
      CHECK(app.confirmingRestore());
      app.render();
      app.action(foil::ui::Action::Back);
      app.action(foil::ui::Action::Back);
      app.action(foil::ui::Action::Down);   // 사용자 1
      app.action(foil::ui::Action::Accept);
      app.action(foil::ui::Action::Down);
      app.action(foil::ui::Action::Accept); // refused: it is the shared save's
      CHECK(!app.confirmingRestore());
      app.render();
      app.action(foil::ui::Action::Back);
      // A server save marked shared: the shared row asks, an account's refuses.
      foil::serversaves::ServerSave server;
      server.id = "s";
      server.titleId = installed.titleId;
      server.name = "Server save";
      server.saveType = "device";
      app.previewApplying(server);
      app.action(foil::ui::Action::Accept); // 사용자 1
      CHECK(!app.askingApply());
      app.action(foil::ui::Action::Up);
      app.action(foil::ui::Action::Accept); // 공용 세이브
      CHECK(app.askingApply());
      app.render();
      app.action(foil::ui::Action::Back);
      app.action(foil::ui::Action::Back);
      CHECK(!app.managingGame());
      std::filesystem::remove_all(sd);
    }
  }
  {
    // The card check runs on its own and ends: an enabled patch with a file
    // gone from the card reads as missing, and nothing shows as busy for it.
    char sdDir[] = "/tmp/cookingfoil-patchcheck-XXXXXX";
    const char *sd = mkdtemp(sdDir);
    CHECK(sd != nullptr);
    if (sd) {
      const std::string storeRoot = std::string(sd) + "/switch/cookingfoil/patches";
      const std::string file = "atmosphere/contents/0100F2C0115B6000/romfs/a.bin";
      {
        foil::PatchStore store(sd, storeRoot);
        foil::Error error;
        std::string ignored;
        CHECK(store.load(error) && foil::makeDirectories(store.staging("chk") + "/atmosphere/contents/0100F2C0115B6000/romfs"));
        foil::atomicWrite(store.staging("chk") + "/" + file, "a", ignored);
        foil::UserPatch patch;
        patch.id = "chk";
        patch.titleId = "0100F2C0115B6000";
        CHECK(store.commit(patch, error) && store.enable("chk", error));
      }
      std::filesystem::remove(std::string(sd) + "/" + file);
      app.usePatchFolder(sd);
      app.screen(foil::ui::Screen::Patches);
      bool busyShown = false;
      // Bounded: the check has a time budget, so it ends well within this.
      for (int frame = 0; frame < 3000 && (app.patchChecking() || !app.patchHealth().count("chk")); ++frame) {
        app.poll();
        app.render();
        busyShown |= app.patchJobShown();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      CHECK(!app.patchChecking() && !busyShown);
      CHECK(app.patchHealth().count("chk") && app.patchHealth().at("chk").missing == 1);
      app.screen(foil::ui::Screen::Home);
      std::filesystem::remove_all(sd);
    }
  }
  char directory[] = "/tmp/cookingfoil-reload-XXXXXX";
  const auto root = mkdtemp(directory);
  CHECK(root != nullptr);
  if (root) {
    const auto configPath = std::string(root) + "/config.json";
    const auto statePath = std::string(root) + "/cookingfoil.db";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[]})", error));
    foil::ui::Application reload(renderer, configPath, statePath, "host/fixtures/art");
    CHECK(reload.open());
    reload.setSession(std::make_unique<foil::net::Session>(other, foil::State{}, statePath));
    reload.setItems(parsed.value);
    CHECK(reload.groupCount() == 13);
    reload.action(foil::ui::Action::Reload);
    settle(reload);
    CHECK(reload.groupCount() == 0);
    // No server left: it stays put (홈 says how to add one), not 설정.
    CHECK(reload.current() != foil::ui::Screen::Settings);
    std::filesystem::remove_all(root);
  }
  char refreshDirectory[] = "/tmp/cookingfoil-refresh-XXXXXX";
  if (const auto refreshRoot = mkdtemp(refreshDirectory)) {
    // - on 홈 turns it to placeholders with the menu and the buttons, as on
    // every menu screen: the cursor waits on the menu until the answer.
    const auto configPath = std::string(refreshRoot) + "/config.json";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"title":"A","url":"http://127.0.0.1:1"}]})", error));
    foil::ui::Application home(renderer, configPath, std::string(refreshRoot) + "/cookingfoil.db", "host/fixtures/art");
    CHECK(home.open());
    settle(home);
    for (int frame = 0; frame < 400 && home.asking(); ++frame) { SDL_Delay(5); home.render(); }
    home.setItems(parsed.value);
    home.render();
    home.action(foil::ui::Action::Right); // into the rows
    home.render();
    CHECK(home.current() == foil::ui::Screen::Home && !home.onMenu());
    home.refreshAll(); // what - does (main.cpp)
    home.render();
    CHECK(home.onMenu());
    std::filesystem::remove_all(refreshRoot);
  }
  char togetherDirectory[] = "/tmp/cookingfoil-together-XXXXXX";
  if (const auto togetherRoot = mkdtemp(togetherDirectory)) {
    // Two servers slow to give their lists are asked together: the home
    // waits for the slower one, not for both in turn.
    static constexpr int SlowMs = 700;
    const auto slow = [](const std::string &request) -> Reply {
      if (request.rfind("GET /api/library/sections", 0) != 0) return {404, "", "{}"};
      std::this_thread::sleep_for(std::chrono::milliseconds(SlowMs));
      return {200, "", R"({"sections":[]})"};
    };
    Mock first, second;
    first.answer = slow;
    second.answer = slow;
    const auto configPath = std::string(togetherRoot) + "/config.json";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"title":"A","url":"http://127.0.0.1:)" + std::to_string(first.port) +
                                            R"("},{"title":"B","url":"http://127.0.0.1:)" + std::to_string(second.port) + R"("}]})",
                            error));
    foil::ui::Application both(renderer, configPath, std::string(togetherRoot) + "/cookingfoil.db", "host/fixtures/art");
    CHECK(both.open());
    for (int frame = 0; frame < 800 && both.asking(); ++frame) { SDL_Delay(5); both.render(); }
    first.take();
    second.take();
    const auto started = std::chrono::steady_clock::now();
    both.refreshAll();
    settle(both);
    for (int frame = 0; frame < 800 && both.asking(); ++frame) { SDL_Delay(5); both.render(); }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    CHECK(!both.asking());
    CHECK(first.take().size() >= 1 && second.take().size() >= 1);
    CHECK(took < 2 * SlowMs - 100);
    std::filesystem::remove_all(togetherRoot);
  }
  char stuckDirectory[] = "/tmp/cookingfoil-stuck-XXXXXX";
  if (const auto stuckRoot = mkdtemp(stuckDirectory)) {
    // A server that takes the connection and never answers: picking another
    // one meanwhile stops it within about a second and connects there,
    // instead of "Connecting to the server." until every timeout ran out.
    std::atomic<bool> letGo{false};
    Mock stuck, quick;
    stuck.answer = [&](const std::string &) -> Reply {
      for (int i = 0; i < 1500 && !letGo; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
      return {503, "", "{}"};
    };
    quick.answer = [](const std::string &request) -> Reply {
      if (request.rfind("GET /api/library/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
      return {404, "", "{}"};
    };
    const auto configPath = std::string(stuckRoot) + "/config.json";
    std::string error;
    const auto quickUrl = "http://127.0.0.1:" + std::to_string(quick.port);
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"title":"A","url":"http://127.0.0.1:)" + std::to_string(stuck.port) +
                                            R"("},{"title":"B","url":")" + quickUrl + R"("}]})",
                            error));
    foil::ui::Application app(renderer, configPath, std::string(stuckRoot) + "/cookingfoil.db", "host/fixtures/art");
    CHECK(app.open());
    for (int frame = 0; frame < 40; ++frame) { SDL_Delay(5); app.render(); }
    CHECK(app.asking());
    const auto picked = std::chrono::steady_clock::now();
    app.pickServer(1);
    for (int frame = 0; frame < 1000 && app.serverUsed() != quickUrl; ++frame) { SDL_Delay(5); app.render(); }
    const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - picked).count();
    CHECK(app.serverUsed() == quickUrl && took < 3000);
    letGo = true;
    std::filesystem::remove_all(stuckRoot);
  }
  char downDirectory[] = "/tmp/cookingfoil-down-XXXXXX";
  if (const auto downRoot = mkdtemp(downDirectory)) {
    // A server that does not answer: the card with 다시 시도 stays, and
    // nothing asks again on its own (each try put the skeleton back, which
    // read as a load that never ended).
    Mock down;
    down.answer = [](const std::string &) -> Reply { return {503, "", "{}"}; };
    const auto configPath = std::string(downRoot) + "/config.json";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"title":"A","url":"http://127.0.0.1:)" + std::to_string(down.port) + R"("}]})", error));
    foil::ui::Application app(renderer, configPath, std::string(downRoot) + "/cookingfoil.db", "host/fixtures/art");
    CHECK(app.open());
    for (int frame = 0; frame < 800 && app.asking(); ++frame) { SDL_Delay(5); app.render(); }
    CHECK(!app.asking());
    down.take();
    for (int frame = 0; frame < 600; ++frame) { SDL_Delay(10); app.render(); } // past the 5s the old retry waited
    CHECK(down.take().empty() && !app.asking());
    // The footer's link state, pressed, is the try again.
    app.pressLink();
    for (int frame = 0; frame < 800 && (app.asking() || app.reloading()); ++frame) { SDL_Delay(5); app.render(); }
    CHECK(!down.take().empty() && !app.serverInfoOpen());
    std::filesystem::remove_all(downRoot);
  }
  char adminDirectory[] = "/tmp/cookingfoil-adminhb-XXXXXX";
  if (const auto adminRoot = mkdtemp(adminDirectory)) {
    // An app's cores changed from its page by an admin console: the server
    // makes its shortcut again, so closing the editor reads the app list
    // again (before, only the server's lists were, and the page kept the old
    // shortcut until 홈브류 was left).
    Mock upstream;
    std::atomic<int> lists{0};
    std::string cores = "3";
    upstream.answer = [&](const std::string &request) -> Reply {
      if (request.rfind("GET /api/library/info", 0) == 0) return {200, "", R"({"features":["homebrew","admin-edit"]})"};
      if (request.rfind("GET /api/library/sections", 0) == 0) return {200, "", R"({"sections":[]})"};
      if (request.rfind("GET /api/homebrew?", 0) == 0) {
        ++lists;
        return {200, "", R"({"homebrew":[{"id":"gh-1","name":"Demo","version":"1.0.0","file_name":"demo.nro","hash":"sha256:ab",)"
                         R"("download_url":"/api/homebrew/gh-1/download","source":"github"}]})"};
      }
      const bool patch = request.rfind("PATCH /api/admin/items/homebrew/gh-1/fields", 0) == 0;
      if (patch || request.rfind("GET /api/admin/items/homebrew/gh-1/fields", 0) == 0) {
        if (patch) cores = "4";
        return {200, "", R"({"fields":[{"key":"cores","type":"choice","label":{"en":"Cores"},"value":")" + cores +
                             R"x(","options":[{"value":"3","label":{"en":"3 (default)"}},{"value":"4","label":{"en":"4"}}]}]})x"};
      }
      return {404, "", "{}"};
    };
    const auto configPath = std::string(adminRoot) + "/config.json";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"title":"A","url":"http://127.0.0.1:)" + std::to_string(upstream.port) + R"("}]})", error));
    foil::ui::Application admin(renderer, configPath, std::string(adminRoot) + "/cookingfoil.db", "host/fixtures/art");
    CHECK(admin.open());
    for (int frame = 0; frame < 800 && admin.asking(); ++frame) { SDL_Delay(5); admin.render(); }
    admin.screen(foil::ui::Screen::Homebrew);
    for (int frame = 0; frame < 400 && (lists == 0 || !admin.homebrewCardScanned()); ++frame) { SDL_Delay(5); admin.render(); }
    settle(admin);
    CHECK(lists == 1);
    if (admin.onMenu()) admin.action(foil::ui::Action::Right);
    admin.action(foil::ui::Action::Next); // 도구: the app is not on the card
    for (int frame = 0; frame < 400 && admin.homebrewListed() == 0; ++frame) { SDL_Delay(5); admin.render(); }
    CHECK(admin.homebrewListed() == 1);
    settle(admin);
    admin.action(foil::ui::Action::Down); // off the tabs, onto the app
    admin.action(foil::ui::Action::Accept);
    CHECK(admin.appPopupOpen());
    admin.render();
    admin.action(foil::ui::Action::Up); // the admin chip, above the panel's first row
    settle(admin);
    admin.action(foil::ui::Action::Accept);
    CHECK(admin.adminEditOpen());
    for (int frame = 0; frame < 400 && admin.adminEditBusy(); ++frame) { SDL_Delay(5); admin.render(); }
    for (int frame = 0; frame < 60; ++frame) { SDL_Delay(10); admin.render(); } // the editor settles
    admin.action(foil::ui::Action::Accept); // cores: 3 → 4
    for (int frame = 0; frame < 400 && admin.adminEditBusy(); ++frame) { SDL_Delay(5); admin.render(); }
    CHECK(cores == "4");
    admin.action(foil::ui::Action::Back);
    CHECK(!admin.adminEditOpen());
    for (int frame = 0; frame < 400 && lists < 2; ++frame) { SDL_Delay(5); admin.render(); }
    CHECK(lists == 2);
    // Connected, the footer's link state opens the servers' popup; B closes it.
    for (int frame = 0; frame < 800 && (admin.asking() || admin.reloading()); ++frame) { SDL_Delay(5); admin.render(); }
    admin.pressLink();
    CHECK(admin.serverInfoOpen());
    admin.render();
    admin.action(foil::ui::Action::Back);
    CHECK(!admin.serverInfoOpen());
    std::filesystem::remove_all(adminRoot);
  }
  char bugDirectory[] = "/tmp/cookingfoil-bug-XXXXXX";
  if (const auto bugRoot = mkdtemp(bugDirectory)) {
    // A bug report goes to a server the player picks among the ones on,
    // starting from the one the lists come from; one that is off is not offered.
    const auto configPath = std::string(bugRoot) + "/config.json";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"title":"A","url":"http://127.0.0.1:1"},
      {"title":"B","url":"http://127.0.0.1:2","enabled":false},{"title":"C","url":"http://127.0.0.1:3"}]})", error));
    foil::ui::Application bug(renderer, configPath, std::string(bugRoot) + "/cookingfoil.db", "host/fixtures/art");
    CHECK(bug.open());
    bug.openBugReport("");
    CHECK(bug.bugReportOpen() && bug.bugReportServer() == 0);
    bug.action(foil::ui::Action::Down);
    CHECK(bug.bugReportServer() == 2);
    bug.action(foil::ui::Action::Down); // the last stays
    CHECK(bug.bugReportServer() == 2);
    bug.action(foil::ui::Action::Up);
    CHECK(bug.bugReportServer() == 0);
    bug.action(foil::ui::Action::Back);
    CHECK(!bug.bugReportOpen());
    std::filesystem::remove_all(bugRoot);
  }
  char pullDirectory[] = "/tmp/cookingfoil-pull-XXXXXX";
  if (const auto pullRoot = mkdtemp(pullDirectory)) {
    // A list of short rows pulled past its top stretches more than a row:
    // the first row drawn is still the first, not one before it (레트로
    // 게임's versions closed the app this way).
    const auto configPath = std::string(pullRoot) + "/config.json";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"url":"http://127.0.0.1:1"},{"url":"http://127.0.0.1:2"},
      {"url":"http://127.0.0.1:3"},{"url":"http://127.0.0.1:4"},{"url":"http://127.0.0.1:5"}]})", error));
    foil::ui::Application pull(renderer, configPath, std::string(pullRoot) + "/cookingfoil.db", "host/fixtures/art");
    CHECK(pull.open());
    pull.openBugReport("");
    pull.render();
    pull.fingerDown();
    pull.dragBegin(640, 400);
    pull.drag(0, 2000);
    pull.render();
    CHECK(pull.gliding() && pull.glidePos() < -1);
    pull.dragEnd(0, 0);
    for (int i = 0; i < 60; ++i) { SDL_Delay(5); pull.render(); }
    CHECK(pull.glidePos() == 0 && pull.bugReportOpen());
    std::filesystem::remove_all(pullRoot);
  }
  char formDirectory[] = "/tmp/cookingfoil-form-XXXXXX";
  const auto formRoot = mkdtemp(formDirectory);
  CHECK(formRoot != nullptr);
  if (formRoot) {
    // 설정 > + 서버 추가: the form starts empty, each field asks for the
    // keyboard, 저장 writes config.json and connects to the new server.
    const auto configPath = std::string(formRoot) + "/config.json";
    const auto statePath = std::string(formRoot) + "/cookingfoil.db";
    std::string error;
    CHECK(foil::atomicWrite(configPath, R"({"servers":[{"title":"Old","url":"http://127.0.0.1:1","enabled":false}]})", error));
    foil::ui::Application form(renderer, configPath, statePath, "host/fixtures/art");
    CHECK(form.open());
    // No server on: the app does not jump to 설정 by itself.
    CHECK(form.current() != foil::ui::Screen::Settings);
    form.screen(foil::ui::Screen::Settings);
    form.render();
    // One server card, then "+ 서버 추가" under it, both 52 tall with a 12 gap.
    form.touch(700, 86 + 52 + 12 + 26);
    CHECK(form.serverFormOpen());
    CHECK(!form.takeTextRequest());
    // Y with no address stays in the form and writes nothing.
    const auto before = foil::readFile(configPath);
    form.action(foil::ui::Action::Queue);
    CHECK(form.serverFormOpen());
    CHECK(foil::readFile(configPath) == before);
    form.action(foil::ui::Action::Accept);
    auto request = form.takeTextRequest();
    CHECK(request && request->initial.empty() && !request->password);
    // Another scheme is refused; a bare address would get https://.
    form.enterText("ftp://127.0.0.1:1");
    form.action(foil::ui::Action::Reload);
    CHECK(form.serverFormOpen());
    CHECK(foil::readFile(configPath) == before);
    form.enterText("http://127.0.0.1:1/");
    form.action(foil::ui::Action::Down);
    form.action(foil::ui::Action::Accept);
    request = form.takeTextRequest();
    CHECK(request && request->initial.empty() && !request->password);
    form.enterText("player");
    form.action(foil::ui::Action::Down);
    form.action(foil::ui::Action::Accept);
    request = form.takeTextRequest();
    CHECK(request && request->password);
    form.enterText("secret word");
    form.render();
    // 이름 stays empty; the last row takes the approval message, then Y saves.
    form.action(foil::ui::Action::Down);
    form.action(foil::ui::Action::Down);
    form.action(foil::ui::Action::Accept);
    request = form.takeTextRequest();
    CHECK(request && request->initial.empty() && request->maxLength == 100);
    form.enterText("  거실 기기\n");
    form.action(foil::ui::Action::Queue);
    CHECK(!form.serverFormOpen());
    // Kept by address in state, cleaned as the server would, never in config.json.
    CHECK(form.pairMessageFor("http://127.0.0.1:1") == "거실 기기");
    CHECK(foil::readFile(configPath).find("거실") == std::string::npos);
    auto saved = foil::loadConfig(configPath);
    CHECK(!saved.error && saved.value.servers.size() == 2);
    if (saved.value.servers.size() == 2) {
      CHECK(saved.value.servers[0].title == "Old" && !saved.value.servers[0].enabled);
      const auto &added = saved.value.servers[1];
      CHECK(added.url == "http://127.0.0.1:1" && added.title == "127.0.0.1:1" && added.enabled);
      CHECK(added.username == "player" && added.password == "secret word");
    }
    CHECK(form.config().servers.size() == 2);
    CHECK(form.activeServer() == 1);
    // A on the server opens it on 주소; Down to 이름, a new name, Y saves.
    SDL_Delay(300);
    form.render();
    form.action(foil::ui::Action::Accept);
    CHECK(form.serverFormOpen());
    for (int n = 0; n < 3; ++n) form.action(foil::ui::Action::Down);
    form.action(foil::ui::Action::Accept);
    request = form.takeTextRequest();
    CHECK(request && request->initial == "127.0.0.1:1");
    form.enterText("새 서버");
    form.action(foil::ui::Action::Queue);
    CHECK(!form.serverFormOpen());
    saved = foil::loadConfig(configPath);
    CHECK(saved.value.servers.size() == 2 && saved.value.servers[1].title == "새 서버" &&
          saved.value.servers[1].password == "secret word");
    // X deletes, asking first, then leaves the other server alone.
    SDL_Delay(300);
    form.render();
    form.action(foil::ui::Action::Accept);
    form.action(foil::ui::Action::Search);
    form.render();
    SDL_Delay(750);
    form.action(foil::ui::Action::Accept);
    CHECK(!form.serverFormOpen());
    saved = foil::loadConfig(configPath);
    CHECK(saved.value.servers.size() == 1 && saved.value.servers[0].title == "Old");
    // B closes the form without writing.
    const auto kept = foil::readFile(configPath);
    form.render();
    form.action(foil::ui::Action::Accept);
    CHECK(form.serverFormOpen());
    form.action(foil::ui::Action::Back);
    CHECK(!form.serverFormOpen());
    CHECK(foil::readFile(configPath) == kept);
    std::filesystem::remove_all(formRoot);
  }
  {
    // One download at a time, in the order things were added, whatever they
    // are; a finished install leaves 진행 중 and the next one starts.
    std::mutex gateLock;
    std::set<std::string> allowed;
    std::atomic<bool> giveUp{false};
    foil::install::setHostInstall([&](const foil::Item &item) {
      for (int waited = 0; waited < 500 && !giveUp; ++waited) {
        {
          std::lock_guard<std::mutex> held(gateLock);
          if (allowed.count(item.name)) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
    const auto allow = [&](const std::string &name) {
      std::lock_guard<std::mutex> held(gateLock);
      allowed.insert(name);
    };
    const auto file = [](const std::string &name) {
      foil::Item item;
      item.name = name;
      item.url = "https://server.example/" + name + ".nsp";
      item.titleId = item.baseId = "01000000000" + std::to_string(10000 + name.size()) + "0";
      item.size = 1000;
      return item;
    };
    const auto waitFor = [&](foil::ui::Application &on, const std::function<bool()> &done) {
      for (int i = 0; i < 300 && !done(); ++i) {
        on.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return done();
    };
    using foil::history::Outcome;
    foil::platform::setHostSpace(64ull << 30);
    {
      // An app installed as a title (homebrew-titles) goes in as a game's
      // NSP does, and the console's installed titles say whether it is there.
      foil::ui::Application titles(renderer, "", "", "host/fixtures/art");
      CHECK(titles.open());
      titles.previewQueueOpen();
      foil::homebrew::App tv;
      tv.id = "tvswiki";
      tv.name = "TVSWIKI";
      tv.version = "1.0.9";
      tv.title = true;
      tv.titleId = "053F13457F6B0000";
      tv.downloadUrl = "https://server.example/api/homebrew/tvswiki/download";
      tv.size = 63ull << 20;
      titles.previewHomebrew({tv});
      CHECK(titles.previewHomebrewStatus(tv) == foil::homebrew::Status::Available);
      foil::InstalledGame game;
      game.titleId = tv.titleId;
      game.applicationId = 0x053F13457F6B0000ULL;
      game.displayVersion = "1.0.8";
      titles.previewInstalled({{true, {}}, {game}});
      CHECK(titles.previewHomebrewStatus(tv) == foil::homebrew::Status::Update);
      game.displayVersion = "1.0.9";
      titles.previewInstalled({{true, {}}, {game}});
      CHECK(titles.previewHomebrewStatus(tv) == foil::homebrew::Status::Installed);
      CHECK(titles.previewGetHomebrew(tv));
      CHECK(titles.installingName() == "TVSWIKI");
    }
    foil::ui::Application queue(renderer, "", "", "host/fixtures/art");
    CHECK(queue.open());
    queue.previewQueueOpen();
    CHECK(!queue.previewPick({file("A")}, "A"));
    CHECK(!queue.previewPick({file("B")}, "B"));
    CHECK(!queue.previewPick({file("C")}, "C"));
    CHECK(queue.installingName() == "A");
    CHECK((queue.queuedNames() == std::vector<std::string>{"B", "C"}));
    // A homebrew app and a patch asked for meanwhile wait their turn behind
    // what was there; nothing else starts beside the install.
    foil::homebrew::App app;
    app.id = "hb";
    app.name = "HB";
    app.downloadUrl = "https://server.example/hb.nro";
    app.size = 10;
    CHECK(queue.previewGetHomebrew(app));
    CHECK(!queue.otherDownloadRunning());
    foil::UserPatch patch;
    patch.id = "p-1";
    patch.titleId = "0100000000010000";
    patch.name = "D patch";
    patch.gameName = "D"; // queued as "D · D patch", the game first
    CHECK(!queue.previewPick({file("D")}, "D", {patch}));
    CHECK((queue.queuedNames() == std::vector<std::string>{"B", "C", "HB", "D", "D · D patch"}));
    CHECK(!queue.previewGetHomebrew(app)); // the same app is not queued twice
    queue.screen(foil::ui::Screen::Downloads);
    queue.render();
    // A ends: it is 완료, B runs (its card on 진행 중), C waits.
    allow("A");
    CHECK(waitFor(queue, [&] { return queue.installingName() == "B"; }));
    CHECK(queue.outcomeOf("A") == Outcome::Done);
    CHECK(queue.outcomeOf("B") == Outcome::Running);
    CHECK(queue.outcomeOf("C") == Outcome::Queued);
    CHECK((queue.queuedNames() == std::vector<std::string>{"C", "HB", "D", "D · D patch"}));
    queue.render();
    // B and C end; the app's turn comes before D's (it cannot start here,
    // with no server, so it reads as failed and D goes on).
    allow("B");
    allow("C");
    CHECK(waitFor(queue, [&] { return queue.installingName() == "D"; }));
    CHECK(queue.outcomeOf("C") == Outcome::Done);
    CHECK(queue.outcomeOf("HB") == Outcome::Failed);
    CHECK(queue.outcomeOf("D · D patch") == Outcome::Queued);
    queue.render();
    giveUp = true;
    allow("D");
    CHECK(waitFor(queue, [&] { return queue.installingName().empty(); }));
    foil::install::setHostInstall(nullptr);
    foil::platform::setHostSpace(0);
  }
  {
    // Only a finished download's HOME Menu shortcut left (the app closed
    // right after its message): it goes on, nothing is asked.
    std::atomic<bool> release{false};
    foil::install::setHostInstall([&](const foil::Item &) {
      for (int waited = 0; waited < 500 && !release; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    foil::platform::setHostSpace(64ull << 30);
    foil::Item shortcut;
    shortcut.name = "S shortcut";
    shortcut.url = "https://server.example/api/homebrew/gh-1/forwarder";
    shortcut.titleId = shortcut.baseId = "0500000000001000";
    shortcut.size = 1000;
    shortcut.type = "shortcut";
    foil::ui::Application starting(renderer, "", "", "host/fixtures/art");
    CHECK(starting.open());
    const auto waitFor = [&](foil::ui::Application &on, const std::function<bool()> &done) {
      for (int i = 0; i < 300 && !done(); ++i) {
        on.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      return done();
    };
    starting.previewQueued(shortcut, "S shortcut", "");
    starting.previewOfferResume();
    CHECK(!starting.previewResumeAsked());
    CHECK(waitFor(starting, [&] { return !starting.installingName().empty(); }));
    release = true;
    CHECK(waitFor(starting, [&] { return starting.installingName().empty(); }));
    foil::install::setHostInstall(nullptr);
    foil::platform::setHostSpace(0);
  }
  {
    // What was cut off is asked about once the server answers, not started
    // at once: B leaves it paused in 다운로드, A there goes on with it, and
    // leaving while it is paused asks nothing (its place is kept).
    std::atomic<bool> release{false};
    foil::install::setHostInstall([&](const foil::Item &) {
      for (int waited = 0; waited < 500 && !release; ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    });
    foil::platform::setHostSpace(64ull << 30);
    foil::Item item;
    item.name = "A";
    item.url = "https://server.example/A.nsp";
    item.titleId = item.baseId = "0100000000020000";
    item.size = 1000;
    foil::ui::Application starting(renderer, "", "", "host/fixtures/art");
    CHECK(starting.open());
    starting.previewQueued(item, "A", "");
    starting.previewOfferResume();
    CHECK(starting.previewResumeAsked());
    CHECK(starting.installingName().empty()); // asked, not started
    starting.render();
    starting.action(foil::ui::Action::Back);
    CHECK(!starting.previewResumeAsked());
    CHECK(starting.installingName().empty());
    CHECK((starting.queuedNames() == std::vector<std::string>{"A"}));
    starting.screen(foil::ui::Screen::Downloads); // its card, paused
    starting.render();
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    starting.action(foil::ui::Action::Accept);
    CHECK(starting.installingName() == "A");
    starting.previewPause(true);
    CHECK(starting.requestQuit()); // paused: out at once
    starting.previewPause(false);
    CHECK(!starting.requestQuit()); // running: asks first
    release = true;
    for (int i = 0; i < 300 && !starting.installingName().empty(); ++i) {
      starting.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(starting.installingName().empty());
    foil::install::setHostInstall(nullptr);
    foil::platform::setHostSpace(0);
  }
  {
    // Leaving while a download runs stops it, which is not cancelling: it
    // stays in 진행 중 (not 취소 or 실패) with what waited behind it, and
    // the app closes once it has stopped.
    foil::install::setHostInstall([](const foil::Item &) {
      auto *sink = foil::install::engineSink();
      for (int waited = 0; waited < 500 && !(sink && sink->cancellationRequested()); ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      throw std::runtime_error("cut");
    });
    const auto file = [](const std::string &name) {
      foil::Item item;
      item.name = name;
      item.url = "https://server.example/" + name + ".nsp";
      item.titleId = item.baseId = "0100000000020000";
      item.size = 1000;
      return item;
    };
    foil::platform::setHostSpace(64ull << 30);
    foil::ui::Application leaving(renderer, "", "", "host/fixtures/art");
    CHECK(leaving.open());
    leaving.previewQueueOpen();
    CHECK(!leaving.previewPick({file("A")}, "A"));
    auto second = file("B");
    second.titleId = second.baseId = "0100000000030000";
    CHECK(!leaving.previewPick({second}, "B"));
    CHECK(leaving.installingName() == "A");
    CHECK(!leaving.requestQuit()); // asks first
    leaving.render();
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    leaving.action(foil::ui::Action::Accept);
    for (int i = 0; i < 300 && !leaving.shouldQuit(); ++i) {
      leaving.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(leaving.shouldQuit());
    CHECK(leaving.outcomeOf("A") == foil::history::Outcome::Running);
    CHECK((leaving.queuedNames() == std::vector<std::string>{"B"}));
    foil::install::setHostInstall(nullptr);
    foil::platform::setHostSpace(0);
  }
  {
    // 다운로드's lists glide under the right stick (and a finger): the queue
    // on 진행 중 and each history tab.
    foil::ui::Application lists(renderer, "", "", "host/fixtures/art");
    CHECK(lists.open());
    std::vector<foil::history::Entry> rows;
    for (int i = 0; i < 20; ++i) {
      foil::history::Entry e;
      e.id = uint64_t(i + 1);
      e.install.name = "Done " + std::to_string(i);
      e.install.target = "sd";
      foil::Item f;
      f.url = "https://server.example/gone" + std::to_string(i) + ".nsp";
      e.install.files = {f};
      e.outcome = foil::history::Outcome::Done;
      rows.push_back(e);
    }
    lists.previewHistory(rows);
    lists.screen(foil::ui::Screen::Downloads);
    lists.action(foil::ui::Action::Previous); // 완료 is the last tab
    CHECK(lists.downloadsTab() == 3);
    lists.render();
    lists.stick(1, 0.5f);
    lists.render();
    CHECK(lists.glidePos() > 0);
    for (int i = 0; i < 12; ++i) {
      foil::Item f;
      f.name = "Q" + std::to_string(i);
      f.url = "https://server.example/q" + std::to_string(i) + ".nsp";
      f.size = 10;
      lists.previewQueued(f, f.name, "");
    }
    lists.action(foil::ui::Action::Next); // back to 진행 중
    CHECK(lists.downloadsTab() == 0);
    lists.render();
    lists.stick(1, 0.5f);
    lists.render();
    CHECK(lists.glidePos() > 0);
  }
  {
    // A row of 완료 opens the game's page; one the server no longer lists
    // says so.
    foil::ui::Application done(renderer, "", "", "host/fixtures/art");
    CHECK(done.open());
    done.setItems(parsed.value);
    done.previewQueueOpen();
    const auto &known = parsed.value.front();
    foil::history::Entry there, gone;
    there.id = 2;
    there.install.name = "there";
    there.install.files = {known};
    there.outcome = foil::history::Outcome::Done;
    gone.id = 1;
    gone.install.name = "gone";
    foil::Item missing;
    missing.url = "https://server.example/removed.nsp";
    missing.titleId = missing.baseId = "01FFFFFFFFFF0000";
    gone.install.files = {missing};
    gone.outcome = foil::history::Outcome::Done;
    done.previewHistory({there, gone});
    done.screen(foil::ui::Screen::Downloads);
    done.action(foil::ui::Action::Previous);
    done.render();
    done.action(foil::ui::Action::Right); // into the list
    done.action(foil::ui::Action::Accept);
    CHECK(done.current() == foil::ui::Screen::Detail);
    CHECK(done.detailId() == foil::history::catalogId(there));
    done.screen(foil::ui::Screen::Downloads);
    CHECK(done.downloadsTab() == 3); // the tab stays
    done.render();
    done.action(foil::ui::Action::Right);
    done.action(foil::ui::Action::Down);
    done.action(foil::ui::Action::Accept);
    CHECK(done.current() == foil::ui::Screen::Downloads);
    CHECK(done.alert() == std::string(foil::tr("The file no longer exists on the server.")));
  }
  {
    // Paused before it began, the top card still opens the game's page.
    foil::ui::Application held(renderer, "", "", "host/fixtures/art");
    CHECK(held.open());
    held.setItems(parsed.value);
    held.previewQueueOpen();
    const auto &known = parsed.value.front();
    foil::history::Entry waiting;
    waiting.id = 7;
    waiting.install.name = "waiting";
    waiting.install.files = {known};
    waiting.outcome = foil::history::Outcome::Queued;
    held.previewHistory({waiting});
    held.previewHeld(known, "waiting", 7);
    held.screen(foil::ui::Screen::Downloads);
    held.render();
    CHECK(held.previewTopDetails());
    CHECK(held.current() == foil::ui::Screen::Detail);
  }
  return failures;
}
