#pragma once
#include <exception>
#include <string>
namespace foil {
// Appends to the log file opened by openLog. No-op until then.
void openLog(const std::string &path);
void logLine(const std::string &line);
// The last `maxBytes` of the log, from a line start; empty with no log.
std::string logTail(size_t maxBytes);
// Runs work on a worker thread. An escaped exception would terminate the app,
// so it is logged and reported as false instead.
template <class Work> bool guarded(const char *where, Work &&work) {
  try {
    work();
    return true;
  } catch (const std::exception &error) {
    logLine(std::string(where) + ": " + error.what());
  } catch (...) {
    logLine(std::string(where) + ": unknown exception");
  }
  return false;
}
} // namespace foil
