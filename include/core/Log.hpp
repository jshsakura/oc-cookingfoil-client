#pragma once
#include <exception>
#include <string>
#include <vector>
namespace foil {
// Appends to the log file opened by openLog. No-op until then.
void openLog(const std::string &path);
// What the log keeps, chosen in 설정: nothing, errors (the default), or
// every step too.
enum class LogLevel { Off, Errors, All };
void setLogLevel(LogLevel level);
// Queued and written by a thread of its own, so it never waits for the card.
// An error or a fact a bug report needs; dropped only at Off.
void logLine(const std::string &line);
// A step that went as planned (an MTP operation, a download back on the
// network); written only at All.
void logDetail(const std::string &line);
// Waits until every line logged so far is in the file (a report, a test).
void flushLog();
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
