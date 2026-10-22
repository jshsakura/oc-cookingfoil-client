#include "core/Json.hpp"
#include "core/Log.hpp"
#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <regex>
#include <thread>
#ifdef __SWITCH__
#include <switch.h>
#include <malloc.h>
#endif
namespace foil {
namespace {
// Past this the log keeps only its newest part, so it never grows for good.
constexpr uintmax_t MaxLogBytes = 256 * 1024, KeptLogBytes = 64 * 1024;
std::mutex mutex;
std::string logPath;
// What malloc hands out of what it holds. The process figure is left out:
// it includes the whole heap libnx reserves at start, so it never moves.
std::string memory() {
#ifdef __SWITCH__
  const struct mallinfo heap = mallinfo();
  return " [heap " + std::to_string(size_t(heap.uordblks) >> 20) + "/" + std::to_string(size_t(heap.arena) >> 20) +
         "MB]";
#else
  return "";
#endif
}
// A line the same as the one before is counted, not written again (a poll
// failing every few seconds): one line says how many when another comes.
std::string lastLine;
int repeats = 0;
void writeLine(FILE *file, const std::string &line) {
  char stamp[32]{};
  const auto now = std::time(nullptr);
  std::strftime(stamp, sizeof(stamp), "%m-%d %H:%M:%S", std::localtime(&now));
  std::fprintf(file, "%s %s%s\n", stamp, line.c_str(), memory().c_str());
}
// Keeps the last KeptLogBytes, from a whole line.
void trimLog(const std::string &path) {
  std::error_code error;
  auto text = readFile(path);
  text.erase(0, text.size() - std::min<size_t>(text.size(), KeptLogBytes));
  const auto line = text.find('\n');
  if (line != std::string::npos) text.erase(0, line + 1);
  std::string ignored;
  if (!atomicWrite(path, text, ignored)) std::filesystem::remove(path, error);
}
} // namespace
namespace {
void writeBatch(const std::deque<std::string> &raws);
// Lines go to the card on a thread of their own: a screen that logs never
// waits for a write. It drains in batches, each one open of the file, so
// a card that stalls for a moment costs the screen nothing.
class Writer {
public:
  ~Writer() {
    {
      std::lock_guard<std::mutex> held(lock_);
      quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void push(std::string raw) {
    std::lock_guard<std::mutex> held(lock_);
    if (lines_.size() >= MaxQueued) lines_.pop_front(); // the card is stuck: the oldest go, it never grows
    if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    lines_.push_back(std::move(raw));
    wake_.notify_one();
  }
  // Returns once every line pushed before it is on the card.
  void flush() {
    std::unique_lock<std::mutex> held(lock_);
    idle_.wait(held, [this] { return lines_.empty() && !busy_; });
  }

private:
  static constexpr size_t MaxQueued = 4096;
  void run() {
    std::unique_lock<std::mutex> held(lock_);
    for (;;) {
      wake_.wait(held, [this] { return !lines_.empty() || quit_; });
      if (lines_.empty()) return;
      std::deque<std::string> batch;
      batch.swap(lines_);
      busy_ = true;
      held.unlock();
      writeBatch(batch);
      held.lock();
      busy_ = false;
      idle_.notify_all();
    }
  }
  std::mutex lock_;
  std::condition_variable wake_, idle_;
  std::deque<std::string> lines_;
  std::thread thread_;
  bool busy_ = false, quit_ = false;
};
Writer &writer() {
  static Writer instance;
  return instance;
}
} // namespace
void flushLog() { writer().flush(); }
void openLog(const std::string &path) {
  flushLog(); // what came before belongs to the file it was meant for
  std::lock_guard<std::mutex> lock(mutex);
  std::error_code error;
  lastLine.clear();
  repeats = 0;
  if (std::filesystem::file_size(path, error) > MaxLogBytes && !error) trimLog(path);
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
  for (const char *lead : {"Bearer ", "Basic ", "X-Access-Key: ", "X-Device-Key: ", "X-Pair-Secret: ", "access_token",
                           "refresh_token", "password", "accessKey", "enrollToken", "pairSecret", "token=", "\"token\"",
                           "Secret"}) {
    for (size_t at = line.find(lead); at != std::string::npos; at = line.find(lead, at)) {
      size_t from = at + std::char_traits<char>::length(lead);
      while (from < line.size() && (line[from] == '"' || line[from] == ':' || line[from] == '=' || line[from] == ' ')) ++from;
      size_t to = from;
      while (to < line.size() && tokenChar(line[to])) ++to;
      if (to > from) line.replace(from, to - from, "***");
      at = from;
    }
  }
  // A session token (cfs_…) or an enrollment token (cfe_…) wherever it shows.
  for (const char *mark : {"cfs_", "cfe_"})
    for (size_t at = line.find(mark); at != std::string::npos; at = line.find(mark, at + 3)) {
      size_t to = at + 4;
      while (to < line.size() && tokenChar(line[to])) ++to;
      if (to > at + 4) line.replace(at, to - at, "***");
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
namespace {
void replaceAll(std::string &text, const std::string &what, const std::string &with) {
  if (what.empty()) return;
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + with.size()))
    text.replace(at, what.size(), with);
}
// `what` where it stands as a word of its own (not inside a longer name).
void replaceWord(std::string &text, const std::string &what, const std::string &with) {
  const auto inWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
  for (size_t at = text.find(what); at != std::string::npos; at = text.find(what, at)) {
    const size_t end = at + what.size();
    if ((at > 0 && inWord(text[at - 1])) || (end < text.size() && inWord(text[end]))) {
      at = end;
      continue;
    }
    text.replace(at, what.size(), with);
    at += with.size();
  }
}
} // namespace
std::string forReport(const std::string &text, const ReportPrivate &known) {
  std::string out;
  size_t start = 0;
  while (start < text.size()) {
    const auto end = text.find('\n', start);
    const auto stop = end == std::string::npos ? text.size() : end;
    out += withoutAddresses(text.substr(start, stop - start));
    if (end != std::string::npos) out += '\n';
    start = stop + 1;
  }
  // Known values first, whole: a secret never leaves even its first 8.
  for (const auto &secret : known.secrets)
    if (secret.size() >= 6) replaceAll(out, secret, "***");
  for (const auto &host : known.hosts)
    if (host.size() >= 3) replaceAll(out, host, "(server)");
  for (const auto &name : known.names)
    if (name.size() >= 3) replaceWord(out, name, "(user)");
  static const std::regex ipv4(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)");
  static const std::regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
  static const std::regex home(R"(((?:/home|/Users|[A-Za-z]:\\Users)[/\\])[^/\\\s"']+)");
  static const std::regex longHex(R"(\b([0-9A-Fa-f]{8})[0-9A-Fa-f]{32,}\b)");
  out = std::regex_replace(out, email, "(email)");
  out = std::regex_replace(out, ipv4, "(ip)");
  out = std::regex_replace(out, home, "$1(user)");
  return std::regex_replace(out, longHex, "$1...");
}
namespace {
void writeBatch(const std::deque<std::string> &raws) {
  std::lock_guard<std::mutex> lock(mutex);
  if (logPath.empty())
    return;
  FILE *file = nullptr;
  for (const auto &raw : raws) {
    auto line = withoutAddresses(raw);
    if (line == lastLine) {
      ++repeats;
      continue;
    }
    if (!file && !(file = std::fopen(logPath.c_str(), "a")))
      return;
    if (repeats)
      writeLine(file, "(the line before " + std::to_string(repeats) + " more times)");
    repeats = 0;
    writeLine(file, line);
    lastLine = std::move(line);
  }
  if (!file)
    return;
  const long size = std::ftell(file);
  std::fclose(file);
  // A long session (or something logging in a loop) is cut back here too,
  // not only at the next start: the log never grows past twice its size.
  if (size > long(2 * MaxLogBytes)) trimLog(logPath);
}
} // namespace
void logLine(const std::string &raw) { writer().push(raw); }
std::string logTail(size_t maxBytes) {
  flushLog();
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
  // A line still repeating when the report is made says so too.
  if (repeats) clean += "(the line before " + std::to_string(repeats) + " more times so far)\n";
  return clean;
}
} // namespace foil
