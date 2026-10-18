#pragma once
#include <exception>
#include <string>
#include <vector>
namespace foil {
// Appends to the log file opened by openLog. No-op until then.
void openLog(const std::string &path);
void logLine(const std::string &line);
// A line as the log keeps it: no server addresses. An address's scheme and
// host and its query (signatures, keys) go; the path stays, to tell what it
// was ("/api/homebrew/x/bundle"). Keys, passwords and tokens read as "***".
// The log goes out in bug reports.
std::string withoutAddresses(const std::string &line);
// What a bug report must not carry beyond what withoutAddresses drops: the
// server's host names, the account's name, and secrets known by value.
struct ReportPrivate {
  std::vector<std::string> hosts, names, secrets;
};
// The report as it leaves the console: every line without addresses, the
// known hosts as "(server)", names as "(user)", secrets as "***", and on any
// line IPv4 addresses, e-mail addresses, home folders' user names and long hex
// (keys, hashes; the first 8 stay) masked too. Versions, firmware, error lines
// and Title IDs stay as they were.
std::string forReport(const std::string &text, const ReportPrivate &known);
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
