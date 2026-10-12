#include "Test.hpp"
#include "core/Forwarder.hpp"
#include "core/HomebrewCard.hpp"
#include "core/State.hpp"
#include "core/Json.hpp"
#include "platform/Shortcut.hpp"
#include "ui/Application.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>
using namespace foil::forwarder;
namespace {
void put32(std::string &b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; ++i) b[at + i] = char(v >> (8 * i));
}
void put64(std::string &b, size_t at, uint64_t v) {
  for (int i = 0; i < 8; ++i) b[at + i] = char(v >> (8 * i));
}
uint64_t get(const std::string &b, size_t at, int bytes) {
  uint64_t v = 0;
  for (int i = bytes - 1; i >= 0; --i) v = v << 8 | uint8_t(b[at + i]);
  return v;
}
std::string hex(const Digest &d) {
  return std::string(reinterpret_cast<const char *>(d.data()), d.size());
}
const std::string Jpeg = std::string("\xFF\xD8\xFF\xE0", 4) + "jpeg body";
std::string testNacp(const std::string &name) {
  std::string b(NacpSize, '\0');
  for (int entry : {0, 12}) {
    b.replace(size_t(entry) * 0x300, name.size(), name);
    b.replace(size_t(entry) * 0x300 + 0x200, 6, "author");
  }
  b.replace(0x3060, 5, "1.2.3");
  put64(b, 0x3080, 0x100000); // a save data size the shortcut must drop
  return b;
}
// A small .nro: header, a little code, then the icon and NACP.
std::string nro(const std::string &control, const std::string &icon = Jpeg) {
  constexpr size_t Code = 0x100;
  std::string b(Code, '\0');
  b.replace(0x10, 4, "NRO0");
  put32(b, 0x18, Code);
  std::string asset(0x38, '\0');
  asset.replace(0, 4, "ASET");
  put64(asset, 0x08, 0x38);
  put64(asset, 0x10, icon.size());
  put64(asset, 0x18, 0x38 + icon.size());
  put64(asset, 0x20, control.size());
  return b + asset + icon + control;
}
// An NPDM shaped like hbl's: META, then ACI0 and ACID each with kernel
// capabilities (kernel flags and debug flags descriptors).
std::string testNpdm() {
  std::string b(0x400, '\0');
  b.replace(0, 4, "META");
  b[0xC] = char(1 << 1);
  put32(b, 0x70, 0x80);  // ACI0
  put32(b, 0x74, 0x48);
  put32(b, 0x78, 0x100); // ACID
  put32(b, 0x7C, 0x248);
  b.replace(0x80, 4, "ACI0");
  put32(b, 0x80 + 0x30, 0x40); // its kernel capabilities, from ACI0
  put32(b, 0x80 + 0x34, 8);
  b.replace(0x100 + 0x200, 4, "ACID");
  put32(b, 0x100 + 0x230, 0x240);
  put32(b, 0x100 + 0x234, 8);
  for (size_t kc : {size_t(0x80 + 0x40), size_t(0x100 + 0x240)}) {
    put32(b, kc, 0x020073B7);              // kernel flags: three cores
    put32(b, kc + 4, (1u << 18) | 0xFFFF); // debug flags: force_debug_prod
  }
  return b;
}
// A RomFS file the way hbl looks it up: the root's first file, then siblings.
bool romfsFile(const std::string &image, const std::string &name, std::string &out) {
  const uint64_t dirTable = get(image, 0x18, 8), fileTable = get(image, 0x38, 8), dataAt = get(image, 0x48, 8);
  uint32_t at = uint32_t(get(image, dirTable + 0xC, 4));
  while (at != 0xFFFFFFFF) {
    const size_t entry = fileTable + at;
    const auto length = get(image, entry + 0x1C, 4);
    if (image.compare(entry + 0x20, length, name) == 0 && length == name.size()) {
      out = image.substr(dataAt + get(image, entry + 8, 8), get(image, entry + 0x10, 8));
      return true;
    }
    at = uint32_t(get(image, entry + 4, 4));
  }
  return false;
}
// And through the file hash table, as the console's RomFS driver does.
bool romfsHashed(const std::string &image, const std::string &name) {
  uint32_t hash = 123456789u;
  for (const unsigned char c : name) hash = ((hash >> 5) | (hash << 27)) ^ c;
  const uint64_t hashAt = get(image, 0x28, 8), buckets = get(image, 0x30, 8) / 4, fileTable = get(image, 0x38, 8);
  uint32_t at = uint32_t(get(image, hashAt + (hash % buckets) * 4, 4));
  while (at != 0xFFFFFFFF) {
    const size_t entry = fileTable + at;
    if (image.compare(entry + 0x20, name.size(), name) == 0 && get(image, entry + 0x1C, 4) == name.size()) return true;
    at = uint32_t(get(image, entry + 0x18, 4));
  }
  return false;
}
// The PFS0 file `name`'s data.
bool pfs0File(const std::string &image, const std::string &name, std::string &out) {
  if (image.compare(0, 4, "PFS0") != 0) return false;
  const auto count = get(image, 4, 4), names = get(image, 8, 4);
  const size_t table = 0x10, strings = table + count * 0x18, data = strings + names;
  for (uint64_t i = 0; i < count; ++i) {
    const size_t e = table + i * 0x18;
    const auto n = image.c_str() + strings + get(image, e + 0x10, 4);
    if (name == n) {
      out = image.substr(data + get(image, e, 8), get(image, e + 8, 8));
      return true;
    }
  }
  return false;
}
struct Section {
  uint64_t start, end;
  std::string fsHeader;
};
Section section(const std::string &nca, int i) {
  return {get(nca, 0x240 + i * 0x10, 4) * 0x200, get(nca, 0x244 + i * 0x10, 4) * 0x200, nca.substr(0x400 + i * 0x200, 0x200)};
}
// Checks a PFS0 section's hashes and returns its image.
std::string pfs0Section(const std::string &nca, int i, int &failures) {
  const auto s = section(nca, i);
  CHECK(s.fsHeader[3] == 2 && s.fsHeader[2] == 1 && s.fsHeader[4] == 1); // sha256, PFS0, not encrypted
  CHECK(nca.substr(0x280 + i * 0x20, 0x20) == hex(sha256(s.fsHeader)));
  const auto tableSize = get(s.fsHeader, 0x8 + 0x30, 8), imageAt = get(s.fsHeader, 0x8 + 0x38, 8),
             imageSize = get(s.fsHeader, 0x8 + 0x40, 8), block = get(s.fsHeader, 0x8 + 0x20, 4);
  const auto table = nca.substr(s.start, tableSize);
  CHECK(s.fsHeader.substr(0x8, 0x20) == hex(sha256(table)));
  const auto image = nca.substr(s.start + imageAt, imageSize);
  CHECK(table.substr(0, 0x20) == hex(sha256(image.substr(0, block))));
  CHECK(s.start % 0x200 == 0 && s.end % 0x200 == 0 && s.start + imageAt + imageSize <= s.end);
  return image;
}
// Checks a RomFS section's IVFC levels and returns the RomFS.
std::string romfsSection(const std::string &nca, int i, int &failures) {
  const auto s = section(nca, i);
  CHECK(s.fsHeader[3] == 3 && s.fsHeader[2] == 0 && s.fsHeader[4] == 1); // IVFC, RomFS, not encrypted
  CHECK(nca.substr(0x280 + i * 0x20, 0x20) == hex(sha256(s.fsHeader)));
  CHECK(get(s.fsHeader, 0x8, 4) == 0x43465649 && get(s.fsHeader, 0x8 + 0xC, 4) == 7);
  std::string levels[6];
  uint64_t offsets[6];
  for (int l = 0; l < 6; ++l) {
    const size_t at = 0x8 + 0x10 + l * 0x18;
    offsets[l] = s.start + get(s.fsHeader, at, 8);
    levels[l] = nca.substr(offsets[l], get(s.fsHeader, at + 8, 8));
    CHECK(get(s.fsHeader, at + 0x10, 4) == 0x0E);
  }
  CHECK(s.fsHeader.substr(0x8 + 0xC0, 0x20) == hex(sha256(levels[0])));
  // Each level holds the hashes of the 0x4000 blocks of the one below (the
  // RomFS's last block hashed with its padding).
  for (int l = 0; l < 5; ++l) CHECK(levels[l].substr(0, 0x20) == hex(sha256(nca.substr(offsets[l + 1], 0x4000))));
  CHECK(offsets[5] + levels[5].size() <= s.end);
  return levels[5];
}
std::string tempDir(const std::string &name) {
  const auto dir = std::filesystem::temp_directory_path() / ("cf-" + name + "-" + std::to_string(getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir.string();
}
void write(const std::string &path, const std::string &bytes) {
  std::filesystem::create_directories(std::filesystem::path(path).parent_path());
  std::ofstream(path, std::ios::binary) << bytes;
}
} // namespace

int runForwarderTests() {
  int failures = 0;
  {
    // The id is sphaira's: the same path gives the same id (made again, it
    // replaces), its range is 0x05.. with the low 12 bits free. Values
    // computed independently with Python's hashlib.
    CHECK(titleId("/switch/foo/foo.nro") == 0x0595CE38EDAC1000ULL);
    CHECK(titleId("/switch/foo/foo.nro") == titleId(loaderPath("switch/foo/foo.nro")));
    CHECK(titleId("/switch/foo/foo.nro") == titleId(loaderPath("sdmc:/switch/foo/foo.nro")));
    CHECK(titleId("/switch/foo/foo.nro") != titleId("/switch/bar/bar.nro"));
    CHECK(titleId("/retroarch/cores/snes9x_libretro_libnx.nro", "\"sdmc:/roms/snes/My Game.sfc\"") == 0x05D81B2D99BCF000ULL);
    const auto legacy = titleId("/switch/foo/foo.nro", "", LegacyTitleIdPrefix);
    CHECK(legacy >> 56 == 0x01 && (legacy & 0x00FFFFFFFFFFFFFFULL) == (0x0595CE38EDAC1000ULL & 0x00FFFFFFFFFFFFFFULL));
    CHECK(hexId(0x0595CE38EDAC1000ULL) == "0595CE38EDAC1000");
    CHECK(parseHexId("0595ce38edac1000") == 0x0595CE38EDAC1000ULL && parseHexId("") == 0 && parseHexId("0595CE38EDAC10") == 0);
    CHECK(launchArgs("/switch/a.nro") == "/switch/a.nro");
    CHECK(launchArgs("/switch/my app.nro", "x") == "\"/switch/my app.nro\" x");
  }
  {
    // The .nro parser the card scan uses hands over the whole NACP too.
    foil::homebrew::NroInfo info;
    CHECK(foil::homebrew::parseNro(nro(testNacp("Demo")), {0}, info));
    CHECK(info.name == "Demo" && info.icon == Jpeg && info.nacp.size() == NacpSize);
    CHECK(foil::homebrew::parseNro(nro(""), {0}, info) && info.nacp.empty());
  }
  {
    // The NACP made fit for a shortcut: ids moved, no save data, names kept
    // unless given.
    auto nacp = testNacp("Demo");
    CHECK(patchNacp(nacp, 0x0500000000001000ULL));
    CHECK(nacp.compare(0, 4, "Demo") == 0 && nacp.compare(12 * 0x300, 4, "Demo") == 0);
    CHECK(get(nacp, 0x3038, 8) == 0x0500000000001000ULL && get(nacp, 0x3078, 8) == 0x0500000000001000ULL);
    CHECK(get(nacp, 0x3070, 8) == (0x0500000000001000ULL ^ 0x1000) && get(nacp, 0x30F8, 8) == 0x0500000000001000ULL);
    CHECK(get(nacp, 0x3080, 8) == 0 && nacp[0x3025] == 0 && nacp[0x3035] == 2);
    CHECK(nacp.compare(0x3060, 5, "1.2.3") == 0);
    CHECK(patchNacp(nacp, 1, "Game", "RetroArch"));
    CHECK(nacp.compare(5 * 0x300, 5, std::string("Game\0", 5)) == 0 && nacp.compare(5 * 0x300 + 0x200, 9, "RetroArch") == 0);
    std::string tooShort(10, '\0');
    CHECK(!patchNacp(tooShort, 1));
    const auto blank = blankNacp("Name", "Me", "2.0");
    CHECK(blank.size() == NacpSize && blank.compare(15 * 0x300, 4, "Name") == 0 && blank.compare(0x3060, 3, "2.0") == 0);
  }
  {
    // hbl's NPDM runs as the shortcut's id; the debug flag follows sphaira's
    // automatic mode.
    auto npdm = testNpdm();
    CHECK(patchNpdm(npdm, 0x0512345678901000ULL, true));
    CHECK(get(npdm, 0x80 + 0x10, 8) == 0x0512345678901000ULL);
    CHECK(get(npdm, 0x100 + 0x210, 8) == 0x0512345678901000ULL && get(npdm, 0x100 + 0x218, 8) == 0x0512345678901000ULL);
    CHECK(npdm.compare(0x20, 12, std::string("Application\0", 12)) == 0);
    CHECK(get(npdm, 0x80 + 0x40, 4) == 0x020073B7 && get(npdm, 0x80 + 0x44, 4) == ((1u << 19) | 0xFFFF));
    auto keep = testNpdm();
    CHECK(patchNpdm(keep, 1, false) && get(keep, 0x100 + 0x244, 4) == ((1u << 18) | 0xFFFF));
    // The real one, once the console build has made it from hbl.json.
    std::ifstream built("romfs/forwarder/main.npdm", std::ios::binary);
    if (built) {
      std::string real((std::istreambuf_iterator<char>(built)), {});
      CHECK(patchNpdm(real, 0x0512345678901000ULL, true));
      CHECK(get(real, get(real, 0x70, 4) + 0x10, 8) == 0x0512345678901000ULL);
    }
    auto broken = testNpdm();
    broken.replace(0x300, 4, "XXXX");
    CHECK(!patchNpdm(broken, 1, false));
  }
  {
    // A RomFS hbl and the console can both read; padded for the IVFC.
    uint64_t size = 0;
    const auto image = romfs({{"/nextArgv", "/switch/a.nro"}, {"/nextNroPath", "/switch/a.nro"}, {"/third", "xyz"}}, &size);
    CHECK(image.size() % 0x4000 == 0 && size < image.size() && get(image, 0, 8) == 0x50 && get(image, 0x48, 8) == 0x200);
    std::string data;
    CHECK(romfsFile(image, "nextNroPath", data) && data == "/switch/a.nro");
    CHECK(romfsFile(image, "third", data) && data == "xyz");
    CHECK(!romfsFile(image, "missing", data));
    CHECK(romfsHashed(image, "nextArgv") && romfsHashed(image, "nextNroPath") && romfsHashed(image, "third"));
    const auto pfs = pfs0({{"main", "code"}, {"main.npdm", "meta"}});
    CHECK(get(pfs, 8, 4) % 0x20 == 0);
    CHECK(pfs0File(pfs, "main.npdm", data) && data == "meta");
  }
  {
    // The whole shortcut, built without the console's key: the layout,
    // sizes and hashes of everything that is not encrypted.
    Input in;
    in.nroPath = "/switch/foo/foo.nro";
    in.nacp = testNacp("Foo");
    in.icon = Jpeg;
    in.hblMain = std::string(0x2345, 'M');
    in.hblNpdm = testNpdm();
    Package pkg;
    std::string error;
    CHECK(build(in, nullptr, pkg, error));
    CHECK(pkg.titleId == 0x0595CE38EDAC1000ULL && pkg.ncas.size() == 3);
    if (pkg.ncas.size() == 3) {
      const auto &program = pkg.ncas[0].bytes, &control = pkg.ncas[1].bytes, &meta = pkg.ncas[2].bytes;
      for (const auto &nca : pkg.ncas) {
        CHECK(nca.bytes.compare(0x200, 4, "NCA3") == 0 && get(nca.bytes, 0x208, 8) == nca.bytes.size());
        CHECK(get(nca.bytes, 0x210, 8) == pkg.titleId && nca.bytes.size() % 0x200 == 0);
        CHECK(hex(nca.hash) == hex(sha256(nca.bytes)));
      }
      CHECK(program[0x205] == 0 && control[0x205] == 2 && meta[0x205] == 1);
      CHECK(pkg.ncas[0].type == ContentType::Program && pkg.ncas[1].type == ContentType::Control &&
            pkg.ncas[2].type == ContentType::Meta);
      // Program: hbl in the ExeFS, the path in the RomFS.
      const auto exefs = pfs0Section(program, 0, failures);
      std::string data;
      CHECK(pfs0File(exefs, "main", data) && data == in.hblMain);
      CHECK(pfs0File(exefs, "main.npdm", data) && get(data, 0x80 + 0x10, 8) == pkg.titleId);
      CHECK(section(program, 1).start == section(program, 0).end);
      const auto args = romfsSection(program, 1, failures);
      CHECK(romfsFile(args, "nextNroPath", data) && data == "/switch/foo/foo.nro");
      CHECK(romfsFile(args, "nextArgv", data) && data == "/switch/foo/foo.nro");
      // Control: the NACP (patched) and the icon.
      const auto controlFs = romfsSection(control, 0, failures);
      CHECK(romfsFile(controlFs, "control.nacp", data) && data.size() == NacpSize && get(data, 0x3078, 8) == pkg.titleId);
      CHECK(romfsFile(controlFs, "icon_AmericanEnglish.dat", data) && data == Jpeg);
      // Meta: the CNMT lists program and control by hash and size.
      const auto metaFs = pfs0Section(meta, 0, failures);
      CHECK(pfs0File(metaFs, "Application_0595CE38EDAC1000.cnmt", data));
      CHECK(get(data, 0, 8) == pkg.titleId && uint8_t(data[0xC]) == 0x80 && get(data, 0x10, 2) == 2);
      CHECK(get(data, 0x20, 8) == (pkg.titleId | 0x800));
      CHECK(data.substr(0x30, 0x20) == hex(pkg.ncas[0].hash) && get(data, 0x30 + 0x30, 5) == program.size());
      CHECK(data.substr(0x68, 0x20) == hex(pkg.ncas[1].hash) && uint8_t(data[0x68 + 0x36]) == 3);
      // NCM: the key and the content meta data, the meta itself first.
      CHECK(pkg.metaKey.size() == 0x10 && get(pkg.metaKey, 0, 8) == pkg.titleId && uint8_t(pkg.metaKey[0xC]) == 0x80);
      CHECK(pkg.metaData.size() == 0x18 + 3 * 0x18 && get(pkg.metaData, 2, 2) == 3);
      CHECK(pkg.metaData.substr(0x18, 0x10) == hex(pkg.ncas[2].hash).substr(0, 0x10) && get(pkg.metaData, 0x28, 5) == meta.size());
      CHECK(pkg.metaData.substr(0x30, 0x10) == hex(pkg.ncas[0].hash).substr(0, 0x10) && uint8_t(pkg.metaData[0x30 + 0x16]) == 1);
      CHECK(pkg.metaData.substr(0x48, 0x10) == hex(pkg.ncas[1].hash).substr(0, 0x10) && uint8_t(pkg.metaData[0x48 + 0x16]) == 3);
    }
    // The header cipher touches the header only, and the ids follow it.
    Package sealed;
    CHECK(build(in, [](uint8_t *header) {
      for (size_t i = 0; i < NcaHeaderSize; ++i) header[i] ^= 0x5A;
    }, sealed, error));
    if (sealed.ncas.size() == 3 && pkg.ncas.size() == 3) {
      CHECK(sealed.ncas[0].bytes.compare(0x200, 4, "NCA3") != 0);
      CHECK(sealed.ncas[0].bytes.substr(NcaHeaderSize) == pkg.ncas[0].bytes.substr(NcaHeaderSize));
      CHECK(hex(sealed.ncas[0].hash) != hex(pkg.ncas[0].hash));
    }
    // What it cannot do without.
    auto noIcon = in;
    noIcon.icon.clear();
    CHECK(!build(noIcon, nullptr, pkg, error) && error == "icon");
    auto noLoader = in;
    noLoader.hblMain.clear();
    CHECK(!build(noLoader, nullptr, pkg, error) && error == "loader");
  }
  {
    // Signature patches, guessed from the card; the offer to make this
    // app's own shortcut.
    const auto sd = tempDir("sig");
    CHECK(!signaturePatchesFound(sd));
    std::filesystem::create_directories(sd + "/atmosphere/kip_patches/fs_patches");
    CHECK(!signaturePatchesFound(sd)); // an empty folder is no patch
    write(sd + "/atmosphere/kip_patches/fs_patches/x.ips", "PATCH");
    CHECK(signaturePatchesFound(sd));
    std::filesystem::remove_all(sd);
    std::filesystem::create_directories(sd + "/atmosphere/contents/420000000000000B");
    CHECK(signaturePatchesFound(sd));
    std::filesystem::remove_all(sd);
    const std::string self = "sdmc:/switch/cookingfoil/cookingfoil.nro";
    CHECK(offerSelfShortcut(false, false, self, false));
    CHECK(!offerSelfShortcut(true, false, self, false));  // started from a shortcut already
    CHECK(!offerSelfShortcut(false, true, self, false));  // asked before
    CHECK(!offerSelfShortcut(false, false, self, true));  // it has one
    CHECK(!offerSelfShortcut(false, false, "", false));   // no path to point at
  }
  {
    // The answer is kept with the other settings.
    const auto dir = tempDir("state");
    foil::State state;
    state.deviceKey = std::string(64, 'a');
    state.selfShortcutAsked = true;
    foil::Error error;
    CHECK(foil::saveState(dir + "/state.json", state, error));
    CHECK(foil::loadState(dir + "/state.json").value.selfShortcutAsked);
    std::filesystem::remove_all(dir);
  }
  return failures;
}

int runShortcutUiTests(SDL_Renderer *renderer) {
  int failures = 0;
  using foil::ui::Action;
  const auto sd = tempDir("shortcut-ui");
  write(sd + "/switch/cookingfoil/cookingfoil.nro", nro(testNacp("CookingFoil")));
  write(sd + "/switch/demo/demo.nro", nro(testNacp("Demo")));
  write(sd + "/bootloader/patches.ini", "[patches]"); // so it says made, not warns
  const auto settle = [&](foil::ui::Application &app) {
    app.render();
    SDL_Delay(750); // the double-press guard
  };
  {
    // Opened from the album with no shortcut: offered once; A makes it.
    foil::ui::Application app(renderer, "", "", "");
    CHECK(app.open());
    app.usePatchFolder(sd);
    app.setSelfPath("sdmc:/switch/cookingfoil/cookingfoil.nro");
    CHECK(app.askingSelfShortcut());
    settle(app);
    app.action(Action::Accept);
    CHECK(!app.askingSelfShortcut());
    CHECK(app.hasShortcut(titleId("/switch/cookingfoil/cookingfoil.nro")));
    CHECK(app.alert().empty());
    // Asked once: not again, and not when a shortcut is there.
    app.setSelfPath("sdmc:/switch/cookingfoil/cookingfoil.nro");
    CHECK(!app.askingSelfShortcut());
  }
  {
    // A new run sees the shortcut made above: nothing to offer. Without it,
    // B closes the offer for good.
    foil::ui::Application app(renderer, "", "", "");
    CHECK(app.open());
    app.usePatchFolder(sd);
    app.setSelfPath("sdmc:/switch/cookingfoil/cookingfoil.nro");
    CHECK(!app.askingSelfShortcut());
    foil::platform::removeShortcut(titleId("/switch/cookingfoil/cookingfoil.nro"));
    foil::ui::Application again(renderer, "", "", "");
    CHECK(again.open());
    again.usePatchFolder(sd);
    again.setSelfPath("sdmc:/switch/cookingfoil/cookingfoil.nro");
    CHECK(again.askingSelfShortcut());
    again.render();
    again.action(Action::Back);
    CHECK(!again.askingSelfShortcut() && !again.hasShortcut(titleId("/switch/cookingfoil/cookingfoil.nro")));
    again.setSelfPath("sdmc:/switch/cookingfoil/cookingfoil.nro");
    CHECK(!again.askingSelfShortcut());
    // Not from the SD card (the PC preview): nothing to point a shortcut at.
    foil::ui::Application preview(renderer, "", "", "");
    CHECK(preview.open());
    preview.setSelfPath("");
    CHECK(!preview.askingSelfShortcut());
  }
  {
    // 홈브류 > 설치됨: an installed app's card lists its rows, the shortcut
    // first here; A on it makes the shortcut, A again asks before deleting it.
    foil::ui::Application app(renderer, "", "", "");
    CHECK(app.open());
    app.usePatchFolder(sd);
    app.previewHomebrew({});
    app.screen(foil::ui::Screen::Homebrew);
    for (int i = 0; i < 300 && !app.homebrewCardScanned(); ++i) SDL_Delay(10);
    CHECK(app.homebrewCardScanned());
    app.render();
    if (app.onMenu()) app.action(Action::Right);
    app.action(Action::Accept);
    CHECK(app.appPopupOpen());
    const auto demo = titleId("/switch/demo/demo.nro");
    CHECK(!app.hasShortcut(demo));
    CHECK(app.appPopupRow() == 0); // an installed app starts on its first row
    settle(app);
    app.action(Action::Accept);
    CHECK(app.hasShortcut(demo) && app.appPopupOpen());
    app.action(Action::Accept);
    CHECK(app.askingShortcutRemoval() && !app.appPopupOpen());
    settle(app);
    app.action(Action::Back); // B keeps it and goes back to the card
    CHECK(!app.askingShortcutRemoval() && app.appPopupOpen() && app.hasShortcut(demo));
    settle(app);
    app.action(Action::Accept);
    settle(app);
    app.action(Action::Accept);
    CHECK(!app.hasShortcut(demo) && app.appPopupOpen());
    // The last row deletes the app, and its shortcut goes with it.
    app.action(Action::Accept);
    CHECK(app.hasShortcut(demo));
    app.action(Action::Down);
    CHECK(app.appPopupRow() == 1);
    settle(app);
    app.action(Action::Accept);
    CHECK(!app.appPopupOpen() && !app.askingShortcutRemoval());
    settle(app);
    app.action(Action::Accept);
    CHECK(!app.hasShortcut(demo) && !std::filesystem::exists(sd + "/switch/demo/demo.nro"));
  }
  std::filesystem::remove_all(sd);
  return failures;
}
