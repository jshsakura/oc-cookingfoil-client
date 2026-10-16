#include "Test.hpp"
#include "core/CardPatches.hpp"
#include <filesystem>
#include <fstream>
// Patches already on the card that no record accounts for still show, by
// game and kind; a record's own files, a system module's folder and loose
// files outside the known layouts do not.
namespace {
void put(const std::filesystem::path &file, size_t bytes) {
  std::filesystem::create_directories(file.parent_path());
  std::ofstream(file, std::ios::binary) << std::string(bytes, 'x');
}
} // namespace
int runCardPatchTests() {
  int failures = 0;
  char temporary[] = "/tmp/cookingfoil-cardpatch-XXXXXX";
  const char *made = mkdtemp(temporary);
  CHECK(made);
  if (!made) return failures;
  const std::filesystem::path sd(made);
  const std::string game = "0100ABCD12340000";
  put(sd / "atmosphere/contents" / game / "romfs/data/a.bin", 100);
  put(sd / "atmosphere/contents" / game / "romfs/data/b.bin", 50);
  put(sd / "atmosphere/contents" / game / "cheats/1234567890ABCDEF.txt", 10);
  put(sd / "atmosphere/contents" / game / "cheats/readme.md", 5); // not a cheat file
  // An update's folder counts for its game.
  put(sd / "atmosphere/contents/0100ABCD12340800/exefs/main.npdm", 7);
  // FPSLocker, by Title ID, lower case on the card.
  put(sd / "SaltySD/plugins/FPSLocker/patches/0100beef00000000/1A2B.yaml", 3);
  // System modules and anything that is not a game's: left alone.
  put(sd / "atmosphere/contents/010000000000BD00/exefs.nsp", 999);
  put(sd / "atmosphere/contents/4200000000000010/exefs/main", 999);
  put(sd / "atmosphere/contents/notatitle/romfs/x", 999);
  // A file the ledger lists is that patch's: not counted again.
  foil::PatchRecord record;
  record.files = {"atmosphere/contents/" + game + "/romfs/data/b.bin"};
  const auto found = foil::scanCardPatches(sd.string(), {{"p1", record}});
  CHECK(found.size() == 2);
  if (found.size() == 2) {
    const auto &first = found[0].titleId == game ? found[0] : found[1];
    const auto &other = found[0].titleId == game ? found[1] : found[0];
    CHECK(first.titleId == game);
    CHECK((first.kinds == std::vector<std::string>{"romfs", "exefs", "cheats"}));
    CHECK(first.files == 3 && first.bytes == 117);
    CHECK(other.titleId == "0100BEEF00000000" && other.kinds == std::vector<std::string>{"fpslocker"});
  }
  // Every file a record's: nothing to show.
  record.files.push_back("atmosphere/contents/" + game + "/romfs/data/a.bin");
  record.cheats = {"atmosphere/contents/" + game + "/cheats/1234567890ABCDEF.txt"};
  record.files.push_back("atmosphere/contents/0100ABCD12340800/exefs/main.npdm");
  record.files.push_back("SaltySD/plugins/FPSLocker/patches/0100beef00000000/1A2B.yaml");
  CHECK(foil::scanCardPatches(sd.string(), {{"p1", record}}).empty());
  // A Mac that opened the folders leaves "._" files and .DS_Store: they
  // make no unknown patch of one the app installed.
  put(sd / "atmosphere/contents" / game / "romfs/data/._a.bin", 4);
  put(sd / "atmosphere/contents" / game / ".DS_Store", 6);
  put(sd / "atmosphere/contents" / game / "cheats/._1234567890ABCDEF.txt", 4);
  CHECK(foil::scanCardPatches(sd.string(), {{"p1", record}}).empty());
  // No card folders at all: nothing, and no error.
  CHECK(foil::scanCardPatches((sd / "missing").string(), {}).empty());
  std::filesystem::remove_all(sd);
  return failures;
}
