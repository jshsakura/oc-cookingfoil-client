#include "Test.hpp"
#include "core/Json.hpp"
#include "core/Patches.hpp"
#include <filesystem>
#include <unistd.h>
namespace fs = std::filesystem;
// A patch's file on the card is told from one the user put there by the mark
// the ledger keeps; the light check reads only what the ledger names.
namespace {
constexpr const char *Tid = "0100F2C0115B6000";
const std::string Dir = std::string("atmosphere/contents/") + Tid + "/romfs";
const std::string Text = Dir + "/Data/text.bin";
const std::string Font = Dir + "/Data/font.bin";
const std::string Mine = Dir + "/Data/mine.bin";
void write(const std::string &path, const std::string &body) {
  fs::create_directories(fs::path(path).parent_path());
  std::string ignored;
  foil::atomicWrite(path, body, ignored);
}
foil::UserPatch patch(const std::string &id) {
  foil::UserPatch p;
  p.id = id;
  p.titleId = Tid;
  p.name = id;
  p.version = "1";
  return p;
}
// Puts files into the patch's staging folder and takes it into the store.
bool stage(foil::PatchStore &store, const std::string &id,
           const std::vector<std::pair<std::string, std::string>> &files) {
  for (const auto &[path, body] : files)
    write(store.staging(id) + "/" + path, body);
  foil::Error error;
  return store.commit(patch(id), error);
}
// Runs a fresh check to the end.
std::map<std::string, foil::PatchHealth> check(const std::string &sd, const std::string &storeRoot,
                                               const foil::PatchStore &store) {
  foil::PatchChecker checker(sd, storeRoot, store.records());
  checker.run(100000, std::chrono::seconds(30));
  return checker.health();
}
} // namespace
int runPatchMarkTests() {
  int failures = 0;
  const auto root = fs::temp_directory_path() / ("cf-patchmarks-" + std::to_string(getpid()));
  fs::remove_all(root);
  const auto sd = (root / "sd").string(), storeRoot = (root / "sd/switch/cookingfoil/patches").string();
  const auto onCard = [&](const std::string &path) { return foil::readFile(sd + "/" + path); };
  const auto inStore = [&](const std::string &id, const std::string &path) {
    return foil::readFile(storeRoot + "/" + id + "/" + path);
  };
  {
    // A file of the user's already in the folder: the patch goes file by
    // file, and each file it puts on the card is marked as it lands.
    write(sd + "/" + Mine, "mine");
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && stage(store, "p", {{Text, "text"}, {Font, "font"}}));
    const auto before = foil::fileMark(storeRoot + "/p/" + Text);
    CHECK(store.enable("p", error) && onCard(Text) == "text");
    const auto &marks = store.find("p")->marks;
    CHECK(marks.size() == 2 && marks.at(Text).size == 4);
    // A rename keeps the modified time on this file system, so it is compared.
    CHECK(before && marks.at(Text).mtime != 0 && marks.at(Text) == *before);
    // The marks survive the ledger.
    foil::PatchStore reopened(sd, storeRoot);
    CHECK(reopened.load(error) && reopened.find("p")->marks == marks);
    CHECK(check(sd, storeRoot, reopened)["p"].changed == 0);

    // The user writes over the patch's file. Turning the patch off leaves
    // the user's file on the card, takes the patch's other file back to its
    // store, and never takes the user's file into the store.
    write(sd + "/" + Text, "the user's own text");
    const auto health = check(sd, storeRoot, reopened);
    CHECK(health.at("p").changed == 1 && health.at("p").missing == 0);
    CHECK(reopened.disable("p", error));
    CHECK(onCard(Text) == "the user's own text" && onCard(Mine) == "mine");
    CHECK(inStore("p", Font) == "font" && !fs::exists(sd + "/" + Font));
    // Its copy of that path is gone: the path is marked foreign.
    CHECK(!fs::exists(storeRoot + "/p/" + Text) && reopened.find("p")->marks.size() == 1);
    CHECK(reopened.find("p")->marks.at(Text).foreign());
    // Back on: its copy is gone, so the user's file stays where it is, and
    // the check still tells it apart.
    CHECK(reopened.enable("p", error) && onCard(Text) == "the user's own text" && onCard(Font) == "font");
    CHECK(check(sd, storeRoot, reopened).at("p").changed == 1);
    {
      foil::PatchStore again(sd, storeRoot); // the foreign mark survives the ledger
      CHECK(again.load(error) && again.find("p")->marks.at(Text).foreign());
    }
    // Deleting the patch never deletes the user's file.
    CHECK(reopened.remove("p", error));
    CHECK(onCard(Text) == "the user's own text" && onCard(Mine) == "mine" && !fs::exists(sd + "/" + Font));
    CHECK(!fs::exists(storeRoot + "/p"));
  }
  {
    // Same size, another time: still the user's file.
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    fs::remove(sd + "/" + Text);
    CHECK(store.load(error) && stage(store, "q", {{Text, "text"}}) && store.enable("q", error));
    write(sd + "/" + Text, "TEXT");
    fs::last_write_time(sd + "/" + Text, fs::last_write_time(sd + "/" + Text) + std::chrono::hours(1));
    CHECK(check(sd, storeRoot, store)["q"].changed == 1);
    CHECK(store.disable("q", error) && onCard(Text) == "TEXT" && !fs::exists(storeRoot + "/q/" + Text));
    // Another patch going on top sets the user's file aside, and it comes
    // back when that patch goes off.
    CHECK(stage(store, "r", {{Text, "r-text"}}) && store.enable("q", error) && store.enable("r", error));
    CHECK(onCard(Text) == "r-text" && foil::readFile(storeRoot + "/_base/" + Text) == "TEXT");
    CHECK(store.disable("r", error) && onCard(Text) == "TEXT" && inStore("r", Text) == "r-text");
    CHECK(store.remove("q", error) && store.remove("r", error) && onCard(Text) == "TEXT");
    fs::remove(sd + "/" + Text);
  }
  {
    // A missing file is found, and 다시 적용 puts it back from the store.
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && stage(store, "m", {{Text, "text"}, {Font, "font"}}) && store.enable("m", error));
    // Gone from the card with a copy left in the store, as after a move cut short.
    fs::create_directories(fs::path(storeRoot + "/m/" + Font).parent_path());
    fs::rename(sd + "/" + Font, storeRoot + "/m/" + Font);
    auto health = check(sd, storeRoot, store);
    CHECK(health.at("m").missing == 1 && health.at("m").changed == 0);
    size_t left = 9;
    CHECK(store.reapply("m", left, error) && left == 0 && onCard(Font) == "font");
    CHECK(store.find("m")->marks.count(Font) == 1);
    CHECK(check(sd, storeRoot, store).at("m").missing == 0);
    // Deleted outright: nothing in the store either, so it needs a download.
    fs::remove(sd + "/" + Text);
    health = check(sd, storeRoot, store);
    CHECK(health.at("m").missing == 1);
    CHECK(store.reapply("m", left, error) && left == 1 && !fs::exists(sd + "/" + Text));
    CHECK(store.remove("m", error));
  }
  fs::remove_all(root);
  {
    // A folder moved whole is marked file by file; one file replaced keeps
    // the folder from going back whole, and the user's file stays.
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    const std::string Third = Dir + "/Data/Deep/third.bin";
    CHECK(store.load(error) && stage(store, "w", {{Text, "text"}, {Font, "font"}, {Third, "third"}}));
    CHECK(store.enable("w", error) && !fs::exists(storeRoot + "/w/" + Dir));
    CHECK(store.find("w")->marks.size() == 3);
    write(sd + "/" + Font, "a font of the user's");
    const auto health = check(sd, storeRoot, store);
    CHECK(health.at("w").changed == 1 && health.at("w").missing == 0);
    CHECK(store.disable("w", error));
    CHECK(onCard(Font) == "a font of the user's" && !fs::exists(sd + "/" + Text) && !fs::exists(sd + "/" + Third));
    CHECK(inStore("w", Text) == "text" && inStore("w", Third) == "third" && !fs::exists(storeRoot + "/w/" + Font));
    // Its whole folder gone from the card counts every file missing with
    // one lookup for the folder.
    CHECK(store.remove("w", error) && onCard(Font) == "a font of the user's");
    fs::remove(sd + "/" + Font);
    CHECK(stage(store, "w", {{Text, "text"}, {Font, "font"}, {Third, "third"}}) && store.enable("w", error));
    fs::remove_all(sd + "/" + Dir);
    foil::PatchChecker checker(sd, storeRoot, store.records());
    CHECK(checker.run(100, std::chrono::seconds(30)) && checker.health().at("w").missing == 3);
    CHECK(checker.looks() == 1); // the folder only
    CHECK(store.remove("w", error));
  }
  fs::remove_all(root);
  {
    // A ledger from before marks still loads, and its patch's file on the
    // card still counts as the patch's until the next move marks it.
    const auto ledger = storeRoot + "/ledger.json";
    write(sd + "/" + Text, "old");
    write(storeRoot + "/old/.keep", "");
    write(ledger, std::string(R"({"order":1,"cheatPaths":[],"patches":{
      "old":{"titleId":")") + Tid + R"(","name":"old","version":"1","hash":"","on":true,"pending":"",
             "files":[")" + Text + R"("],"cheats":[],"bytes":3,"order":1}}})");
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    CHECK(store.load(error) && store.find("old")->on && store.find("old")->marks.empty());
    CHECK(check(sd, storeRoot, store).at("old").changed == 0);
    CHECK(store.disable("old", error) && !fs::exists(sd + "/" + Text) && inStore("old", Text) == "old");
    CHECK(store.enable("old", error) && store.find("old")->marks.count(Text) == 1);
    // A mark for a path the patch does not list is dropped on load.
    foil::Error parse;
    auto json = foil::parseJson(foil::readFile(ledger), parse);
    auto *marks = foil::field(foil::field(foil::field(json.get(), "patches"), "old"), "marks");
    auto *pair = json_object_new_array();
    json_object_array_add(pair, json_object_new_int64(1));
    json_object_array_add(pair, json_object_new_int64(1));
    json_object_object_add(marks, "atmosphere/elsewhere.bin", pair);
    write(ledger, json_object_to_json_string(json.get()));
    foil::PatchStore reopened(sd, storeRoot);
    CHECK(reopened.load(error) && reopened.find("old")->marks.size() == 1);
  }
  fs::remove_all(root);
  {
    // The check reads only what the ledger says enabled patches have on the
    // card, within its budget, and picks up where it stopped.
    write(sd + "/" + Mine, "mine"); // keeps the folder from moving whole
    foil::PatchStore store(sd, storeRoot);
    foil::Error error;
    std::vector<std::pair<std::string, std::string>> many;
    for (int n = 0; n < 40; ++n)
      many.push_back({Dir + "/Many/f" + std::to_string(n) + ".bin", "x" + std::to_string(n)});
    CHECK(store.load(error) && stage(store, "many", many) && store.enable("many", error));
    CHECK(stage(store, "off", {{Dir + "/Off/never.bin", "off"}})); // stored, never on
    for (int n = 0; n < 5; ++n)
      write(sd + "/unrelated/u" + std::to_string(n) + ".bin", "u");
    fs::remove(sd + "/" + many[7].first);
    foil::PatchChecker checker(sd, storeRoot, store.records());
    CHECK(checker.files() == 40); // neither the off patch nor anyone else's files
    CHECK(!checker.run(10, std::chrono::seconds(30)) && checker.looks() <= 11 && !checker.finished());
    const std::atomic<bool> stop{true};
    const auto looked = checker.looks();
    CHECK(!checker.run(100, std::chrono::seconds(30), &stop) && checker.looks() == looked);
    CHECK(!checker.run(100, std::chrono::milliseconds(0)) && checker.looks() == looked);
    while (!checker.run(10, std::chrono::seconds(30))) {
    }
    CHECK(checker.finished() && checker.health().size() == 1);
    CHECK(checker.health().at("many").missing == 1 && checker.health().at("many").changed == 0);
    CHECK(checker.looks() == 40 + 1); // one per file and one for their folder
  }
  fs::remove_all(root);
  return failures;
}
