// Zip tests ported from oc-save-keeper (MIT): test_security_zip_path and the
// zip half of test_security_limits, plus archive round trips.
#include "Test.hpp"
#include "core/Zip.hpp"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>
namespace fs = std::filesystem;
namespace {
int pathTests() {
  int failures = 0;
  using foil::isSafeArchivePath;
  for (const char *safe : {"file.txt", "save/file.txt", "save/data/file.txt", "folder/subfolder/file.dat",
                           "save-file.txt", "save_file.txt", "save.file.txt", "save file.txt"})
    CHECK(isSafeArchivePath(safe));
  for (const char *unsafe :
       {"", "../etc/passwd", "save/../../etc/passwd", "save/../file.txt", "..\\windows\\system32",
        "save\\..\\file.txt", "/etc/passwd", "/root/.ssh/id_rsa", "/home/user/file.txt", "C:\\Windows\\System32",
        "D:\\secret\\file.txt", "c:\\users\\admin\\desktop", "\\\\server\\share\\file", "\\etc\\passwd",
        "file..txt", "save..backup/file.txt", "sdmc:/switch/x.nro", "romfs:/a", "save/sdmc:x", "a\\b"})
    CHECK(!isSafeArchivePath(unsafe));
  CHECK(foil::MaxZipUncompressed == 512ULL * 1024 * 1024);
  CHECK(foil::MaxZipRatio == 100U);
  return failures;
}
#ifndef FOIL_NO_MINIZIP
void writeRaw(const fs::path &path, const std::string &text) {
  fs::create_directories(path.parent_path());
  std::ofstream(path, std::ios::binary) << text;
}
std::string readRaw(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
int archiveTests() {
  int failures = 0;
  const fs::path root = fs::temp_directory_path() / ("foil_zip_" + std::to_string(getpid()));
  fs::remove_all(root);
  const auto src = root / "src", zipPath = root / "out.zip", dest = root / "dest";
  writeRaw(src / "save.dat", std::string(5000, 'x') + "end");
  writeRaw(src / "sub" / "slot1.bin", "slot one");
  writeRaw(src / ".dropkeep.meta", "title_id=1\nbackup_name=b\n");
  CHECK(foil::zipDirectory(src.string(), zipPath.string()));
  {
    foil::ZipArchive zip;
    CHECK(zip.open(zipPath.string()));
    CHECK(!zip.isWriting());
    CHECK(zip.listFiles().size() == 3);
    CHECK(zip.hasFile("sub/slot1.bin"));
    CHECK(!zip.hasFile("missing"));
    CHECK(!zip.extractFile("../escape", (root / "escape").string()));
    CHECK(!fs::exists(root / "escape"));
    CHECK(zip.close());
  }
  CHECK(foil::unzipToDirectory(zipPath.string(), dest.string()));
  CHECK(readRaw(dest / "save.dat") == std::string(5000, 'x') + "end");
  CHECK(readRaw(dest / "sub" / "slot1.bin") == "slot one");
  CHECK(readRaw(dest / ".dropkeep.meta") == "title_id=1\nbackup_name=b\n");
  {
    // An archive with a traversal entry: the entry is skipped, the rest lands.
    const auto evil = root / "evil.zip";
    foil::ZipArchive zip;
    CHECK(zip.create(evil.string()));
    CHECK(zip.addFile((src / "sub" / "slot1.bin").string(), "../outside.bin"));
    CHECK(zip.addFile((src / "sub" / "slot1.bin").string(), "/abs.bin"));
    CHECK(zip.addFile((src / "sub" / "slot1.bin").string(), "ok.bin"));
    CHECK(zip.close());
    const auto out = root / "evil-out";
    CHECK(foil::unzipToDirectory(evil.string(), out.string()));
    CHECK(fs::exists(out / "ok.bin"));
    CHECK(!fs::exists(root / "outside.bin"));
    CHECK(!fs::exists(out / "abs.bin"));
  }
  {
    // 2MB of zeros compresses far past 100:1 and is refused.
    const auto bomb = root / "bomb.zip";
    writeRaw(root / "zeros.bin", std::string(2 * 1024 * 1024, '\0'));
    foil::ZipArchive zip;
    CHECK(zip.create(bomb.string()));
    CHECK(zip.addFile((root / "zeros.bin").string(), "zeros.bin"));
    CHECK(zip.close());
    foil::ZipArchive reader;
    CHECK(reader.open(bomb.string()));
    CHECK(!reader.extractFile("zeros.bin", (root / "bomb-out" / "zeros.bin").string()));
    CHECK(!reader.error().empty());
    CHECK(!fs::exists(root / "bomb-out" / "zeros.bin"));
  }
  {
    // A damaged payload fails extraction and leaves no partial file.
    std::string bytes = readRaw(zipPath);
    const auto at = bytes.find("save.dat");
    CHECK(at != std::string::npos);
    if (at != std::string::npos) {
      for (size_t i = at + 8 + 4; i < at + 8 + 24 && i < bytes.size(); ++i)
        bytes[i] = static_cast<char>(bytes[i] ^ 0x5A);
      const auto bad = root / "bad.zip";
      writeRaw(bad, bytes);
      foil::ZipArchive zip;
      CHECK(zip.open(bad.string()));
      CHECK(!zip.extractFile("save.dat", (root / "bad-out" / "save.dat").string()));
      CHECK(!fs::exists(root / "bad-out" / "save.dat"));
    }
  }
  if (fs::exists("/dev/full")) {
    // Writes are buffered, so the failure surfaces when the zip is closed.
    foil::ZipArchive zip;
    if (zip.create("/dev/full")) {
      zip.addFile((src / "sub" / "slot1.bin").string(), "slot1.bin");
      CHECK(!zip.close());
      CHECK(!zip.error().empty());
    }
  }
  CHECK(foil::ZipArchive().close());
  CHECK(!foil::unzipToDirectory((root / "missing.zip").string(), dest.string()));
  fs::remove_all(root);
  return failures;
}
#endif
} // namespace
int runZipTests() {
#ifndef FOIL_NO_MINIZIP
  return pathTests() + archiveTests();
#else
  return pathTests();
#endif
}
