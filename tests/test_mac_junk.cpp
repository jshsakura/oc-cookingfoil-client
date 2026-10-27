#include "Test.hpp"
#include "core/MacJunk.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
int runMacJunkTests() {
  namespace fs = std::filesystem;
  int failures = 0;
  CHECK(foil::macJunk("._game.nsp", false, false));
  CHECK(foil::macJunk(".DS_Store", false, false));
  CHECK(foil::macJunk(".Spotlight-V100", true, true));
  CHECK(!foil::macJunk(".Spotlight-V100", true, false)); // only at the top
  CHECK(!foil::macJunk("._folder", true, false));          // a folder named so is the user's
  CHECK(!foil::macJunk(".cookingfoil", false, true));
  CHECK(!foil::macJunk("game.nsp", false, true));

  char pattern[] = "/tmp/cookingfoil-macjunk-XXXXXX";
  const auto directory = mkdtemp(pattern);
  CHECK(directory != nullptr);
  if (!directory) return failures;
  const fs::path root(directory);
  const auto write = [](const fs::path &path, const char *text = "data") {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << text;
  };
  write(root / "._hbmenu.nro");
  write(root / ".DS_Store", "12345");
  write(root / ".Trashes/501/old.bin");
  write(root / ".fseventsd/0001");
  write(root / "hbmenu.nro");
  write(root / "switch/app/app.nro");
  write(root / "switch/app/._app.nro");
  write(root / "switch/app/.DS_Store");
  write(root / "switch/other/._left.nro"); // a folder the PC did not write to
  write(root / "atmosphere/contents/._x/keep"); // "._x" is a folder: kept
  const auto swept = foil::sweepMacJunk(root.string(), {"/switch/app", "switch/app", "/../../etc"});
  CHECK(swept.removed == 6 && swept.failed == 0);
  CHECK(swept.bytes == 4 + 5 + 4 + 4);
  CHECK(!fs::exists(root / "._hbmenu.nro") && !fs::exists(root / ".Trashes") && !fs::exists(root / ".fseventsd"));
  CHECK(!fs::exists(root / "switch/app/._app.nro") && !fs::exists(root / "switch/app/.DS_Store"));
  CHECK(fs::exists(root / "hbmenu.nro") && fs::exists(root / "switch/app/app.nro"));
  CHECK(fs::exists(root / "switch/other/._left.nro"));
  CHECK(fs::exists(root / "atmosphere/contents/._x/keep"));
  CHECK(foil::sweepMacJunk("", {"switch"}).removed == 0);
  fs::remove_all(root);
  return failures;
}
