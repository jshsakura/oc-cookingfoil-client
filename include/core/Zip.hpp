#pragma once
// Zip archives for save backups, ported from oc-save-keeper (MIT,
// include/zip/ZipArchive.hpp). Uses minizip.
#include <cstdint>
#include <string>
#include <vector>
namespace foil {
// Zip bomb limits, the same as save-keeper's.
constexpr uint64_t MaxZipUncompressed = 512ULL * 1024 * 1024;
constexpr uint32_t MaxZipRatio = 100;
constexpr size_t MaxZipEntries = 10000;
// Rejects empty names, any "..", absolute paths, backslashes and any colon:
// a device name (sdmc:, romfs:) as much as a drive letter. Inline so it is
// usable where minizip is not linked.
inline bool isSafeArchivePath(const std::string &archivePath) {
  if (archivePath.empty() || archivePath.find("..") != std::string::npos)
    return false;
  return archivePath[0] != '/' && archivePath.find_first_of("\\:") == std::string::npos;
}

class ZipArchive {
public:
  ZipArchive() = default;
  ~ZipArchive();
  ZipArchive(const ZipArchive &) = delete;
  ZipArchive &operator=(const ZipArchive &) = delete;
  ZipArchive(ZipArchive &&other) noexcept;
  ZipArchive &operator=(ZipArchive &&other) noexcept;

  bool create(const std::string &path);
  bool open(const std::string &path);
  // False when the central directory or the file could not be written, so a
  // backup that failed here must not be kept.
  bool close();
  bool addFile(const std::string &sourcePath, const std::string &archivePath);
  // Adds regular files under sourceDir recursively, named relative to it.
  bool addDirectory(const std::string &sourceDir, const std::string &archiveBase = "");
  bool extractFile(const std::string &archivePath, const std::string &destPath);
  // Unsafe entry names are skipped and logged, as in save-keeper.
  bool extractAll(const std::string &destDir);
  std::vector<std::string> listFiles();
  bool hasFile(const std::string &archivePath);
  bool isWriting() const { return zip_ != nullptr; }
  const std::string &error() const { return error_; }

private:
  bool fail(const std::string &message);
  void *zip_ = nullptr; // zipFile
  void *unz_ = nullptr; // unzFile
  std::string path_, error_;
  uint64_t extracted_ = 0; // bytes written by extractFile since open
};

bool zipDirectory(const std::string &sourceDir, const std::string &zipPath);
bool unzipToDirectory(const std::string &zipPath, const std::string &destDir);
} // namespace foil
