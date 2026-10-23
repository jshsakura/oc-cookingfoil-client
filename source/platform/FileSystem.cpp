#include "platform/FileSystem.hpp"
#include <cerrno>
#include <cstdio>
#include <filesystem>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace foil::platform {
#ifdef __SWITCH__
namespace {
constexpr Result NotFound = 0x202;
class FsErrors : public std::error_category {
public:
  const char *name() const noexcept override { return "switch-fs"; }
  std::string message(int value) const override {
    char code[32];
    std::snprintf(code, sizeof code, "Switch FS 0x%08X", unsigned(value));
    return code;
  }
  std::error_condition default_error_condition(int value) const noexcept override {
    if (value == 0x202) return std::errc::no_such_file_or_directory;
    if (value == 0x402) return std::errc::file_exists;
    return {value, *this};
  }
};
bool result(Result rc, std::error_code &error) {
  static FsErrors category;
  error = R_FAILED(rc) ? std::error_code(int(rc), category) : std::error_code{};
  return !error;
}
struct Path {
  FsFileSystem *fs = nullptr;
  char name[FS_MAX_PATH]{};
  bool translate(const std::string &path, std::error_code &error) {
    if (fsdevTranslatePath(path.c_str(), &fs, name) != 0) {
      error = {errno, std::generic_category()};
      return false;
    }
    return true;
  }
};
}
#endif

bool removePath(const std::string &path, std::error_code &error) {
  error.clear();
  // Never recursively delete a mount root, or a path whose spelling hides it.
  const auto normalized = std::filesystem::path(path).lexically_normal();
  const auto leaf = normalized.filename().string();
  if (path.empty() || leaf.empty() || leaf == "." || leaf == ".." || leaf.back() == ':') {
    error = std::make_error_code(std::errc::invalid_argument);
    return false;
  }
#ifdef __SWITCH__
  Path native;
  if (!native.translate(path, error)) return false;
  FsDirEntryType type;
  Result rc = fsFsGetEntryType(native.fs, native.name, &type);
  if (rc == NotFound) return true;
  if (!result(rc, error)) return false;
  // libstdc++ remove_all opens a directory before it knows the entry type.
  // Horizon can report PathNotFound for OpenDirectory(file), which becomes a
  // false successful no-op. For directories avoid client-side traversal and
  // deletion with directory handles still open. Let FS remove the tree.
  rc = type == FsDirEntryType_Dir
      ? fsFsDeleteDirectoryRecursively(native.fs, native.name)
      : fsFsDeleteFile(native.fs, native.name);
  if (!result(rc, error)) return false;
  rc = fsFsGetEntryType(native.fs, native.name, &type);
  if (rc == NotFound) return true;
  if (!result(rc, error)) return false;
#else
  std::filesystem::remove_all(path, error);
  if (error) return false;
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory ||
      (!error && status.type() == std::filesystem::file_type::not_found)) {
    error.clear();
    return true;
  }
  if (error) return false;
#endif
  error = std::make_error_code(std::errc::device_or_resource_busy);
  return false;
}

bool renamePath(const std::string &from, const std::string &to, std::error_code &error) {
  error.clear();
#ifdef __SWITCH__
  Path source, target;
  if (!source.translate(from, error) || !target.translate(to, error)) return false;
  if (source.fs != target.fs) {
    error = std::make_error_code(std::errc::cross_device_link);
    return false;
  }
  FsDirEntryType type;
  if (!result(fsFsGetEntryType(source.fs, source.name, &type), error)) return false;
  return result(type == FsDirEntryType_Dir
      ? fsFsRenameDirectory(source.fs, source.name, target.name)
      : fsFsRenameFile(source.fs, source.name, target.name), error);
#else
  std::filesystem::rename(from, to, error);
  return !error;
#endif
}
}
