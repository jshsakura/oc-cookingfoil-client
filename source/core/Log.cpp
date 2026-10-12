#include "core/Json.hpp"
#include "core/Log.hpp"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>
#endif
namespace foil {
namespace {
constexpr uintmax_t MaxLogBytes = 256 * 1024;
std::mutex mutex;
std::string logPath;
std::string memory() {
#ifdef __SWITCH__
  u64 used = 0, total = 0;
  if (R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) &&
      R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)))
  {
    // The process figure includes the whole heap libnx reserves at start,
    // so it never moves; what malloc actually hands out is the useful part.
    const struct mallinfo heap = mallinfo();
    return " [heap " + std::to_string(size_t(heap.uordblks) >> 20) + "/" +
           std::to_string(size_t(heap.arena) >> 20) + "MB, process " + std::to_string(used >> 20) + "/" +
           std::to_string(total >> 20) + "MB]";
  }
#endif
  return "";
}
} // namespace
void openLog(const std::string &path) {
  std::lock_guard<std::mutex> lock(mutex);
  std::error_code error;
  if (std::filesystem::file_size(path, error) > MaxLogBytes && !error)
    std::filesystem::remove(path, error);
  makeDirectories(std::filesystem::path(std::filesystem::path(path).parent_path()).string());
  logPath = path;
}
void logLine(const std::string &line) {
  std::lock_guard<std::mutex> lock(mutex);
  if (logPath.empty())
    return;
  if (auto *file = std::fopen(logPath.c_str(), "a")) {
    char stamp[32]{};
    const auto now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(file, "%s %s%s\n", stamp, line.c_str(), memory().c_str());
    std::fclose(file);
  }
}
std::string logTail(size_t maxBytes) {
  std::lock_guard<std::mutex> lock(mutex);
  if (logPath.empty()) return {};
  auto text = readFile(logPath);
  if (text.size() > maxBytes) {
    text.erase(0, text.size() - maxBytes);
    const auto line = text.find('\n');
    if (line != std::string::npos) text.erase(0, line + 1);
  }
  return text;
}
} // namespace foil
