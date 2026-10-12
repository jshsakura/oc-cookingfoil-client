#include "Test.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include "net/Api.hpp"
#include <cstring>
#include <filesystem>
#include <unistd.h>
namespace fs = std::filesystem;
namespace {
constexpr const char *Tid = "0100F2C0115B6000";
const std::string Cheat = std::string("atmosphere/contents/") + Tid + "/cheats/ABCD.txt";
const std::string Romfs = std::string("atmosphere/contents/") + Tid + "/romfs/Data/text.bin";
const std::string Other = std::string("atmosphere/contents/") + Tid + "/romfs/Data/font.bin";

struct Entry {
  std::string path, body;
  char type = '0';
};
void octal(char *field, size_t size, uint64_t value) {
  char digits[32];
  std::snprintf(digits, sizeof digits, "%0*llo", int(size - 1), static_cast<unsigned long long>(value));
  std::memcpy(field, digits, size - 1);
}
std::string block(const std::string &path, uint64_t size, char type) {
  std::string b(512, '\0');
  auto name = path, prefix = std::string();
  if (name.size() > 100) {
    const auto slash = name.rfind('/', 154);
    prefix = name.substr(0, slash);
    name = name.substr(slash + 1);
  }
  std::memcpy(&b[0], name.data(), name.size());
  std::memcpy(&b[345], prefix.data(), prefix.size());
  octal(&b[100], 8, 0644);
  octal(&b[124], 12, size);
  b[156] = type;
  std::memcpy(&b[257], "ustar\0" "00", 8);
  std::memset(&b[148], ' ', 8);
  unsigned sum = 0;
  for (unsigned char c : b)
    sum += c;
  std::snprintf(&b[148], 8, "%06o", sum);
  return b;
}
std::string tar(const std::vector<Entry> &entries, bool end = true) {
  std::string out;
  for (const auto &e : entries) {
    out += block(e.path, e.body.size(), e.type);
    out += e.body;
    out.append((512 - e.body.size() % 512) % 512, '\0');
  }
  if (end)
    out.append(1024, '\0');
  return out;
}
std::string pax(const std::string &path) {
  auto record = " path=" + path + "\n";
  auto length = record.size() + 2;
  if (std::to_string(length).size() > 2)
    ++length;
  return std::to_string(length) + record;
}
// Feeds in small uneven pieces so headers and bodies straddle chunks.
bool unpack(const std::string &root, const std::string &archive, std::string *error = nullptr) {
  foil::TarWriter writer(root, Tid);
  for (size_t at = 0; at < archive.size(); at += 37)
    if (!writer.feed(archive.data() + at, std::min<size_t>(37, archive.size() - at)))
      break;
  const bool ok = writer.finish();
  if (error)
    *error = writer.error();
  return ok;
}
void write(const std::string &path, const std::string &body) {
  fs::create_directories(fs::path(path).parent_path());
  std::string ignored;
  foil::atomicWrite(path, body, ignored);
}
foil::UserPatch patch(const std::string &id, const std::string &version = "1") {
  foil::UserPatch p;
  p.id = id;
  p.titleId = Tid;
  p.name = id;
  p.version = version;
  return p;
}
bool stage(foil::PatchStore &store, const std::string &id, const std::vector<Entry> &entries,
           const std::string &version = "1") {
  foil::Error error;
  return unpack(store.staging(id), tar(entries)) && store.commit(patch(id, version), error);
}
} // namespace

int runPatchTests() {
  int failures = 0;
  CHECK(foil::patchPathAllowed(Tid, Romfs));
  CHECK(foil::patchPathAllowed(Tid, "atmosphere/contents/0100f2c0115b6000/exefs/main.npdm"));
  CHECK(foil::patchPathAllowed(Tid, "atmosphere/exefs_patches/res/ABCD.ips"));
  CHECK(foil::patchPathAllowed(Tid, std::string("SaltySD/plugins/FPSLocker/patches/") + Tid + "/ABCD.bin"));
  CHECK(!foil::patchPathAllowed(Tid, "atmosphere/contents/0100000000001000/romfs/a"));
  CHECK(!foil::patchPathAllowed(Tid, "atmosphere/contents/" + std::string(Tid) + "/../../hekate_ipl.ini"));
  CHECK(!foil::patchPathAllowed(Tid, "/atmosphere/contents/" + std::string(Tid) + "/romfs/a"));
  CHECK(!foil::patchPathAllowed(Tid, "bootloader/hekate_ipl.ini"));
  CHECK(!foil::patchPathAllowed(Tid, "atmosphere/config/system_settings.ini"));
  CHECK(!foil::patchPathAllowed(Tid, "atmosphere/exefs_patches/res"));
  CHECK(!foil::patchPathAllowed(Tid, "atmosphere//contents/x/y"));
  CHECK(foil::isCheatPath(Cheat));
  CHECK(!foil::isCheatPath(Romfs));

  const auto root = fs::temp_directory_path() / ("cf-patches-" + std::to_string(getpid()));
  fs::remove_all(root);
  const auto sd = (root / "sd").string(), storeRoot = (root / "sd/switch/cookingfoil/patches").string();
  {
    // Long paths use the ustar prefix; pax path records override the name.
    const std::string deep = std::string("atmosphere/contents/") + Tid + "/romfs/" +
                             std::string(90, 'd') + "/file.bin";
    const auto dir = (root / "tar").string();
    CHECK(unpack(dir, tar({{Romfs, "korean"}, {deep, "deep"}, {"", pax(Other), 'x'},
                           {"ignored-name", "font"}, {"atmosphere/", "", '5'}})));
    CHECK(foil::readFile(dir + "/" + Romfs) == "korean");
    CHECK(foil::readFile(dir + "/" + deep) == "deep");
    CHECK(foil::readFile(dir + "/" + Other) == "font");
    std::string error;
    const auto escape = tar({{"bootloader/hekate_ipl.ini", "x"}});
    CHECK(!unpack(dir, escape, &error) && !error.empty());
    CHECK(!fs::exists(dir + "/bootloader"));
    CHECK(!unpack(dir, tar({{Romfs, "", '2'}})));
    CHECK(!unpack(dir, tar({{Romfs, "cut"}}, false)));
    CHECK(!unpack(dir, "<html>not a tar</html>" + std::string(1024, ' ')));
  }
  {
    // The user's own cheats for this build survive every toggle.
    write(sd + "/" + Cheat, "[user]\n04000000 0 0");
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error));
    CHECK(stage(store, "korean", {{Romfs, "ko"}, {Cheat, "[A]\n1"}}));
    CHECK(stage(store, "fps", {{Other, "font"}, {Cheat, "[B]\n2"}}));
    CHECK(stage(store, "texture", {{Romfs, "hd"}}));
    CHECK(store.records().size() == 3 && !fs::exists(store.staging("korean")));

    CHECK(store.enable("korean", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "ko");
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[A]\n1");
    CHECK(store.enable("fps", error));
    CHECK(foil::readFile(sd + "/" + Other) == "font");
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[A]\n1\n\n[B]\n2");

    // Two patches that replace the same file cannot both be on.
    const auto clash = store.conflicts("texture");
    CHECK(clash.size() == 1 && clash[0].path == Romfs && clash[0].owner == "korean");
    CHECK(!store.enable("texture", error) && !error.message.empty());
    error = {};
    CHECK(foil::readFile(sd + "/" + Romfs) == "ko");

    CHECK(store.disable("korean", error));
    CHECK(!fs::exists(sd + "/" + Romfs) && fs::exists(storeRoot + "/korean/" + Romfs));
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[B]\n2");
    CHECK(store.conflicts("texture").empty() && store.enable("texture", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "hd");
    CHECK(store.disable("texture", error) && store.disable("fps", error));
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0");
    CHECK(!fs::exists(storeRoot + "/_base/" + Cheat));

    // A file the user put in place is never overwritten.
    write(sd + "/" + Romfs, "mine");
    const auto mine = store.conflicts("korean");
    CHECK(mine.size() == 1 && mine[0].owner.empty());
    CHECK(!store.enable("korean", error));
    error = {};
    fs::remove(sd + "/" + Romfs);

    // A new version replaces the stored files and stays on.
    CHECK(store.enable("korean", error));
    CHECK(stage(store, "korean", {{Romfs, "ko2"}, {Cheat, "[A2]\n1"}}, "2"));
    CHECK(store.find("korean")->on && store.find("korean")->version == "2");
    CHECK(foil::readFile(sd + "/" + Romfs) == "ko2");
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[A2]\n1");

    // Removing an enabled patch takes its files back first.
    CHECK(store.remove("korean", error));
    CHECK(!store.find("korean") && !fs::exists(storeRoot + "/korean") && !fs::exists(sd + "/" + Romfs));
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0");
  }
  {
    // Power loss mid-enable: the ledger says "enable", load finishes it.
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.records().size() == 2);
    const auto ledger = storeRoot + "/ledger.json";
    foil::Error parse;
    auto json = foil::parseJson(foil::readFile(ledger), parse);
    auto *fps = foil::field(foil::field(json.get(), "patches"), "fps");
    CHECK(fps != nullptr);
    json_object_object_add(fps, "pending", json_object_new_string("enable"));
    const std::string text = json_object_to_json_string(json.get());
    write(ledger, text);
    fs::create_directories(store.staging("cut"));
    foil::PatchStore reopened(sd, storeRoot);
    CHECK(reopened.load(error) && reopened.find("fps")->on && reopened.find("fps")->pending.empty());
    CHECK(foil::readFile(sd + "/" + Other) == "font");
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[B]\n2");
    CHECK(!fs::exists(reopened.staging("cut")));
    // A set-aside cheat file the ledger never recorded is put back.
    CHECK(reopened.disable("fps", error));
    write(storeRoot + "/_base/" + Cheat, "[lost]");
    fs::remove(sd + "/" + Cheat);
    foil::PatchStore again(sd, storeRoot);
    CHECK(again.load(error) && foil::readFile(sd + "/" + Cheat) == "[lost]");
  }
  fs::remove_all(root);
  // The game a patch is for comes with it when the shop does not list it.
  {
    const auto list = foil::net::parsePatches(std::string(R"({"patches":[
      {"id":"a-1","title_id":"01001730144DA000","name":"한글패치",
       "game":{"name":"Base","names":{"ko":"한국어 이름","ja":"日本語"},"icon":"/api/icon/01001730144DA000"}},
      {"id":"b-2","title_id":"01002A000CD48000","name":"한글패치","game":{"icon":"javascript:x"}},
      {"id":"c-3","title_id":"010043600B6A6000","name":"한글패치"}]})"));
    CHECK(list.size() == 3);
    if (list.size() == 3) {
      CHECK(list[0].gameName == "Base");
      CHECK(list[0].gameNames.size() == 4 && list[0].gameNames[0] == "한국어 이름" && list[0].gameNames[1].empty());
      CHECK(list[0].gameIcon == "/api/icon/01001730144DA000");
      CHECK(list[1].gameIcon.empty()); // neither http(s) nor a shop path
      CHECK(list[2].gameName.empty() && list[2].gameNames.empty());
    }
  }
  // An app's patch: no Title ID, its app's folder instead. Entries that name
  // neither, or an app folder that is not one safe name, are left out.
  {
    const auto list = foil::net::parsePatches(std::string(R"({"patches":[
      {"id":"hb-1","title_id":null,"homebrew":"sm64","kinds":["homebrew"],"name":"HD 텍스처","size":10},
      {"id":"hb-2","title_id":null,"homebrew":"../atmosphere","kinds":["homebrew"]},
      {"id":"hb-3","title_id":null,"homebrew":"a/b"},
      {"id":"hb-4","title_id":null},
      {"id":"g-1","title_id":"01001730144DA000","homebrew":"sm64","name":"게임 패치"}]})"));
    CHECK(list.size() == 2);
    if (list.size() == 2) {
      CHECK(list[0].id == "hb-1" && list[0].titleId.empty() && list[0].homebrewApp == "sm64");
      CHECK(list[0].kinds.size() == 1 && list[0].kinds[0] == "homebrew" && list[0].name == "HD 텍스처");
      // A game patch stays a game patch.
      CHECK(list[1].titleId == "01001730144DA000" && list[1].homebrewApp.empty());
    }
  }
  {
    foil::UserPatch app;
    app.id = "hb-1";
    app.homebrewApp = "sm64";
    CHECK(foil::patchPathAllowed(app, "switch/sm64/mods/hd.bin"));
    CHECK(!foil::patchPathAllowed(app, "switch/other/hd.bin"));
    CHECK(!foil::patchPathAllowed(app, "switch/sm64/../other/hd.bin"));
    CHECK(!foil::patchPathAllowed(app, Romfs));
    CHECK(!foil::patchPathAllowed(app, "atmosphere/exefs_patches/res/ABCD.ips"));
    const auto game = patch("g");
    CHECK(foil::patchPathAllowed(game, Romfs) && !foil::patchPathAllowed(game, "switch/sm64/a"));

    const auto hbRoot = fs::temp_directory_path() / ("cf-hbpatches-" + std::to_string(getpid()));
    fs::remove_all(hbRoot);
    const auto hbSd = (hbRoot / "sd").string(), hbStore = (hbRoot / "sd/switch/cookingfoil/patches").string();
    const std::string Mod = "switch/sm64/mods/hd.bin", Config = "switch/sm64/config.ini";
    // The tar may only write inside the app's folder.
    const auto unpackApp = [&](const std::string &dir, const std::string &archive) {
      foil::TarWriter writer(dir, [&](const std::string &path) { return foil::patchPathAllowed(app, path); });
      return writer.feed(archive.data(), archive.size()) && writer.finish();
    };
    CHECK(!unpackApp((hbRoot / "x").string(), tar({{"switch/other/a.bin", "x"}})));
    CHECK(!fs::exists(hbRoot / "x/switch/other"));
    CHECK(!unpackApp((hbRoot / "x").string(), tar({{Romfs, "x"}})));

    write(hbSd + "/switch/sm64/sm64.nro", "app");
    write(hbSd + "/" + Config, "mine");
    foil::PatchStore store(hbSd, hbStore);
    foil::Error error;
    CHECK(store.load(error));
    // A staging folder holding a file outside the folder is refused at commit.
    write(store.staging("bad") + "/switch/other/a.bin", "x");
    auto bad = app;
    bad.id = "bad";
    CHECK(!store.commit(bad, error) && !store.find("bad"));
    error = {};
    fs::remove_all(store.staging("bad"));

    const auto archive = tar({{Mod, "hd"}, {Config, "patched"}});
    CHECK(unpackApp(store.staging("hb-1"), archive) && store.commit(app, error));
    CHECK(store.find("hb-1") && store.find("hb-1")->homebrewApp == "sm64");
    // The app's own files are set aside, not a conflict, and come back on disable.
    CHECK(store.conflicts("hb-1").empty());
    CHECK(store.enable("hb-1", error));
    CHECK(foil::readFile(hbSd + "/" + Mod) == "hd" && foil::readFile(hbSd + "/" + Config) == "patched");
    foil::PatchStore reopened(hbSd, hbStore);
    CHECK(reopened.load(error) && reopened.find("hb-1")->on && reopened.find("hb-1")->homebrewApp == "sm64");
    CHECK(foil::readFile(hbSd + "/" + Config) == "patched"); // load leaves a set-aside file of an enabled patch
    CHECK(reopened.disable("hb-1", error));
    CHECK(!fs::exists(hbSd + "/" + Mod) && foil::readFile(hbSd + "/" + Config) == "mine");
    CHECK(fs::exists(hbSd + "/switch/sm64/sm64.nro") && !fs::exists(hbStore + "/_base/" + Config));
    // Power loss mid-disable: the patch file is back in the store, the
    // app's own file still set aside; load puts it back.
    CHECK(reopened.enable("hb-1", error));
    {
      const auto ledger = hbStore + "/ledger.json";
      foil::Error parse;
      auto json = foil::parseJson(foil::readFile(ledger), parse);
      json_object_object_add(foil::field(foil::field(json.get(), "patches"), "hb-1"), "pending",
                             json_object_new_string("disable"));
      write(ledger, json_object_to_json_string(json.get()));
      fs::rename(hbSd + "/" + Config, hbStore + "/hb-1/" + Config);
      foil::PatchStore again(hbSd, hbStore);
      CHECK(again.load(error) && !again.find("hb-1")->on);
      CHECK(foil::readFile(hbSd + "/" + Config) == "mine" && !fs::exists(hbSd + "/" + Mod));
    }
    fs::remove_all(hbRoot);
  }
  {
    // A skipped entry is read past: nothing written, the next one still lands.
    const auto dir = root / "skip";
    foil::TarWriter writer(dir.string(), [](const std::string &) { return true; });
    writer.skip([](const std::string &path) { return path == "keep/settings.cfg"; });
    const auto archive = tar({{"keep/settings.cfg", "server copy"}, {"keep/core.nro", "new core"}});
    for (size_t at = 0; at < archive.size(); at += 37)
      writer.feed(archive.data() + at, std::min<size_t>(37, archive.size() - at));
    CHECK(writer.finish());
    CHECK(!fs::exists(dir / "keep/settings.cfg"));
    CHECK(foil::readFile((dir / "keep/core.nro").string()) == "new core");
    fs::remove_all(root);
  }
  return failures;
}
