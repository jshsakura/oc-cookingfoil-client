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
  const auto pieces = foil::downloadPieces(root.string());
  CHECK(pieces.size() == 3);
  bool found = false;
  for (const auto &[path, bytes] : pieces) {
    if (path == staging.string()) {
      found = true;
      CHECK(bytes == 16);
    }
  }
  const auto removed = foil::cleanDownloadPieces(pieces);
  CHECK(removed.error.empty() && removed.bytes == 20);
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
