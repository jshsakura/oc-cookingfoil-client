// Ported from oc-save-keeper (MIT), source/zip/ZipArchive.cpp.
// Copyright (c) 2026 jshsakura.
#include "core/Json.hpp"
#include "core/Zip.hpp"
#include "core/Log.hpp"
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <sys/stat.h>
extern "C" {
#include <minizip/unzip.h>
#include <minizip/zip.h>
}
namespace foil {
namespace {
constexpr size_t Buffer = 0x10000;
// The ratio check only applies above this size, as in save-keeper.
constexpr uint64_t RatioFloor = 1024 * 1024;
void makeParent(const std::string &path) {
  const auto parent = std::filesystem::path(path).parent_path();
  std::error_code ec;
  if (!parent.empty())
    makeDirectories(std::filesystem::path(parent).string());
}
} // namespace

ZipArchive::~ZipArchive() { close(); }

ZipArchive::ZipArchive(ZipArchive &&other) noexcept
    : zip_(other.zip_), unz_(other.unz_), path_(std::move(other.path_)), error_(std::move(other.error_)),
      extracted_(other.extracted_) {
  other.zip_ = other.unz_ = nullptr;
  other.extracted_ = 0;
}

ZipArchive &ZipArchive::operator=(ZipArchive &&other) noexcept {
  if (this != &other) {
    close();
    zip_ = other.zip_;
    unz_ = other.unz_;
    path_ = std::move(other.path_);
    error_ = std::move(other.error_);
    extracted_ = other.extracted_;
    other.zip_ = other.unz_ = nullptr;
    other.extracted_ = 0;
  }
  return *this;
}

bool ZipArchive::fail(const std::string &message) {
  error_ = message;
  logLine("zip: " + message);
  return false;
}

bool ZipArchive::create(const std::string &path) {
  close();
  error_.clear();
  makeParent(path);
  zip_ = zipOpen64(path.c_str(), APPEND_STATUS_CREATE);
  if (!zip_)
    return fail("cannot create " + path);
  path_ = path;
  return true;
}

bool ZipArchive::open(const std::string &path) {
  close();
  error_.clear();
  unz_ = unzOpen64(path.c_str());
  if (!unz_)
    return fail("cannot open " + path);
  path_ = path;
  extracted_ = 0;
  return true;
}

bool ZipArchive::close() {
  bool ok = true;
  if (zip_) {
    const int rc = zipClose(zip_, nullptr);
    zip_ = nullptr;
    if (rc != ZIP_OK)
      ok = fail("closing " + path_ + " failed (" + std::to_string(rc) + ")");
  }
  if (unz_) {
    unzClose(unz_);
    unz_ = nullptr;
  }
  return ok;
}

bool ZipArchive::addFile(const std::string &sourcePath, const std::string &archivePath) {
  if (!zip_)
    return fail("not open for writing");
  std::FILE *source = std::fopen(sourcePath.c_str(), "rb");
  if (!source)
    return fail("cannot read " + sourcePath);
  struct stat st {};
  std::time_t modified = stat(sourcePath.c_str(), &st) == 0 ? st.st_mtime : std::time(nullptr);
  std::tm t{};
  zip_fileinfo info{};
  if (localtime_r(&modified, &t)) {
    info.tmz_date.tm_year = t.tm_year;
    info.tmz_date.tm_mon = t.tm_mon;
    info.tmz_date.tm_mday = t.tm_mday;
    info.tmz_date.tm_hour = t.tm_hour;
    info.tmz_date.tm_min = t.tm_min;
    info.tmz_date.tm_sec = t.tm_sec;
  }
  if (zipOpenNewFileInZip(zip_, archivePath.c_str(), &info, nullptr, 0, nullptr, 0, nullptr, Z_DEFLATED,
                          Z_DEFAULT_COMPRESSION) != ZIP_OK) {
    std::fclose(source);
    return fail("cannot add entry " + archivePath);
  }
  std::array<char, Buffer> buffer{};
  bool ok = true;
  while (ok) {
    const size_t got = std::fread(buffer.data(), 1, buffer.size(), source);
    if (got > 0 && zipWriteInFileInZip(zip_, buffer.data(), static_cast<unsigned>(got)) != ZIP_OK)
      ok = fail("cannot write entry " + archivePath);
    if (got < buffer.size()) {
      if (std::ferror(source))
        ok = fail("read error in " + sourcePath);
      break;
    }
  }
  std::fclose(source);
  if (zipCloseFileInZip(zip_) != ZIP_OK && ok)
    ok = fail("cannot finish entry " + archivePath);
  return ok;
}

bool ZipArchive::addDirectory(const std::string &sourceDir, const std::string &archiveBase) {
  if (!zip_)
    return fail("not open for writing");
  DIR *dir = opendir(sourceDir.c_str());
  if (!dir)
    return fail("cannot open directory " + sourceDir);
  bool ok = true;
  while (ok) {
    const dirent *entry = readdir(dir);
    if (!entry)
      break;
    if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0)
      continue;
    const std::string source = sourceDir + "/" + entry->d_name;
    const std::string name = archiveBase.empty() ? entry->d_name : archiveBase + "/" + entry->d_name;
    struct stat st {};
    if (stat(source.c_str(), &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode))
      ok = addDirectory(source, name);
    else if (S_ISREG(st.st_mode))
      ok = addFile(source, name);
  }
  closedir(dir);
  return ok;
}

bool ZipArchive::extractFile(const std::string &archivePath, const std::string &destPath) {
  if (!unz_)
    return fail("not open for reading");
  if (!isSafeArchivePath(archivePath))
    return fail("unsafe entry " + archivePath);
  if (unzLocateFile(unz_, archivePath.c_str(), 0) != UNZ_OK)
    return fail("no entry " + archivePath);
  unz_file_info64 info{};
  if (unzGetCurrentFileInfo64(unz_, &info, nullptr, 0, nullptr, 0, nullptr, 0) != UNZ_OK)
    return fail("bad entry " + archivePath);
  const uint64_t size = info.uncompressed_size;
  if (size > MaxZipUncompressed)
    return fail("entry too large " + archivePath);
  if (extracted_ + size > MaxZipUncompressed)
    return fail("archive too large at " + archivePath);
  if (info.compressed_size > 0 && size > RatioFloor && size / info.compressed_size > MaxZipRatio)
    return fail("suspicious compression ratio " + archivePath);
  if (unzOpenCurrentFile(unz_) != UNZ_OK)
    return fail("cannot open entry " + archivePath);
  makeParent(destPath);
  std::FILE *out = std::fopen(destPath.c_str(), "wb");
  if (!out) {
    unzCloseCurrentFile(unz_);
    return fail("cannot create " + destPath);
  }
  std::array<char, Buffer> buffer{};
  bool ok = true;
  uint64_t written = 0;
  int got = 0;
  // The header sizes are attacker controlled, so the limits are checked
  // against the bytes actually inflated too.
  while (ok && (got = unzReadCurrentFile(unz_, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0) {
    written += static_cast<uint64_t>(got);
    if (written > size || extracted_ + written > MaxZipUncompressed)
      ok = fail("entry larger than declared " + archivePath);
    else if (std::fwrite(buffer.data(), 1, static_cast<size_t>(got), out) != static_cast<size_t>(got))
      ok = fail("cannot write " + destPath);
  }
  if (ok && got < 0)
    ok = fail("read error in entry " + archivePath);
  if (std::fclose(out) != 0 && ok)
    ok = fail("cannot write " + destPath);
  const int crc = unzCloseCurrentFile(unz_);
  if (ok && crc == UNZ_CRCERROR)
    ok = fail("CRC mismatch in " + archivePath);
  else if (ok && crc != UNZ_OK)
    ok = fail("cannot close entry " + archivePath);
  if (!ok) {
    std::remove(destPath.c_str());
    return false;
  }
  extracted_ += written;
  return true;
}

bool ZipArchive::extractAll(const std::string &destDir) {
  if (!unz_)
    return fail("not open for reading");
  std::error_code ec;
  makeDirectories(std::filesystem::path(destDir).string());
  if (unzGoToFirstFile(unz_) != UNZ_OK)
    return true;
  // Collect names first; extractFile moves the archive cursor.
  std::vector<std::string> names;
  do {
    if (names.size() >= MaxZipEntries)
      return fail("too many entries in " + path_);
    char name[512];
    unz_file_info64 info{};
    if (unzGetCurrentFileInfo64(unz_, &info, name, sizeof(name), nullptr, 0, nullptr, 0) != UNZ_OK)
      return fail("bad entry in " + path_);
    names.emplace_back(name);
  } while (unzGoToNextFile(unz_) == UNZ_OK);
  bool ok = true;
  for (const auto &name : names) {
    if (!isSafeArchivePath(name)) {
      logLine("zip: skipping unsafe entry " + name);
      continue;
    }
    const std::string dest = destDir + "/" + name;
    if (name.back() == '/') {
      makeDirectories(std::filesystem::path(dest).string());
      continue;
    }
    if (!extractFile(name, dest))
      ok = false;
  }
  return ok;
}

std::vector<std::string> ZipArchive::listFiles() {
  std::vector<std::string> files;
  if (!unz_ || unzGoToFirstFile(unz_) != UNZ_OK)
    return files;
  do {
    if (files.size() >= MaxZipEntries) {
      logLine("zip: entry limit reached in " + path_);
      break;
    }
    char name[512];
    if (unzGetCurrentFileInfo64(unz_, nullptr, name, sizeof(name), nullptr, 0, nullptr, 0) == UNZ_OK) {
      const size_t length = std::strlen(name);
      if (length > 0 && name[length - 1] != '/')
        files.emplace_back(name);
    }
  } while (unzGoToNextFile(unz_) == UNZ_OK);
  return files;
}

bool ZipArchive::hasFile(const std::string &archivePath) {
  return unz_ && unzLocateFile(unz_, archivePath.c_str(), 0) == UNZ_OK;
}

bool zipDirectory(const std::string &sourceDir, const std::string &zipPath) {
  ZipArchive zip;
  if (!zip.create(zipPath))
    return false;
  const bool added = zip.addDirectory(sourceDir);
  const bool closed = zip.close();
  if (!added || !closed) {
    std::remove(zipPath.c_str());
    return false;
  }
  return true;
}

bool unzipToDirectory(const std::string &zipPath, const std::string &destDir) {
  ZipArchive zip;
  if (!zip.open(zipPath))
    return false;
  const bool ok = zip.extractAll(destDir);
  zip.close();
  return ok;
}
} // namespace foil
