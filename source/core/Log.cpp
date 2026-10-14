#include "core/Json.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cctype>
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
// Past this the log keeps only its newest part, so it never grows for good.
constexpr uintmax_t MaxLogBytes = 512 * 1024, KeptLogBytes = 128 * 1024;
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
  if (std::filesystem::file_size(path, error) > MaxLogBytes && !error) {
    auto text = readFile(path);
    text.erase(0, text.size() - std::min<size_t>(text.size(), KeptLogBytes));
    const auto line = text.find('\n');
    if (line != std::string::npos) text.erase(0, line + 1);
    std::string ignored;
    if (!atomicWrite(path, text, ignored)) std::filesystem::remove(path, error);
  }
  makeDirectories(std::filesystem::path(std::filesystem::path(path).parent_path()).string());
  logPath = path;
}
namespace {
bool tokenChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '+' || c == '/' || c == '=';
}
// What follows a credential's name, and anything shaped like a signed token
// (eyJ...), reads as "***": a bug report never carries a key or a password.
std::string withoutSecrets(std::string line) {
  for (const char *lead : {"Bearer ", "Basic ", "X-Access-Key: ", "access_token", "refresh_token", "password", "accessKey"}) {
    for (size_t at = line.find(lead); at != std::string::npos; at = line.find(lead, at)) {
      size_t from = at + std::char_traits<char>::length(lead);
      while (from < line.size() && (line[from] == '"' || line[from] == ':' || line[from] == '=' || line[from] == ' ')) ++from;
      size_t to = from;
      while (to < line.size() && tokenChar(line[to])) ++to;
      if (to > from) line.replace(from, to - from, "***");
      at = from;
    }
  }
  for (size_t at = line.find("eyJ"); at != std::string::npos; at = line.find("eyJ", at + 3)) {
    size_t to = at;
    while (to < line.size() && tokenChar(line[to])) ++to;
    if (std::count(line.begin() + long(at), line.begin() + long(to), '.') >= 2) line.replace(at, to - at, "***");
  }
  return line;
}
} // namespace
std::string withoutAddresses(const std::string &line) {
  std::string out;
  out.reserve(line.size());
  size_t at = 0;
  while (at < line.size()) {
    const auto http = line.find("http", at);
    if (http == std::string::npos) break;
    const auto scheme = line.compare(http, 8, "https://") == 0 ? 8 : line.compare(http, 7, "http://") == 0 ? 7 : 0;
    if (!scheme) {
      out.append(line, at, http + 4 - at);
      at = http + 4;
      continue;
    }
    out.append(line, at, http - at);
    // The address runs to the next space or quote (file names hold brackets).
    const auto end = std::min(line.size(), line.find_first_of(" \t\"'<>", http));
    const auto slash = line.find('/', http + scheme);
    std::string path = slash != std::string::npos && slash < end ? line.substr(slash, end - slash) : "/";
    path = path.substr(0, path.find_first_of("?#"));
    out += path;
    at = end;
  }
  out.append(line, std::min(at, line.size()), std::string::npos);
  // curl names the host it could not reach.
  if (const auto host = out.find("resolve host: "); host != std::string::npos)
    out = out.substr(0, host + 14) + "(server)";
  return withoutSecrets(out);
}
void logLine(const std::string &raw) {
  const auto line = withoutAddresses(raw);
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
  // Lines written before addresses were left out lose theirs here.
  std::string clean;
  size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto stop = end == std::string::npos ? text.size() : end;
    clean += withoutAddresses(text.substr(start, stop - start));
    if (end != std::string::npos) clean += '\n';
    start = stop + 1;
  }
  return clean;
}
} // namespace foil
