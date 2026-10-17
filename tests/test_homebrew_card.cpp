#include "Test.hpp"
#include "core/HomebrewCard.hpp"
#include "ui/ShortcutIcon.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>
using namespace foil::homebrew;
namespace {
void put32(std::string &b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = char(v >> (8 * i));
}
void put64(std::string &b, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[at + i] = char(v >> (8 * i));
}
// A NACP with a name in some title entries and a display version.
std::string nacp(const std::vector<std::pair<int, std::string>> &names, const std::string &version) {
  std::string b(0x4000, '\0');
  for (const auto &[entry, name] : names) {
    b.replace(size_t(entry) * 0x300, name.size(), name);
    b.replace(size_t(entry) * 0x300 + 0x200, 6, "author");
  }
  b.replace(0x3060, version.size(), version);
  return b;
}
const std::string Jpeg = std::string("\xFF\xD8\xFF\xE0", 4) + "jpeg body";
// A small .nro: the header, a little code, then the asset section with the
// icon and the NACP.
std::string nro(const std::string &control, const std::string &icon = Jpeg, const char *magic = "NRO0",
                const char *assetMagic = "ASET") {
  constexpr size_t Code = 0x100;
  std::string b(Code, '\0');
  b.replace(0x10, 4, magic);
  put32(b, 0x18, Code);
  std::string asset(0x38, '\0');
  asset.replace(0, 4, assetMagic);
  put64(asset, 0x08, 0x38);
  put64(asset, 0x10, icon.size());
  put64(asset, 0x18, 0x38 + icon.size());
  put64(asset, 0x20, control.size());
  return b + asset + icon + control;
}
void write(const std::filesystem::path &path, const std::string &bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
} // namespace
int runHomebrewCardTests() {
  int failures = 0;
  {
    // A shortcut's icon from the server's picture: its middle square, made
    // 256x256 JPEG within the control data's room; nothing from no picture.
    SDL_Surface *wide = SDL_CreateRGBSurfaceWithFormat(0, 400, 300, 24, SDL_PIXELFORMAT_RGB24);
    CHECK(wide);
    if (wide) {
      SDL_FillRect(wide, nullptr, SDL_MapRGB(wide->format, 0, 0, 255));
      const SDL_Rect middle{50, 0, 300, 300};
      SDL_FillRect(wide, &middle, SDL_MapRGB(wide->format, 255, 0, 0));
      std::string png(1 << 20, '\0'), bmp(1 << 20, '\0');
      SDL_RWops *rw = SDL_RWFromMem(png.data(), int(png.size()));
      CHECK(IMG_SavePNG_RW(wide, rw, 0) == 0);
      png.resize(size_t(SDL_RWtell(rw)));
      SDL_RWclose(rw);
      rw = SDL_RWFromMem(bmp.data(), int(bmp.size()));
      CHECK(SDL_SaveBMP_RW(wide, rw, 0) == 0);
      bmp.resize(size_t(SDL_RWtell(rw)));
      SDL_RWclose(rw);
      SDL_FreeSurface(wide);
      // Only JPEG and PNG are read (WebP crashed on the console): a BMP is not.
      CHECK(foil::ui::shortcutIconJpeg(bmp).empty());
      const auto icon = foil::ui::shortcutIconJpeg(png);
      CHECK(icon.size() > 4 && icon.size() <= 0x20000 && uint8_t(icon[0]) == 0xFF && uint8_t(icon[1]) == 0xD8);
      SDL_Surface *back = IMG_Load_RW(SDL_RWFromConstMem(icon.data(), int(icon.size())), 1);
      CHECK(back && back->w == 256 && back->h == 256);
      if (back) {
        SDL_Surface *rgb = SDL_ConvertSurfaceFormat(back, SDL_PIXELFORMAT_RGB24, 0);
        if (rgb) {
          // The edges were blue outside the middle square: none of it shows.
          const auto *edge = static_cast<const uint8_t *>(rgb->pixels) + 128 * rgb->pitch + 2 * 3;
          CHECK(edge[0] > 200 && edge[2] < 60);
          SDL_FreeSurface(rgb);
        }
        SDL_FreeSurface(back);
      }
    }
    CHECK(foil::ui::shortcutIconJpeg("").empty());
    CHECK(foil::ui::shortcutIconJpeg("not a picture").empty());
  }
  const auto ko = nacpLanguages("ko"), en = nacpLanguages("en"), zh = nacpLanguages("zh");
  CHECK(ko == (std::vector<int>{12, 0, 1}));
  CHECK(en == (std::vector<int>{0, 1}));
  CHECK(zh == (std::vector<int>{14, 13, 0, 1}));
  // A valid file: the name in the interface language, else English, else
  // whichever entry has one; the display version; the JPEG icon.
  const auto file = nro(nacp({{0, "Tester"}, {12, "테스터"}}, "1.2.0"));
  NroInfo info;
  CHECK(parseNro(file, ko, info) && info.name == "테스터" && info.version == "1.2.0" && info.icon == Jpeg);
  CHECK(info.author == "author");
  CHECK(parseNro(file, en, info) && info.name == "Tester");
  CHECK(parseNro(nro(nacp({{2, "テスター"}}, "")), en, info) && info.name == "テスター" && info.version.empty());
  // Wrong magic: no .nro at all.
  CHECK((!parseNro(nro(nacp({{0, "X"}}, "1"), Jpeg, "NRO1"), en, info)));
  CHECK(!parseNro("", en, info));
  CHECK(!parseNro(std::string(0x7F, '\0'), en, info));
  // Truncated or a broken asset section: still a .nro, nothing read from it.
  CHECK(parseNro(file.substr(0, 0x100), en, info) && info.name.empty() && info.icon.empty());
  CHECK(parseNro(file.substr(0, file.size() - 1), en, info) && info.name.empty() && info.version.empty());
  CHECK(parseNro(file.substr(0, 0x120), en, info) && info.name.empty());
  CHECK(parseNro(nro(nacp({{0, "X"}}, "1"), Jpeg, "NRO0", "ASEX"), en, info) && info.name.empty());
  {
    auto huge = file;
    put64(huge, 0x100 + 0x18, ~uint64_t(0) - 4); // NACP offset that wraps around
    CHECK(parseNro(huge, en, info) && info.name.empty() && info.icon == Jpeg);
    auto big = file;
    put64(big, 0x100 + 0x20, 0x8000); // NACP larger than any real one
    CHECK(parseNro(big, en, info) && info.name.empty());
    auto start = file;
    put32(start, 0x18, 0x10); // asset section inside the header
    CHECK(parseNro(start, en, info) && info.name.empty());
  }
  // An icon that is neither JPEG nor PNG is left out, with why; the rest
  // still reads. A PNG one is kept.
  CHECK(parseNro(nro(nacp({{0, "X"}}, "1"), "GIF8"), en, info) && info.name == "X" && info.icon.empty() &&
        info.iconProblem == "icon is no JPEG or PNG");
  const std::string png = "\x89PNG\r\n";
  CHECK(parseNro(nro(nacp({{0, "X"}}, "1"), png), en, info) && info.icon == png && info.iconProblem.empty());
  CHECK(parseNro(nro(nacp({{0, "X"}}, "1"), Jpeg, "NRO0", "ASEX"), en, info) && info.iconProblem == "no asset section");
  // Scan path rules: switch/x.nro and switch/<folder>/x.nro only.
  CHECK(cardNroPath("switch/a.nro"));
  CHECK(cardNroPath("switch/app/app.nro"));
  CHECK(cardNroPath("switch/app/APP.NRO"));
  CHECK(!cardNroPath("switch/a/b/c.nro"));
  CHECK(!cardNroPath("hbmenu.nro"));
  CHECK(!cardNroPath("switch/hbmenu.nro"));
  CHECK(!cardNroPath("a.nro"));
  CHECK(!cardNroPath("atmosphere/a.nro"));
  CHECK(!cardNroPath("switch/cookingfoil/cookingfoil.nro"));
  CHECK(!cardNroPath("switch/CookingFoil/cookingfoil.nro"));
  CHECK(cardNroPath("switch/cookingfoil.nro"));
  CHECK(!cardNroPath("switch/app/readme.txt"));
  CHECK(!cardNroPath("switch/.nro"));
  CHECK(!cardNroPath("switch/../a.nro"));
  CHECK(!cardNroPath("switch//a.nro"));
  CHECK(!cardNroPath("switch/a\\b.nro"));
  CHECK(!cardNroPath("/switch/a.nro"));
  CHECK(cardNroPaths({"switch/b/b.nro", "switch/a.nro", "switch/b/b.nro", "switch/x/y/z.nro"}) ==
        (std::vector<std::string>{"switch/a.nro", "switch/b/b.nro"}));
  {
    std::vector<std::string> many;
    for (size_t i = 0; i < MaxCardNros + 10; ++i) many.push_back("switch/" + std::to_string(i) + ".nro");
    CHECK(cardNroPaths(many).size() == MaxCardNros);
  }
  // Matching a card file to the list: install path, folder, file name.
  std::vector<App> apps(3);
  apps[0].id = "a", apps[0].app = "ytb", apps[0].fileName = "ytb-player.nro", apps[0].installPath = "switch/ytb/ytb-player.nro";
  apps[1].id = "b", apps[1].app = "sm64", apps[1].fileName = "sm64.nro", apps[1].installPath = "switch/sm64/sm64.nro";
  apps[2].id = "c", apps[2].app = "tool", apps[2].fileName = "tool.nro", apps[2].installPath = "switch/tool/tool.nro";
  CHECK(matchCard(apps, "switch/ytb/ytb-player.nro") == &apps[0]);
  CHECK(matchCard(apps, "switch/YTB/ytb-player.nro") == &apps[0]);
  CHECK(matchCard(apps, "switch/sm64/other-name.nro") == &apps[1]);
  CHECK(matchCard(apps, "switch/tool.nro") == &apps[2]);
  CHECK(matchCard(apps, "switch/elsewhere/tool.nro") == nullptr);
  CHECK(matchCard(apps, "switch/unknown/unknown.nro") == nullptr);
  CHECK(matchCard(apps, "switch/unknown.nro") == nullptr);
  CHECK(matchCard({}, "switch/ytb/ytb-player.nro") == nullptr);
  // No ledger entry, a file on the card: the versions decide.
  CHECK(cardStatus("1.2.0", "1.2.0") == Status::Installed);
  CHECK(cardStatus("v1.2.0", "1.2.0") == Status::Installed);
  CHECK(cardStatus("1.2.0", "V1.2.0 ") == Status::Installed);
  CHECK(cardStatus("1.3.0", "1.2.0") == Status::Update);
  CHECK(cardStatus("", "1.2.0") == Status::Installed);
  CHECK(cardStatus("1.2.0", "") == Status::Installed);
  {
    App app = apps[0];
    app.version = "2.0";
    app.hash = "h";
    const auto there = [](const std::string &) { return true; };
    const auto gone = [](const std::string &) { return false; };
    const std::string same = "v2.0", older = "1.0", unknown;
    CHECK(status(app, {}, there) == Status::Available);
    CHECK(status(app, {}, there, &same) == Status::Installed);
    CHECK(status(app, {}, there, &older) == Status::Update);
    CHECK(status(app, {}, there, &unknown) == Status::Installed);
    // A ledger entry on the card still decides as before.
    Ledger ledger{{"a", {"h", "2.0", app.installPath, {}}}};
    CHECK(status(app, ledger, there, &older) == Status::Installed);
    CHECK(status(app, ledger, gone, &older) == Status::Update);
  }
  {
    // The tile's picture: an app on the card shows its file's icon, never
    // the server's (none in the file: the name's initial); one not on the
    // card the server's.
    App listed;
    listed.iconUrl = "https://shop.example/api/homebrew/gh-1/icon";
    CHECK(tileIcon(listed, true, "card:switch/a.nro") == "card:switch/a.nro");
    CHECK(tileIcon(listed, true, "").empty());
    CHECK(tileIcon(listed, false, "") == listed.iconUrl);
    App local = listed;
    local.iconUrl.clear();
    local.cardOnly = true;
    CHECK(tileIcon(local, true, "card:switch/a.nro") == "card:switch/a.nro");
    CHECK(tileIcon(local, true, "").empty());
  }
  // The scan on a real folder: depth, exclusions, files that are no .nro.
  namespace fs = std::filesystem;
  const auto sd = fs::temp_directory_path() / ("cf-card-" + std::to_string(getpid()));
  fs::remove_all(sd);
  write(sd / "switch/root.nro", nro(nacp({{0, "Root"}}, "0.1")));
  write(sd / "switch/ytb/ytb-player.nro", nro(nacp({{0, "YTB"}}, "v1.1.0")));
  write(sd / "switch/ytb/config.ini", "x");
  write(sd / "switch/ytb/deep/deep.nro", nro(nacp({{0, "Deep"}}, "1")));
  write(sd / "switch/cookingfoil/cookingfoil.nro", nro(nacp({{0, "Self"}}, "1")));
  write(sd / "hbmenu.nro", nro(nacp({{0, "Menu"}}, "1")));
  write(sd / "switch/fake.nro", "not an nro at all");
  // What a Mac leaves beside each file it touched is no app, even one that
  // reads as an nro.
  write(sd / "switch/._root.nro", nro(nacp({{0, "Mac"}}, "1")));
  write(sd / "switch/ytb/._ytb-player.nro", nro(nacp({{0, "Mac"}}, "1")));
  write(sd / "switch/.DS_Store", "x");
  const auto card = scanCard(sd.string(), en);
  CHECK(card.size() == 2);
  if (card.size() == 2) {
    CHECK(card[0].path == "switch/root.nro" && card[0].name == "Root" && card[0].version == "0.1");
    CHECK(card[1].path == "switch/ytb/ytb-player.nro" && card[1].name == "YTB" && card[1].icon == Jpeg);
    CHECK(card[1].size == fs::file_size(sd / "switch/ytb/ytb-player.nro"));
  }
  CHECK(scanCard((sd / "missing").string(), en).empty());
  if (card.size() == 2) {
    // A file read before with the same size and time is taken from the
    // earlier scan; one that changed is read again.
    auto known = card;
    known[0].name = "Remembered";
    auto again = scanCard(sd.string(), en, known);
    CHECK(again.size() == 2 && again[0].name == "Remembered" && again[1].icon == Jpeg);
    known[0].mtime -= 1;
    again = scanCard(sd.string(), en, known);
    CHECK(again.size() == 2 && again[0].name == "Root");
  }
  // Removing a card-only app: its file and the folders it leaves empty, up
  // to switch/; nothing else.
  write(sd / "switch/solo/solo.nro", "x");
  std::string error;
  CHECK(removeCardFile(sd.string(), "switch/solo/solo.nro", error));
  CHECK(!fs::exists(sd / "switch/solo") && fs::exists(sd / "switch"));
  CHECK(removeCardFile(sd.string(), "switch/ytb/ytb-player.nro", error));
  CHECK(!fs::exists(sd / "switch/ytb/ytb-player.nro") && fs::exists(sd / "switch/ytb/config.ini"));
  CHECK(fs::exists(sd / "switch/ytb/deep/deep.nro"));
  CHECK(removeCardFile(sd.string(), "switch/root.nro", error));
  CHECK(!fs::exists(sd / "switch/root.nro") && fs::exists(sd / "switch/fake.nro"));
  // Paths the scan never lists are refused untouched.
  CHECK(!removeCardFile(sd.string(), "switch/cookingfoil/cookingfoil.nro", error));
  CHECK(fs::exists(sd / "switch/cookingfoil/cookingfoil.nro"));
  CHECK(!removeCardFile(sd.string(), "hbmenu.nro", error) && fs::exists(sd / "hbmenu.nro"));
  CHECK(!removeCardFile(sd.string(), "switch/ytb/deep/deep.nro", error) && fs::exists(sd / "switch/ytb/deep/deep.nro"));
  CHECK(!removeCardFile(sd.string(), "switch/../hbmenu.nro", error) && fs::exists(sd / "hbmenu.nro"));
  fs::remove_all(sd);
  return failures;
}
