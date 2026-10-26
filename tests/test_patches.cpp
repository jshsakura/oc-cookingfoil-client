#include "Test.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include "net/Api.hpp"
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
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
// Moves one file as the store would, making its folder first: a patch moved
// whole leaves no folders behind in its store.
void moveFile(const std::string &from, const std::string &to) {
  fs::create_directories(fs::path(to).parent_path());
  fs::rename(from, to);
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

static int runPatchOwnerTests() {
  int failures = 0;
  // An update's or DLC's ID leads back to its game; case does not matter.
  CHECK(foil::baseTitleId("0100b00b51230000") == "0100B00B51230000");
  CHECK(foil::baseTitleId("0100B00B51230800") == "0100B00B51230000");
  CHECK(foil::baseTitleId("0100B00B51231001") == "0100B00B51230000");
  CHECK(foil::baseTitleId("01007EF00011E800") == "01007EF00011E000");
  CHECK(foil::baseTitleId("not-an-id") == "NOT-AN-ID");
  foil::UserPatch game, otherGame, app;
  game.id = "a";
  game.titleId = "0100b00b51230000";
  otherGame.id = "b";
  otherGame.titleId = "0100B00B51240000";
  app.id = "c";
  app.homebrewApp = "Tool";
  CHECK(foil::patchOwner(game) == "0100B00B51230000");
  CHECK(foil::patchOwner(app) == "app:Tool");
  CHECK(foil::patchBelongs(game, "0100B00B51230000"));
  CHECK(foil::patchBelongs(game, "0100b00b51230800")); // from the update's page
  CHECK(!foil::patchBelongs(otherGame, "0100B00B51230000"));
  CHECK(foil::patchBelongs(app, "app:Tool"));
  CHECK(!foil::patchBelongs(app, "app:tool2"));
  CHECK(!foil::patchBelongs(game, ""));
  const auto mine = foil::patchesFor({game, otherGame, app}, "0100B00B51230000");
  CHECK(mine.size() == 1 && mine.front().id == "a");
  // The version a patch is for, against the version being installed.
  CHECK(foil::normalizeVersion("  v1.2.0 ") == "1.2.0");
  CHECK(foil::normalizeVersion("V2") == "2");
  CHECK(foil::normalizeVersion("   ").empty());
  foil::UserPatch versioned;
  CHECK(foil::patchFitsVersion(versioned, "1.2.0")); // none given: any version
  versioned.gameVersion = "any";
  CHECK(foil::patchFitsVersion(versioned, "1.2.0"));
  versioned.gameVersion = "1.2.0";
  CHECK(foil::patchFitsVersion(versioned, "1.2.0"));
  CHECK(foil::patchFitsVersion(versioned, " v1.2.0"));
  versioned.gameVersion = "v1.2.0 ";
  CHECK(foil::patchFitsVersion(versioned, "1.2.0"));
  CHECK(!foil::patchFitsVersion(versioned, "1.3.0"));
  CHECK(foil::patchFitsVersion(versioned, "")); // version unknown: shown
  // What a picked patch still needs from the store.
  foil::PatchRecord record;
  game.hash = "h1";
  CHECK(foil::patchStep(game, nullptr) == foil::PatchStep::Get);
  record.hash = "h1";
  CHECK(foil::patchStep(game, &record) == foil::PatchStep::Enable);
  record.on = true;
  CHECK(foil::patchStep(game, &record) == foil::PatchStep::Skip);
  record.hash = "h0";
  CHECK(foil::patchStep(game, &record) == foil::PatchStep::Update);
  return failures;
}
// Three or more patches on one path, removal, power loss and an old ledger.
static int runPatchLayerTests() {
  int failures = 0;
  const auto root = fs::temp_directory_path() / ("cf-patchlayers-" + std::to_string(getpid()));
  fs::remove_all(root);
  const auto sd = (root / "sd").string(), storeRoot = (root / "sd/switch/cookingfoil/patches").string();
  const std::string Stack = std::string("atmosphere/contents/") + Tid + "/romfs/Data/stack.bin";
  const auto ledger = storeRoot + "/ledger.json";
  const auto onCard = [&] { return foil::readFile(sd + "/" + Stack); };
  const auto inStore = [&](const std::string &id) { return foil::readFile(storeRoot + "/" + id + "/" + Stack); };
  // Rewrites one patch's fields in the ledger, as a power loss would leave it.
  const auto edit = [&](const std::string &id, bool on, int64_t order, const char *pending) {
    foil::Error parse;
    auto json = foil::parseJson(foil::readFile(ledger), parse);
    auto *p = foil::field(foil::field(json.get(), "patches"), id.c_str());
    json_object_object_add(p, "on", json_object_new_boolean(on));
    json_object_object_add(p, "order", json_object_new_int64(order));
    json_object_object_add(p, "pending", json_object_new_string(pending));
    if (order) json_object_object_add(json.get(), "order", json_object_new_int64(order));
    write(ledger, json_object_to_json_string(json.get()));
  };
  {
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error));
    const auto stageOne = [&](const std::string &id, const std::string &body) {
      return stage(store, id, {{Stack, body}});
    };
    // A failed earlier commit can leave an unrecorded installation tree.
    // Replace it completely, including files absent from the new archive.
    write(storeRoot + "/s1/old/nested.bin", "stale");
    CHECK(stageOne("s1", "one") && stageOne("s2", "two") && stageOne("s3", "three"));
    CHECK(!fs::exists(storeRoot + "/s1/old"));
    // A deletion error must keep the ledger entry for a later retry.
    fs::rename(storeRoot, storeRoot + "-saved");
    write(storeRoot, "not a directory");
    CHECK(!store.remove("s1", error) && store.find("s1"));
    fs::remove(storeRoot);
    fs::rename(storeRoot + "-saved", storeRoot);
    error = {};
    CHECK(store.enable("s1", error) && store.enable("s2", error) && store.enable("s3", error));
    CHECK(onCard() == "three" && inStore("s1") == "one" && inStore("s2") == "two");
    const auto over = store.conflicts("s1");
    CHECK(over.size() == 2 && over[0].owner == "s2" && over[1].owner == "s3");
    CHECK(store.find("s1")->shadowed.at(Stack) == "s3" && store.find("s2")->shadowed.at(Stack) == "s3");
    // The middle one goes off: the top stays.
    CHECK(store.disable("s2", error) && onCard() == "three" && inStore("s2") == "two");
    CHECK(store.disable("s3", error) && onCard() == "one");
    // The latest one still on comes back, not the first.
    CHECK(store.enable("s2", error) && store.enable("s3", error) && store.disable("s3", error));
    CHECK(onCard() == "two" && inStore("s1") == "one" && inStore("s3") == "three");
    CHECK(store.disable("s2", error) && onCard() == "one");
    CHECK(store.disable("s1", error) && !fs::exists(sd + "/" + Stack));
    CHECK(inStore("s1") == "one" && inStore("s2") == "two" && inStore("s3") == "three");

    // Removing the patch on top brings back the one under it.
    CHECK(store.enable("s1", error) && store.enable("s2", error) && store.remove("s2", error));
    CHECK(onCard() == "one" && !fs::exists(storeRoot + "/s2") && !store.find("s2"));
    // Removing one underneath leaves the top in place.
    CHECK(store.enable("s3", error) && store.remove("s1", error));
    CHECK(onCard() == "three" && !fs::exists(storeRoot + "/s1") && store.find("s3")->shadowed.empty());
    CHECK(store.disable("s3", error) && !fs::exists(sd + "/" + Stack));
    CHECK(stageOne("s1", "one") && store.enable("s1", error));
  }
  {
    // Power loss in the middle of enabling s3 over s1: the ledger already
    // says on with its new place and pending, s1's copy is back in its store
    // and s3's not yet on the card. load finishes it.
    edit("s3", true, 50, "enable");
    moveFile(sd + "/" + Stack, storeRoot + "/s1/" + Stack);
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error));
    CHECK(onCard() == "three" && inStore("s1") == "one");
    CHECK(store.find("s3")->on && store.find("s3")->pending.empty() && store.find("s3")->order == 50);
    CHECK(store.find("s1")->shadowed.at(Stack) == "s3");
  }
  {
    // Power loss in the middle of turning s3 off: its copy is back in its
    // store, s1's not yet on the card.
    edit("s3", false, 50, "disable");
    moveFile(sd + "/" + Stack, storeRoot + "/s3/" + Stack);
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && !store.find("s3")->on && store.find("s3")->pending.empty());
    CHECK(onCard() == "one" && inStore("s3") == "three");
  }
  {
    // A ledger from before layering marked a patch going off as still on:
    // nothing moved yet. load finishes it the same way.
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.enable("s3", error) && onCard() == "three");
    edit("s3", true, 60, "disable");
    foil::PatchStore reopened(sd, storeRoot);
    CHECK(reopened.load(error) && !reopened.find("s3")->on && onCard() == "one" && inStore("s3") == "three");
    // And one going on as still off, its order not given yet.
    edit("s3", false, 0, "enable");
    foil::PatchStore again(sd, storeRoot);
    CHECK(again.load(error) && again.find("s3")->on && again.find("s3")->order > again.find("s1")->order);
    CHECK(onCard() == "three" && inStore("s1") == "one");
  }
  fs::remove_all(root);
  {
    // An old ledger, written before layering: no shadowed entries; an
    // enabled patch's file on the card, the user's own file of an app patch
    // set aside in _base.
    const std::string Old = std::string("atmosphere/contents/") + Tid + "/romfs/Data/old.bin";
    const std::string Config = "switch/sm64/config.ini";
    write(sd + "/" + Old, "old");
    write(sd + "/" + Config, "patched");
    write(storeRoot + "/_base/" + Config, "mine");
    write(storeRoot + "/old/.keep", "");
    write(storeRoot + "/hb/.keep", "");
    write(ledger, std::string(R"({"order":2,"cheatPaths":[],"patches":{
      "old":{"titleId":")") + Tid + R"(","name":"old","version":"1","hash":"","on":true,"pending":"",
             "files":[")" + Old + R"("],"cheats":[],"bytes":3,"order":1},
      "hb":{"titleId":"","homebrew":"sm64","name":"hb","version":"1","hash":"","on":true,"pending":"",
            "files":[")" + Config + R"("],"cheats":[],"bytes":7,"order":2}}})");
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.records().size() == 2);
    CHECK(store.find("old")->on && store.find("old")->shadowed.empty() && store.find("hb")->on);
    CHECK(foil::readFile(sd + "/" + Old) == "old" && foil::readFile(sd + "/" + Config) == "patched");
    CHECK(foil::readFile(storeRoot + "/_base/" + Config) == "mine");
    // A new patch layers over the old one.
    write(store.staging("new") + "/" + Old, "new");
    CHECK(store.commit(patch("new"), error) && store.enable("new", error));
    CHECK(foil::readFile(sd + "/" + Old) == "new" && foil::readFile(storeRoot + "/old/" + Old) == "old");
    CHECK(store.disable("new", error) && foil::readFile(sd + "/" + Old) == "old");
    CHECK(store.disable("old", error) && !fs::exists(sd + "/" + Old));
    CHECK(store.disable("hb", error) && foil::readFile(sd + "/" + Config) == "mine");
    CHECK(!fs::exists(storeRoot + "/_base/" + Config));
  }
  fs::remove_all(root);
  return failures;
}
// A folder no one else touches moves with one rename; anything shared or
// added by the user falls back to moving file by file.
static int runPatchWholeTests() {
  int failures = 0;
  const auto root = fs::temp_directory_path() / ("cf-patchwhole-" + std::to_string(getpid()));
  fs::remove_all(root);
  const auto sd = (root / "sd").string(), storeRoot = (root / "sd/switch/cookingfoil/patches").string();
  const auto ledger = storeRoot + "/ledger.json";
  const std::string Dir = std::string("atmosphere/contents/") + Tid + "/romfs";
  const std::string First = Dir + "/Data/f000.bin";
  // Directory identity on the host: a folder renamed whole keeps its inode,
  // one rebuilt file by file gets a new one.
  const auto inode = [](const std::string &path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 ? st.st_ino : ino_t(0);
  };
  std::vector<Entry> big;
  for (int n = 0; n < 300; ++n) {
    char name[64];
    std::snprintf(name, sizeof name, "/%s/f%03d.bin", n % 3 ? "Data" : "Data/Deep", n);
    big.push_back({Dir + (n == 0 ? "/Data/f000.bin" : name), "big" + std::to_string(n)});
  }
  std::vector<std::string> bigFiles;
  for (const auto &e : big) bigFiles.push_back(e.path);
  const auto allOnCard = [&] {
    for (size_t n = 0; n < big.size(); ++n)
      if (foil::readFile(sd + "/" + big[n].path) != big[n].body) return false;
    return true;
  };
  const auto allInStore = [&] {
    for (size_t n = 0; n < big.size(); ++n)
      if (foil::readFile(storeRoot + "/big/" + big[n].path) != big[n].body) return false;
    return true;
  };
  // Rewrites one patch's state in the ledger, as a power loss would leave it.
  const auto edit = [&](const std::string &id, bool on, const char *pending, bool withRoots) {
    foil::Error parse;
    auto json = foil::parseJson(foil::readFile(ledger), parse);
    auto *p = foil::field(foil::field(json.get(), "patches"), id.c_str());
    json_object_object_add(p, "on", json_object_new_boolean(on));
    json_object_object_add(p, "pending", json_object_new_string(pending));
    if (withRoots) {
      auto *roots = json_object_new_array();
      json_object_array_add(roots, json_object_new_string(Dir.c_str()));
      json_object_object_add(p, "roots", roots);
    }
    write(ledger, json_object_to_json_string(json.get()));
  };
  {
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error));
    CHECK(stage(store, "big", big));
    size_t done = 0, total = 0;
    store.progress([&](size_t d, size_t t) { done = d, total = t; });
    // Round trip: one rename each way.
    const auto folder = inode(storeRoot + "/big/" + Dir);
    CHECK(folder != 0 && store.enable("big", error));
    CHECK(inode(sd + "/" + Dir) == folder && !fs::exists(storeRoot + "/big/" + Dir) && allOnCard());
    CHECK(done == 300 && total == 300);
    CHECK(store.find("big")->pending.empty() && store.find("big")->roots.empty());
    CHECK(foil::field(foil::field(foil::field(foil::parseJson(foil::readFile(ledger), error).get(), "patches"),
                                  "big"), "roots") == nullptr);
    done = total = 0;
    CHECK(store.disable("big", error));
    CHECK(inode(storeRoot + "/big/" + Dir) == folder && allInStore() && done == 300 && total == 300);
    CHECK(!fs::exists(sd + "/atmosphere/contents/" + Tid) && fs::is_directory(sd + "/atmosphere/contents"));
    store.progress(nullptr);

    // Another patch already covers the folder: file by file, and they layer.
    CHECK(stage(store, "small", {{First, "small"}}));
    CHECK(store.enable("small", error) && store.enable("big", error));
    CHECK(inode(sd + "/" + Dir) != folder && allOnCard());
    CHECK(foil::readFile(storeRoot + "/small/" + First) == "small");
    CHECK(store.find("small")->shadowed.at(First) == "big");
    CHECK(store.disable("big", error) && allInStore() && foil::readFile(sd + "/" + First) == "small");
    CHECK(store.disable("small", error) && !fs::exists(sd + "/" + Dir));
    // The other one is off: the folder moves whole again.
    const auto again = inode(storeRoot + "/big/" + Dir);
    CHECK(store.enable("big", error) && inode(sd + "/" + Dir) == again && allOnCard());

    // A file the user added below the folder stays on the card.
    write(sd + "/" + Dir + "/Data/mine.txt", "mine");
    CHECK(store.disable("big", error) && allInStore());
    CHECK(foil::readFile(sd + "/" + Dir + "/Data/mine.txt") == "mine");
    CHECK(!fs::exists(sd + "/" + First) && !fs::exists(storeRoot + "/big/" + Dir + "/Data/mine.txt"));
    // The card's folder is the user's now: enabling goes file by file and
    // turning off leaves it as the user left it.
    CHECK(store.enable("big", error) && allOnCard() && store.disable("big", error));
    CHECK(foil::readFile(sd + "/" + Dir + "/Data/mine.txt") == "mine" && allInStore());
    fs::remove_all(sd + "/atmosphere/contents/" + std::string(Tid));
  }
  {
    // Power loss after the ledger planned a whole enable, before the rename.
    edit("big", true, "enable", true);
    const auto folder = inode(storeRoot + "/big/" + Dir);
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.find("big")->on && store.find("big")->pending.empty());
    CHECK(store.find("big")->roots.empty() && inode(sd + "/" + Dir) == folder && allOnCard());
  }
  {
    // Power loss after the rename, before the ledger caught up.
    edit("big", true, "enable", true);
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.find("big")->on && store.find("big")->pending.empty() && allOnCard());
  }
  {
    // Power loss in a whole disable, before the rename: it moves back whole.
    edit("big", false, "disable", true);
    const auto folder = inode(sd + "/" + Dir);
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && !store.find("big")->on && store.find("big")->pending.empty());
    CHECK(inode(storeRoot + "/big/" + Dir) == folder && allInStore() && !fs::exists(sd + "/" + Dir));
  }
  {
    // Half done file by file: some files already back in the store. The rest
    // follow one by one.
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.enable("big", error) && allOnCard());
    edit("big", false, "disable", true);
    for (int n = 0; n < 10; ++n) {
      const auto &path = bigFiles[size_t(n)];
      moveFile(sd + "/" + path, storeRoot + "/big/" + path);
    }
    foil::PatchStore reopened(sd, storeRoot);
    CHECK(reopened.load(error) && !reopened.find("big")->on && allInStore() && !fs::exists(sd + "/" + Dir));
  }
  fs::remove_all(root);
  return failures;
}
int runPatchMarkTests();
int runPatchTests() {
  int failures = runPatchOwnerTests() + runPatchLayerTests() + runPatchWholeTests() + runPatchMarkTests();
  // An app's patch reads as 홈브류 in the list, not as an unknown kind.
  CHECK(std::string(foil::patchKindLabel("homebrew")) != foil::patchKindLabel("something-else"));
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

    // Two patches that replace the same file layer: the one turned on last
    // wins, the other's copy goes back to its own store and waits there.
    const auto clash = store.conflicts("texture");
    CHECK(clash.size() == 1 && clash[0].path == Romfs && clash[0].owner == "korean");
    CHECK(store.enable("texture", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "hd");
    CHECK(foil::readFile(storeRoot + "/korean/" + Romfs) == "ko");
    CHECK(store.find("korean")->on && store.find("korean")->shadowed.size() == 1 &&
          store.find("korean")->shadowed.at(Romfs) == "texture");
    CHECK(store.find("texture")->shadowed.empty());
    {
      // The ledger says so too.
      foil::Error parse;
      auto json = foil::parseJson(foil::readFile(storeRoot + "/ledger.json"), parse);
      auto *shadowed = foil::field(foil::field(foil::field(json.get(), "patches"), "korean"), "shadowed");
      CHECK(foil::stringField(shadowed, Romfs.c_str()) == "texture");
    }
    // Cheats still merge in enable order.
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[A]\n1\n\n[B]\n2");
    CHECK(store.disable("texture", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "ko" && foil::readFile(storeRoot + "/texture/" + Romfs) == "hd");
    CHECK(store.find("korean")->shadowed.empty());
    CHECK(store.disable("korean", error));
    CHECK(!fs::exists(sd + "/" + Romfs) && fs::exists(storeRoot + "/korean/" + Romfs));
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[B]\n2");
    // Turning off the one underneath only takes it out of the stack.
    CHECK(store.enable("korean", error) && store.enable("texture", error) && store.disable("korean", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "hd" && foil::readFile(storeRoot + "/korean/" + Romfs) == "ko");
    CHECK(store.disable("texture", error) && !fs::exists(sd + "/" + Romfs));
    CHECK(fs::exists(storeRoot + "/korean/" + Romfs) && fs::exists(storeRoot + "/texture/" + Romfs));
    CHECK(store.disable("fps", error));
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0");
    CHECK(!fs::exists(storeRoot + "/_base/" + Cheat));

    // A file the user put in place is set aside under _base while any patch
    // covers the path and comes back when the last one goes off.
    write(sd + "/" + Romfs, "mine");
    const auto mine = store.conflicts("korean");
    CHECK(mine.size() == 1 && mine[0].owner.empty());
    CHECK(store.enable("korean", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "ko" && foil::readFile(storeRoot + "/_base/" + Romfs) == "mine");
    CHECK(store.enable("texture", error) && store.disable("korean", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "hd" && foil::readFile(storeRoot + "/_base/" + Romfs) == "mine");
    CHECK(store.disable("texture", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "mine" && !fs::exists(storeRoot + "/_base/" + Romfs));
    CHECK(store.enable("korean", error) && store.enable("texture", error) && store.disable("texture", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "ko" && store.disable("korean", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "mine");

    // A new version replaces the stored files, stays on and keeps its place:
    // a patch turned on after it stays on top.
    CHECK(store.enable("korean", error) && store.enable("texture", error));
    CHECK(stage(store, "korean", {{Romfs, "ko2"}, {Cheat, "[A2]\n1"}}, "2"));
    CHECK(store.find("korean")->on && store.find("korean")->version == "2");
    CHECK(foil::readFile(sd + "/" + Romfs) == "hd" && foil::readFile(storeRoot + "/korean/" + Romfs) == "ko2");
    CHECK(store.disable("texture", error));
    CHECK(foil::readFile(sd + "/" + Romfs) == "ko2");
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0\n\n[A2]\n1");

    // Removing an enabled patch takes its files back first; the user's file
    // comes back with it.
    CHECK(store.remove("korean", error));
    CHECK(!store.find("korean") && !fs::exists(storeRoot + "/korean") && foil::readFile(sd + "/" + Romfs) == "mine");
    CHECK(foil::readFile(sd + "/" + Cheat) == "[user]\n04000000 0 0");
    CHECK(store.remove("texture", error) && foil::readFile(sd + "/" + Romfs) == "mine");
    fs::remove(sd + "/" + Romfs);
  }
  {
    // Power loss mid-enable: the ledger says "enable", load finishes it.
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.records().size() == 1);
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
  // The game a patch is for comes with it when the server does not list it.
  {
    const auto list = foil::net::parsePatches(std::string(R"({"patches":[
      {"id":"a-1","title_id":"01001730144DA000","name":"한글패치","required":false,
       "game":{"name":"Base","names":{"ko":"한국어 이름","ja":"日本語"},"icon":"/api/icon/01001730144DA000"}},
      {"id":"b-2","title_id":"01002A000CD48000","name":"한글패치","game":{"icon":"javascript:x"}},
      {"id":"c-3","title_id":"010043600B6A6000","name":"한글패치","required":"yes"}]})"));
    CHECK(list.size() == 3);
    if (list.size() == 3) {
      CHECK(list[0].gameName == "Base");
      CHECK(list[0].gameNames.size() == 2 && *foil::textIn(list[0].gameNames, "ko") == "한국어 이름" &&
            *foil::textIn(list[0].gameNames, "ja") == "日本語");
      CHECK(list[0].gameIcon == "/api/icon/01001730144DA000");
      CHECK(list[1].gameIcon.empty()); // neither http(s) nor a server path
      CHECK(list[2].gameName.empty() && list[2].gameNames.empty());
      // Only a real true marks a patch the game needs.
      CHECK(!list[0].required && !list[2].required);
    }
  }
  // A patch's own pictures: full addresses or server paths, at most 8
  // screenshots; anything else is left out.
  {
    const auto list = foil::net::parsePatches(std::string(R"({"patches":[
      {"id":"p-1","title_id":"01001730144DA000","icon_url":"/api/patches/p-1/icon?v=2","banner_url":"javascript:x",
       "screenshots":["/s/1?v=1","https://cdn.example/2.jpg",3,"ftp://x","/4","/5","/6","/7","/8","/9"]},
      {"id":"p-2","title_id":"01001730144DA000","icon_url":null,"banner_url":"https://cdn.example/b.jpg"}]})"));
    CHECK(list.size() == 2);
    if (list.size() == 2) {
      CHECK(list[0].iconUrl == "/api/patches/p-1/icon?v=2" && list[0].bannerUrl.empty());
      CHECK(list[0].screenshots.size() == 8 && list[0].screenshots[1] == "https://cdn.example/2.jpg");
      CHECK(list[1].iconUrl.empty() && list[1].bannerUrl == "https://cdn.example/b.jpg" && list[1].screenshots.empty());
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
      {"id":"g-1","title_id":"01001730144DA000","homebrew":"sm64","name":"게임 패치","required":true,"update_version":2097152}]})"));
    CHECK(list.size() == 2);
    if (list.size() == 2) {
      CHECK(list[0].id == "hb-1" && list[0].titleId.empty() && list[0].homebrewApp == "sm64");
      CHECK(list[0].kinds.size() == 1 && list[0].kinds[0] == "homebrew" && list[0].name == "HD 텍스처");
      // A game patch stays a game patch.
      CHECK(list[1].titleId == "01001730144DA000" && list[1].homebrewApp.empty());
      CHECK(list[1].required && !list[0].required);
      // The version number it was made for, when the server says (else -1).
      CHECK(list[1].updateVersion == 2097152 && list[0].updateVersion == -1);
    }
  }
  // A patch waiting in 다운로드's queue is kept as text and read back whole,
  // a game's and an app's alike.
  {
    foil::UserPatch game;
    game.id = "font-1";
    game.titleId = "01001730144DA000";
    game.name = "한글 글꼴";
    game.version = "1.2";
    game.gameVersion = "1.0.3";
    game.hash = "abc";
    game.size = 12345;
    game.files = 3;
    game.updatedAt = 1700000000;
    game.required = true;
    game.updateVersion = 65536;
    game.kinds = {"romfs", "cheats"};
    game.gameName = "Base";
    game.gameNames = {{"ko", "한국어"}, {"ja", "日本語"}, {"zh-Hant", "繁體"}};
    game.gameIcon = "/api/icon/01001730144DA000";
    const auto back = foil::net::parsePatches(foil::net::serializePatch(game));
    CHECK(back.size() == 1);
    if (back.size() == 1) {
      CHECK(back[0].id == game.id && back[0].titleId == game.titleId && back[0].name == game.name);
      CHECK(back[0].version == "1.2" && back[0].gameVersion == "1.0.3" && back[0].hash == "abc");
      CHECK(back[0].size == 12345 && back[0].files == 3 && back[0].updatedAt == 1700000000 && back[0].required);
      CHECK(back[0].updateVersion == 65536);
      CHECK(back[0].kinds == game.kinds && back[0].gameName == "Base" && back[0].gameIcon == game.gameIcon);
      CHECK(back[0].gameNames.size() == 3 && *foil::textIn(back[0].gameNames, "ja") == "日本語" &&
            *foil::textIn(back[0].gameNames, "zh-Hant") == "繁體");
    }
    foil::UserPatch app;
    app.id = "hb-1";
    app.homebrewApp = "sm64";
    app.kinds = {"homebrew"};
    const auto appBack = foil::net::parsePatches(foil::net::serializePatch(app));
    CHECK(appBack.size() == 1 && appBack[0].titleId.empty() && appBack[0].homebrewApp == "sm64");
    CHECK(appBack.size() == 1 && appBack[0].updateVersion == -1); // null stays unknown
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
    // The app's own files are reported, set aside on enable and come back on disable.
    const auto own = store.conflicts("hb-1");
    CHECK(own.size() == 1 && own[0].path == Config && own[0].owner.empty());
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
      moveFile(hbSd + "/" + Config, hbStore + "/hb-1/" + Config);
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
