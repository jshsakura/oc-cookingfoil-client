// Exercise the production __SWITCH__ branch against FS service semantics.
// No POSIX remove/rename can accidentally make these checks pass on Linux.
#include "platform/FileSystem.hpp"
#include "core/Json.hpp"
#include "switch.h"
#include "../Test.hpp"
#include <cerrno>
#include <cstring>
#include <map>
#include <vector>
#include <filesystem>
#include <fstream>
#include <unistd.h>
namespace foil { void logLine(const std::string &) {} } // logging is outside this FS test
namespace {
FsFileSystem sd{1}, other{2};
std::map<std::string, FsDirEntryType> entries;
std::vector<std::string> calls;
Result deleteError = 0;
bool pretendDelete = false;
std::string physicalRoot;
bool failFinalRename = false;
}
int fsdevTranslatePath(const char *path, FsFileSystem **fs, char *out) {
  const std::string name = path;
  if (!physicalRoot.empty() && name.rfind(physicalRoot + "/", 0) == 0 && name.size() < FS_MAX_PATH) {
    *fs = &sd;
    std::strcpy(out, path);
    return 0;
  }
  const auto colon = name.find(':');
  if (colon == std::string::npos || name.size() - colon >= FS_MAX_PATH) {
    errno = EINVAL;
    return -1;
  }
  *fs = name.substr(0, colon) == "sdmc" ? &sd : &other;
  std::strcpy(out, path + colon + 1);
  return 0;
}
Result fsFsGetEntryType(FsFileSystem *, const char *path, FsDirEntryType *type) {
  if (!physicalRoot.empty() && std::string(path).rfind(physicalRoot + "/", 0) == 0) {
    if (!std::filesystem::exists(path)) return 0x202;
    *type = std::filesystem::is_directory(path) ? FsDirEntryType_Dir : FsDirEntryType_File;
    return 0;
  }
  const auto it = entries.find(path);
  if (it == entries.end()) return 0x202;
  *type = it->second;
  return 0;
}
Result fsFsDeleteFile(FsFileSystem *, const char *path) {
  calls.push_back("file " + std::string(path));
  if (deleteError) return deleteError;
  if (!physicalRoot.empty() && std::string(path).rfind(physicalRoot + "/", 0) == 0)
    return std::filesystem::remove(path) ? 0 : 0x202;
  if (!entries.count(path) || entries.at(path) != FsDirEntryType_File) return 0x202;
  if (!pretendDelete) entries.erase(path);
  return 0;
}
Result fsFsDeleteDirectoryRecursively(FsFileSystem *, const char *path) {
  calls.push_back("tree " + std::string(path));
  if (deleteError) return deleteError;
  if (!entries.count(path) || entries.at(path) != FsDirEntryType_Dir) return 0x202;
  if (!pretendDelete) {
    const auto prefix = std::string(path) + "/";
    for (auto it = entries.begin(); it != entries.end();) {
      if (it->first == path || it->first.rfind(prefix, 0) == 0) it = entries.erase(it);
      else ++it;
    }
  }
  return 0;
}
Result rename(const char *from, const char *to, FsDirEntryType type) {
  calls.push_back(std::string(type == FsDirEntryType_Dir ? "rename tree " : "rename file ") + from);
  if (!physicalRoot.empty() && std::string(from).rfind(physicalRoot + "/", 0) == 0) {
    if (!std::filesystem::exists(from)) return 0x202;
    if (std::filesystem::exists(to)) return 0x402;
    if (failFinalRename) return 0xE02;
    std::filesystem::rename(from, to);
    return 0;
  }
  if (!entries.count(from) || entries.at(from) != type) return 0x202;
  if (entries.count(to)) return 0x402;
  entries[to] = entries.at(from);
  entries.erase(from);
  return 0;
}
Result fsFsRenameFile(FsFileSystem *, const char *from, const char *to) {
  return rename(from, to, FsDirEntryType_File);
}
Result fsFsRenameDirectory(FsFileSystem *, const char *from, const char *to) {
  return rename(from, to, FsDirEntryType_Dir);
}
int main() {
  int failures = 0;
  std::error_code ec;
  using foil::platform::removePath;
  using foil::platform::renamePath;
  entries = {{"/ledger.json", FsDirEntryType_File}, {"/big-file", FsDirEntryType_File},
             {"/patch", FsDirEntryType_Dir}, {"/patch/romfs", FsDirEntryType_Dir},
             {"/patch/romfs/data", FsDirEntryType_File}, {"/keep", FsDirEntryType_File}};
  CHECK(removePath("sdmc:/ledger.json", ec) && !ec);
  CHECK(calls.back() == "file /ledger.json" && !entries.count("/ledger.json"));
  // A concatenation file is File to FS, even though FAT32 stores chunks.
  CHECK(removePath("sdmc:/big-file", ec) && calls.back() == "file /big-file");
  CHECK(removePath("sdmc:/patch", ec) && calls.back() == "tree /patch");
  CHECK(!entries.count("/patch/romfs/data") && entries.count("/keep"));
  const auto count = calls.size();
  CHECK(removePath("sdmc:/absent", ec) && !ec && calls.size() == count);
  for (const auto &path : {"", "/", "sdmc:", "sdmc:/", "sdmc:/patch/..", ".", ".."})
    CHECK(!removePath(path, ec) && ec == std::errc::invalid_argument);
  CHECK(calls.size() == count);
  deleteError = 0xE02;
  CHECK(!removePath("sdmc:/keep", ec));
  CHECK(ec.message().find("0x00000E02") != std::string::npos && entries.count("/keep"));
  deleteError = 0;
  pretendDelete = true;
  CHECK(!removePath("sdmc:/keep", ec) && ec == std::errc::device_or_resource_busy);
  pretendDelete = false;
  entries["/incoming"] = FsDirEntryType_File;
  CHECK(!renamePath("sdmc:/incoming", "sdmc:/keep", ec) && ec == std::errc::file_exists);
  CHECK(ec.message().find("0x00000402") != std::string::npos);
  CHECK(entries.count("/incoming") && entries.count("/keep"));
  CHECK(!renamePath("sdmc:/missing", "sdmc:/keep", ec) && ec == std::errc::no_such_file_or_directory);
  CHECK(entries.count("/keep"));
  CHECK(!renamePath("sdmc:/incoming", "other:/new", ec) && ec == std::errc::cross_device_link);
  CHECK(removePath("sdmc:/keep", ec));
  CHECK(renamePath("sdmc:/incoming", "sdmc:/keep", ec) && !ec && calls.back() == "rename file /incoming");
  entries["/stage.part"] = FsDirEntryType_Dir;
  CHECK(renamePath("sdmc:/stage.part", "sdmc:/stage", ec) && calls.back() == "rename tree /stage.part");
  // The real shared replacement, with Horizon's refusal to overwrite.
  std::string error;
  entries["/new"] = entries["/old"] = FsDirEntryType_File;
  CHECK(foil::replaceFile("sdmc:/new", "sdmc:/old", error));
  CHECK(!entries.count("/new") && entries.count("/old") && error.empty());
  CHECK(!foil::replaceFile("sdmc:/missing", "sdmc:/old", error));
  CHECK(entries.count("/old"));
  entries["/new"] = FsDirEntryType_File;
  deleteError = 0xE02;
  CHECK(!foil::replaceFile("sdmc:/new", "sdmc:/old", error));
  CHECK(entries.count("/new") && entries.count("/old") && error.find("0x00000E02") != std::string::npos);
  deleteError = 0;
  pretendDelete = true;
  const auto beforeFalseDelete = calls.size();
  CHECK(!foil::replaceFile("sdmc:/new", "sdmc:/old", error));
  CHECK(entries.count("/new") && entries.count("/old"));
  CHECK(calls.size() == beforeFalseDelete + 2); // one rename, one delete; no blind retry
  pretendDelete = false;
  entries["/folder.part"] = entries["/folder"] = FsDirEntryType_Dir;
  entries["/folder/stale"] = FsDirEntryType_File;
  CHECK(foil::replaceFile("sdmc:/folder.part", "sdmc:/folder", error));
  CHECK(!entries.count("/folder/stale") && !entries.count("/folder.part") && entries.count("/folder"));
  physicalRoot = (std::filesystem::temp_directory_path() / ("cf-fs-save-" + std::to_string(getpid()))).string();
  std::filesystem::create_directories(physicalRoot);
  const auto ledger = physicalRoot + "/ledger.json";
  std::ofstream(ledger) << "old";
  failFinalRename = true;
  CHECK(!foil::atomicWrite(ledger, "complete new record", error));
  CHECK(!std::filesystem::exists(ledger));
  CHECK(foil::readFile(ledger + ".tmp") == "complete new record");
  failFinalRename = false;
  CHECK(foil::atomicWrite(ledger, "complete new record", error));
  CHECK(foil::readFile(ledger) == "complete new record" && !std::filesystem::exists(ledger + ".tmp"));
  const auto cwd = std::filesystem::current_path();
  std::filesystem::current_path(physicalRoot);
  CHECK(!foil::atomicWrite("", "no destination", error));
  CHECK(!std::filesystem::exists(".tmp"));
  std::filesystem::current_path(cwd);
  std::filesystem::remove_all(physicalRoot);
  std::cout << "Switch FS service checks: " << failures << " failures\n";
  return failures ? 1 : 0;
}
