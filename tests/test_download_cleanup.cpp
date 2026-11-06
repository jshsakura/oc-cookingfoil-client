#include "Test.hpp"
#include "core/DownloadCleanup.hpp"
#include <filesystem>
#include <fstream>
#include <unistd.h>
int runDownloadCleanupTests() {
  namespace fs = std::filesystem;
  int failures = 0;
  char pattern[] = "/tmp/cookingfoil-cleanup-XXXXXX";
  const auto directory = mkdtemp(pattern);
  CHECK(directory != nullptr);
  if (!directory) return failures;
  const fs::path root(directory);
  const auto staging = root / "switch/cookingfoil/patches/example.part";
  const auto installed = root / "switch/cookingfoil/patches/example";
  const auto unrelated = root / "switch/other.part";
  const auto write = [](const fs::path &path) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << "data";
  };
  write(staging / "atmosphere/contents/example/romfs/data.bin");
  write(staging / ".resume");
  write(staging / ".resume.old");
  write(staging / ".resume.tmp");
  write(installed / "atmosphere/contents/example/romfs/data.bin");
  write(unrelated / "keep.bin");
  write(root / "switch/app/.cf-example.part");
  write(root / "switch/app/app.nro");
  fs::create_directories(root / "switch/cookingfoil/patches/empty.part");
  // A firmware version cut while unpacking, and its resume point; the
  // finished version under firmware/ is not a piece.
  write(root / "switch/cookingfoil/staging/firmware-23.0.1/a.nca");
  write(root / "switch/cookingfoil/resume/firmware-23.0.1.json");
  write(root / "switch/cookingfoil/staging/other/keep.bin");
  write(root / "firmware/23.0.1/a.nca");
  const auto pieces = foil::downloadPieces(root.string());
  CHECK(pieces.size() == 5);
  bool found = false;
  for (const auto &[path, bytes] : pieces) {
    if (path == staging.string()) {
      found = true;
      CHECK(bytes == 16);
    }
  }
  const auto removed = foil::cleanDownloadPieces(pieces);
  CHECK(removed.error.empty() && removed.bytes == 28);
  CHECK(!fs::exists(root / "switch/cookingfoil/staging/firmware-23.0.1") &&
        !fs::exists(root / "switch/cookingfoil/resume/firmware-23.0.1.json"));
  CHECK(fs::exists(root / "switch/cookingfoil/staging/other/keep.bin") && fs::exists(root / "firmware/23.0.1/a.nca"));
  CHECK(!fs::exists(root / "switch/cookingfoil/patches/empty.part"));
  CHECK(found && !fs::exists(staging));
  CHECK(fs::exists(installed / "atmosphere/contents/example/romfs/data.bin"));
  CHECK(fs::exists(unrelated / "keep.bin"));
  CHECK(fs::exists(root / "switch/app/app.nro"));
  write(root / "blocked");
  write(root / "removable.part");
  const auto partial = foil::cleanDownloadPieces({{(root / "blocked/child.part").string(), 9},
                                                  {(root / "removable.part").string(), 4}});
  CHECK(!partial.error.empty() && partial.bytes == 4);
  CHECK(fs::exists(root / "blocked") && !fs::exists(root / "removable.part"));
  fs::remove_all(root);
  return failures;
}
