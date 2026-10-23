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
  const auto pieces = foil::downloadPieces(root.string());
  CHECK(pieces.size() == 2);
  bool found = false;
  for (const auto &[path, bytes] : pieces) {
    if (path == staging.string()) {
      found = true;
      CHECK(bytes == 16);
    }
    std::error_code ec;
    CHECK(fs::remove_all(path, ec) > 0 && !ec);
  }
  CHECK(found && !fs::exists(staging));
  CHECK(fs::exists(installed / "atmosphere/contents/example/romfs/data.bin"));
  CHECK(fs::exists(unrelated / "keep.bin"));
  CHECK(fs::exists(root / "switch/app/app.nro"));
  fs::remove_all(root);
  return failures;
}
