#include "core/DownloadStore.hpp"
#include "core/State.hpp"
#include "core/Json.hpp"
#include "core/Sha256.hpp"
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>
#include <mutex>
#include <map>
namespace foil {
namespace {
// Schema versions, in PRAGMA user_version. Each step runs once, in order.
const char *const Migrations[] = {
    // 1: the list and the resume points.
    "CREATE TABLE downloads("
    "  id INTEGER PRIMARY KEY,"
    "  position INTEGER NOT NULL,"
    "  kind TEXT NOT NULL DEFAULT '',"
    "  outcome TEXT NOT NULL,"
    "  grp INTEGER NOT NULL DEFAULT 0,"
    "  install TEXT NOT NULL,"
    "  payload TEXT NOT NULL DEFAULT '',"
    "  error TEXT NOT NULL DEFAULT '',"
    "  at INTEGER NOT NULL DEFAULT 0,"
    "  started_at INTEGER NOT NULL DEFAULT 0,"
    "  bytes INTEGER NOT NULL DEFAULT 0,"
    "  download_seconds REAL NOT NULL DEFAULT 0,"
    "  install_seconds REAL NOT NULL DEFAULT 0);"
    "CREATE INDEX downloads_position ON downloads(position);"
    "CREATE TABLE resume_points("
    "  key TEXT PRIMARY KEY,"
    "  url TEXT NOT NULL,"
    "  etag TEXT NOT NULL,"
    "  total INTEGER NOT NULL,"
    "  plan TEXT NOT NULL,"
    "  updated_at INTEGER NOT NULL);",
    // 2: the app's state, from state.json (State.cpp moves it in).
    "CREATE TABLE settings("
    "  key TEXT PRIMARY KEY,"
    "  value TEXT NOT NULL);"
    "CREATE TABLE pair_notes("
    "  server TEXT PRIMARY KEY,"
    "  note TEXT NOT NULL);"
    "CREATE TABLE credentials("
    "  kind TEXT NOT NULL,"
    "  server TEXT NOT NULL DEFAULT '',"
    "  value TEXT NOT NULL,"
    "  PRIMARY KEY(kind, server));",
    // 3: a server's screen text per language (GET /api/client/strings).
    "CREATE TABLE client_strings("
    "  lang TEXT PRIMARY KEY,"
    "  server TEXT NOT NULL,"
    "  version TEXT NOT NULL,"
    "  etag TEXT NOT NULL,"
    "  body TEXT NOT NULL);",
    // 4: the languages a server's dictionary has, to name them before it answers.
    "CREATE TABLE client_languages("
    "  code TEXT PRIMARY KEY,"
    "  name TEXT NOT NULL,"
    "  base TEXT NOT NULL);",
};
std::atomic<bool> setAside{false};
const char *outcomeName(history::Outcome outcome) {
  switch (outcome) {
  case history::Outcome::Done: return "done";
  case history::Outcome::Failed: return "failed";
  case history::Outcome::Cancelled: return "cancelled";
  case history::Outcome::Queued: return "queued";
  case history::Outcome::Running: break;
  }
  return "running";
}
history::Outcome outcomeOf(const std::string &text) {
  return text == "done"        ? history::Outcome::Done
         : text == "failed"    ? history::Outcome::Failed
         : text == "cancelled" ? history::Outcome::Cancelled
         : text == "queued"    ? history::Outcome::Queued
                               : history::Outcome::Running;
}
// A prepared statement that finalizes itself on every path. A bind that
// fails (no memory, too big) makes the next step() fail with it, so a caller
// that checks step() never writes a row with a value missing.
class Statement {
public:
  Statement(sqlite3 *db, const char *sql) {
    if (sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
      sqlite3_finalize(stmt_);
      stmt_ = nullptr;
    }
  }
  ~Statement() { sqlite3_finalize(stmt_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  explicit operator bool() const { return stmt_ != nullptr; }
  sqlite3_stmt *get() const { return stmt_; }
  // Text with its length, so a plan of hundreds of KB goes in whole.
  void text(int at, const std::string &value) {
    bound(sqlite3_bind_text64(stmt_, at, value.data(), sqlite3_uint64(value.size()), SQLITE_TRANSIENT, SQLITE_UTF8));
  }
  void integer(int at, int64_t value) { bound(sqlite3_bind_int64(stmt_, at, value)); }
  void real(int at, double value) { bound(sqlite3_bind_double(stmt_, at, value)); }
  std::string column(int at) const {
    const auto *value = sqlite3_column_text(stmt_, at);
    return value ? std::string(reinterpret_cast<const char *>(value), size_t(sqlite3_column_bytes(stmt_, at))) : "";
  }
  int step() { return bindError_ != SQLITE_OK ? bindError_ : sqlite3_step(stmt_); }
  void reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
    bindError_ = SQLITE_OK;
  }

private:
  void bound(int rc) {
    if (rc != SQLITE_OK && bindError_ == SQLITE_OK)
      bindError_ = rc;
  }
  sqlite3_stmt *stmt_ = nullptr;
  int bindError_ = SQLITE_OK;
};
// The file itself is bad (not a database, or damaged), as opposed to an
// error of the moment (the card, memory) that says nothing about the file.
bool damaged(int rc) {
  rc &= 0xff;
  return rc == SQLITE_NOTADB || rc == SQLITE_CORRUPT;
}
const int Latest = int(sizeof(Migrations) / sizeof(Migrations[0]));
const char *const NewerBuild = "cookingfoil.db is from a newer version; kept as it is";
const char *const Busy = "cookingfoil.db is in use by another CookingFoil";
bool exec(sqlite3 *db, const char *sql, std::string &error) {
  char *message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) == SQLITE_OK)
    return true;
  error = message ? message : sqlite3_errmsg(db);
  sqlite3_free(message);
  return false;
}
// Ends a transaction a failed write left open; SQLite may have rolled it back
// itself already (a full card, an I/O error), and then this does nothing.
void rollback(sqlite3 *db) {
  if (db && !sqlite3_get_autocommit(db))
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
}
// close_v2: a statement still open (a failed check's) does not keep the file
// open; it closes once that statement goes. The console cannot rename an
// open file, so a damaged one could never be set aside with plain close.
void closeDb(sqlite3 *&db) {
  if (db) {
    rollback(db);
    sqlite3_close_v2(db);
  }
  db = nullptr;
}
// Everything a row holds, as one string: a row is written only when the file
// holds something else.
std::string fingerprint(int64_t position, const std::string &kind, const std::string &outcome, int64_t group,
                        const std::string &install, const std::string &payload, const std::string &error, int64_t at,
                        int64_t startedAt, int64_t bytes, double downloadSeconds, double installSeconds) {
  return std::to_string(position) + '\n' + kind + '\n' + outcome + '\n' + std::to_string(group) + '\n' + install + '\n' +
         payload + '\n' + error + '\n' + std::to_string(at) + '\n' + std::to_string(startedAt) + '\n' +
         std::to_string(bytes) + '\n' + std::to_string(downloadSeconds) + '\n' + std::to_string(installSeconds);
}
const char *const SelectRows = "SELECT id, position, kind, outcome, grp, install, payload, error, at, started_at, bytes,"
                               " download_seconds, install_seconds FROM downloads";
std::string fingerprint(const Statement &row) {
  sqlite3_stmt *s = row.get();
  return fingerprint(sqlite3_column_int64(s, 1), row.column(2), row.column(3), sqlite3_column_int64(s, 4), row.column(5),
                     row.column(6), row.column(7), sqlite3_column_int64(s, 8), sqlite3_column_int64(s, 9),
                     sqlite3_column_int64(s, 10), sqlite3_column_double(s, 11), sqlite3_column_double(s, 12));
}
// The lock between instances: a file made with O_EXCL, so only one can make
// it, holding the time it was taken. It is let go only by whoever holds it,
// so an instance that wakes after its lock was taken over removes nothing.
constexpr auto LockWait = std::chrono::milliseconds(2000);
constexpr auto LockRetry = std::chrono::milliseconds(20);
constexpr long LockStaleSeconds = 10;
class FileLock {
public:
  explicit FileLock(std::string path) : path_(std::move(path)) {}
  ~FileLock() { release(); }
  FileLock(const FileLock &) = delete;
  FileLock &operator=(const FileLock &) = delete;
  bool acquire(std::string &error) {
    token_ = std::to_string(static_cast<long long>(std::time(nullptr))) + ' ' +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ' ' +
             std::to_string(reinterpret_cast<uintptr_t>(this)) + '\n';
    const auto deadline = std::chrono::steady_clock::now() + LockWait;
    for (;;) {
      const int fd = ::open(path_.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
      if (fd >= 0) {
        const bool written = ::write(fd, token_.data(), token_.size()) == ssize_t(token_.size());
        ::close(fd);
        held_ = true;
        if (written)
          return true;
        release();
        error = "cannot write " + path_;
        return false;
      }
      if (errno != EEXIST) {
        error = path_ + ": " + std::strerror(errno);
        return false;
      }
      // The owner died, or sleeps in the background: taken over. Read again
      // just before removing, so a lock someone else took meanwhile stays.
      const auto seen = contents();
      if (stale(seen) && contents() == seen) {
        ::unlink(path_.c_str());
        continue;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        error = Busy;
        return false;
      }
      std::this_thread::sleep_for(LockRetry);
    }
  }
  // Still ours: nobody took it over while this instance was suspended.
  bool owned() const { return held_ && contents() == token_; }
  void release() {
    if (owned())
      ::unlink(path_.c_str());
    held_ = false;
  }

private:
  std::string contents() const {
    std::string out;
    if (FILE *f = std::fopen(path_.c_str(), "rb")) {
      char buffer[128];
      const size_t got = std::fread(buffer, 1, sizeof(buffer), f);
      out.assign(buffer, got);
      std::fclose(f);
    }
    return out;
  }
  // By the time written inside; by the file's time only when there is none
  // (an owner that died between making the file and writing it).
  bool stale(const std::string &seen) const {
    const long long now = std::time(nullptr);
    char *end = nullptr;
    const long long taken = std::strtoll(seen.c_str(), &end, 10);
    if (end != seen.c_str() && *end == ' ')
      return now - taken > LockStaleSeconds || taken - now > LockStaleSeconds;
    struct stat info;
    if (::stat(path_.c_str(), &info) != 0)
      return false; // gone already: the next try makes it
    return now - (long long)info.st_mtime > LockStaleSeconds;
  }
  std::string path_, token_;
  bool held_ = false;
};
} // namespace
bool DownloadStore::busy(const std::string &error) { return error == Busy; }
DownloadStore::~DownloadStore() { close(); }
std::string DownloadStore::fileName(const std::string &path) {
  // SQLite takes a path without "/" at its start as relative and puts the
  // working folder in front, so "sdmc:/…" never opened on the console. There
  // "/…" is the SD card already.
  return path.rfind("sdmc:/", 0) == 0 ? path.substr(5) : path;
}
// Not under the lock: a save holds it for as long as the card takes to
// write, and the screens ask this before every save.
bool DownloadStore::isOpen() const { return open_; }
bool DownloadStore::readOnly() const { return readOnly_; }
void DownloadStore::close() {
  std::lock_guard<std::mutex> held(lock_);
  file_.clear();
  open_ = false;
  readOnly_ = false;
  known_.clear();
}
// The database in memory, written to `file` whole.
// `was`: the file as it was read for this change. A change that left every
// byte the same (a row written as it already was, one forgotten that was
// never there) writes nothing: each write rewrites the whole file on the
// card, so one that changes nothing only wears it.
// What quick_check last passed for a file (or the app last wrote to it), as
// a SHA-256 of its bytes: the same bytes read again need no check, and only
// 32 bytes stay in memory. The file is still read from the card each time,
// so a change by another instance is always seen.
std::mutex checkedLock;
std::map<std::string, std::string> checkedFiles;
// KeepReads: the bytes last read or written per file, with the size and
// time the card gave for them then.
struct Kept {
  std::string bytes;
  long long size = -1;
  long long time = 0;
};
std::mutex keptLock;
int keeping = 0;
std::map<std::string, Kept> keptFiles;
bool cardStamp(const std::string &file, long long &size, long long &time) {
  struct stat info {};
  if (::stat(file.c_str(), &info) != 0) return false;
  size = (long long)info.st_size;
  time = (long long)info.st_mtime;
  return true;
}
void keepCopy(const std::string &file, std::string bytes) {
  std::lock_guard<std::mutex> held(keptLock);
  if (!keeping) return;
  Kept kept;
  if (!cardStamp(file, kept.size, kept.time)) {
    keptFiles.erase(file);
    return;
  }
  kept.bytes = std::move(bytes);
  keptFiles[file] = std::move(kept);
}
// The kept copy, in memory SQLite owns, when the card still has that file.
unsigned char *keptCopy(const std::string &file, size_t &size) {
  std::lock_guard<std::mutex> held(keptLock);
  const auto found = keptFiles.find(file);
  if (!keeping || found == keptFiles.end()) return nullptr;
  long long now = -1, time = 0;
  if (!cardStamp(file, now, time) || now != found->second.size || time != found->second.time ||
      found->second.bytes.empty()) {
    keptFiles.erase(found);
    return nullptr;
  }
  auto *out = static_cast<unsigned char *>(sqlite3_malloc64(found->second.bytes.size()));
  if (!out) return nullptr;
  std::memcpy(out, found->second.bytes.data(), found->second.bytes.size());
  size = found->second.bytes.size();
  return out;
}
std::string digest(const char *data, size_t size) {
  Sha256 hash;
  hash.update(data, size);
  return hash.hex();
}
std::string digest(const std::string &bytes) { return digest(bytes.data(), bytes.size()); }
// The whole file straight into memory SQLite owns (no second copy); null
// with size 0 when there is none or it cannot be read.
unsigned char *readWhole(const std::string &file, size_t &size) {
  size = 0;
  if (auto *kept = keptCopy(file, size)) return kept;
  FILE *in = std::fopen(file.c_str(), "rb");
  if (!in) return nullptr;
  unsigned char *out = nullptr;
  if (std::fseek(in, 0, SEEK_END) == 0) {
    const long length = std::ftell(in);
    if (length > 0 && std::fseek(in, 0, SEEK_SET) == 0 &&
        (out = static_cast<unsigned char *>(sqlite3_malloc64(sqlite3_uint64(length))))) {
      if (std::fread(out, 1, size_t(length), in) == size_t(length)) size = size_t(length);
      else {
        sqlite3_free(out);
        out = nullptr;
      }
    }
  }
  std::fclose(in);
  if (out) keepCopy(file, std::string(reinterpret_cast<const char *>(out), size));
  return out;
}
bool alreadyChecked(const std::string &file, const std::string &sum) {
  std::lock_guard<std::mutex> held(checkedLock);
  const auto found = checkedFiles.find(file);
  return !sum.empty() && found != checkedFiles.end() && found->second == sum;
}
void rememberChecked(const std::string &file, const std::string &sum) {
  std::lock_guard<std::mutex> held(checkedLock);
  if (sum.empty()) checkedFiles.erase(file);
  else checkedFiles[file] = sum;
}
bool persist(sqlite3 *db, const std::string &file, std::string &error, const std::string *was = nullptr) {
  sqlite3_int64 size = 0;
  unsigned char *bytes = sqlite3_serialize(db, "main", &size, 0);
  if (!bytes) {
    error = "could not serialize the database";
    return false;
  }
  std::string written(reinterpret_cast<const char *>(bytes), size_t(size));
  sqlite3_free(bytes);
  // Nothing changed: the file is left as it is (no write at all).
  const auto sum = digest(written);
  if (was && *was == sum) return true;
  const bool ok = atomicWrite(file, written, error);
  // Made by the app from a database that was checked: no check again for it.
  rememberChecked(file, ok ? sum : std::string());
  if (ok) keepCopy(file, std::move(written));
  else keepCopy(file, {}); // unknown now: read from the card next time
  return ok;
}
// Opens the file and brings it up to date; `check` also runs quick_check.
DownloadStore::Opened DownloadStore::connect(const std::string &file, sqlite3 *&db, bool check, bool &newer,
                                             std::string &error, std::string *read, bool mayWrite) {
  auto failed = [&](int rc) {
    if (error.empty())
      error = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    closeDb(db);
    return damaged(rc) ? Opened::Broken : Opened::Failed;
  };
  error.clear();
  newer = false;
  // The file is read whole into a database in memory and written back whole
  // (atomicWrite: a temporary file, then a rename) after each change. The
  // console's card did not keep SQLite's own file and journal intact (every
  // start found it malformed, and a fresh one too); a whole-file write is
  // what the app's other files use, and it holds there.
  const int opened = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (opened != SQLITE_OK)
    return failed(opened);
  sqlite3_extended_result_codes(db, 1);
  bool checked = false;
  std::string known; // the bytes' digest, when checking
  {
    size_t size = 0;
    unsigned char *bytes = readWhole(file, size);
    if (!bytes)
      bytes = readWhole(file + ".old", size); // a write cut off between its two renames
    const auto *chars = reinterpret_cast<const char *>(bytes);
    // What was read, as a digest: a write of the same bytes is skipped.
    if (read)
      *read = bytes ? digest(chars, size) : std::string();
    if (check && bytes) {
      known = read ? *read : digest(chars, size);
      checked = alreadyChecked(file, known);
    }
    if (bytes) {
      const int rc = sqlite3_deserialize(db, "main", bytes, sqlite3_int64(size), sqlite3_int64(size),
                                         SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_RESIZEABLE);
      if (rc != SQLITE_OK)
        return failed(rc);
    }
  }
  // secure_delete: a value replaced (a key encrypted, a link forgotten) is
  // zeroed where it was, not left in a freed page for a PC to read.
  if (!exec(db, "PRAGMA temp_store=MEMORY; PRAGMA secure_delete=ON;", error))
    return failed(sqlite3_errcode(db));
  if (check && !checked) {
    Statement quick(db, "PRAGMA quick_check");
    if (!quick)
      return failed(sqlite3_errcode(db));
    const int rc = quick.step();
    if (rc != SQLITE_ROW)
      return failed(rc == SQLITE_DONE ? SQLITE_CORRUPT : rc);
    if (quick.column(0) != "ok") {
      error = "quick_check: " + quick.column(0);
      closeDb(db);
      return Opened::Broken;
    }
    rememberChecked(file, known);
  }
  int version = 0;
  {
    Statement read(db, "PRAGMA user_version");
    if (!read)
      return failed(sqlite3_errcode(db));
    const int rc = read.step();
    if (rc != SQLITE_ROW)
      return failed(rc);
    version = sqlite3_column_int(read.get(), 0);
  }
  // A newer build's file (or a number this code never wrote): its tables may
  // hold what this build does not know, so it is read as it is and never
  // migrated, written or set aside.
  if (version < 0 || version > Latest) {
    newer = true;
    if (!exec(db, "PRAGMA query_only=1;", error))
      return failed(sqlite3_errcode(db));
    return Opened::Ok;
  }
  if (version < Latest && !mayWrite) {
    closeDb(db);
    return Opened::Unwritten;
  }
  // Each step and its version number commit together, or not at all; the
  // file is written once they all have.
  for (int step = version; step < Latest; ++step) {
    const std::string sql = std::string("BEGIN IMMEDIATE;") + Migrations[step] + "PRAGMA user_version=" +
                            std::to_string(step + 1) + ";COMMIT;";
    if (!exec(db, sql.c_str(), error)) {
      // The file passed its check, so this is not the file's fault.
      closeDb(db);
      return Opened::Failed;
    }
  }
  if (version < Latest && !persist(db, file, error)) {
    closeDb(db);
    return Opened::Failed;
  }
  return Opened::Ok;
}
namespace {
using Points = std::map<std::string, DownloadStore::ResumePoint>;
// The file: {"points": {key: point}, "made": [key]}. "made" is every key a
// point was saved for, the console placeholders this app made; it outlives
// the point, so a placeholder whose point was dropped is still known as ours
// and is cleared, and the console's own downloads (eShop) are never touched.
// 0.3.97 wrote the points alone, as the whole object.
struct ResumeFile {
  Points points;
  std::vector<std::string> made;
};
DownloadStore::ResumePoint pointOf(json_object *value) {
  return {stringField(value, "url"), stringField(value, "etag"), uint64_t(integerField(value, "total")),
          stringField(value, "plan")};
}
ResumeFile parseResume(const std::string &text) {
  ResumeFile out;
  Error error;
  const auto root = parseJson(text, error);
  if (error || !json_object_is_type(root.get(), json_type_object))
    return out;
  json_object *points = nullptr, *made = nullptr;
  const bool split = json_object_object_get_ex(root.get(), "points", &points);
  json_object_object_foreach(split ? points : root.get(), key, value)
    if (json_object_is_type(value, json_type_object))
      out.points[key] = pointOf(value);
  if (json_object_object_get_ex(root.get(), "made", &made) && json_object_is_type(made, json_type_array))
    for (size_t i = 0; i < json_object_array_length(made); ++i)
      if (auto *one = json_object_array_get_idx(made, i); json_object_is_type(one, json_type_string))
        out.made.push_back(json_object_get_string(one));
  // Every point's placeholder is ours too (a 0.3.97 file has no list).
  for (const auto &[key, point] : out.points)
    if (std::find(out.made.begin(), out.made.end(), key) == out.made.end())
      out.made.push_back(key);
  return out;
}
std::string serializeResume(const ResumeFile &file) {
  Json root(json_object_new_object());
  auto *points = json_object_new_object();
  for (const auto &[key, point] : file.points) {
    auto *one = json_object_new_object();
    json_object_object_add(one, "url", json_object_new_string(point.url.c_str()));
    json_object_object_add(one, "etag", json_object_new_string(point.etag.c_str()));
    json_object_object_add(one, "total", json_object_new_int64(int64_t(point.total)));
    json_object_object_add(one, "plan", json_object_new_string(point.plan.c_str()));
    json_object_object_add(points, key.c_str(), one);
  }
  json_object_object_add(root.get(), "points", points);
  auto *made = json_object_new_array();
  for (const auto &key : file.made)
    json_object_array_add(made, json_object_new_string(key.c_str()));
  json_object_object_add(root.get(), "made", made);
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
Points legacyPoints(sqlite3 *db) {
  Points out;
  Statement read(db, "SELECT key, url, etag, total, plan FROM resume_points");
  if (!read)
    return out;
  while (read.step() == SQLITE_ROW)
    out[read.column(0)] = {read.column(1), read.column(2), uint64_t(sqlite3_column_int64(read.get(), 3)), read.column(4)};
  return out;
}
} // namespace
bool DownloadStore::open(const std::string &path, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  file_.clear();
  open_ = false;
  readOnly_ = false;
  known_.clear();
  const std::string file = fileName(path);
  sqlite3 *db = nullptr;
  bool newer = false;
  // Most opens only read and check: that needs no lock, so one an earlier run
  // left behind (closed from HOME mid-write) is not waited out at the start.
  // Anything to write (an older file, legacy rows, a damaged one) takes the
  // lock below and is done there.
  if (connect(file, db, true, newer, error, nullptr, false) == Opened::Ok && (newer || legacyPoints(db).empty())) {
    closeDb(db);
    std::remove((file + "-journal").c_str());
    file_ = file;
    open_ = true;
    readOnly_ = newer;
    return true;
  }
  closeDb(db);
  error.clear();
  FileLock lock(file + ".lock");
  if (!lock.acquire(error))
    return false;
  const auto first = connect(file, db, true, newer, error);
  // Resume points kept in the database by older builds move to their own
  // file (resumeFile, <name>.resume.json), then leave the database: written here once.
  if (first == Opened::Ok && !newer) {
    file_ = file;
    open_ = true;
    const auto legacy = legacyPoints(db);
    if (!legacy.empty()) {
      auto resume = parseResume(readFile(resumeFile()));
      for (const auto &[key, point] : legacy) {
        resume.points.emplace(key, point); // the file's own, newer, wins
        if (std::find(resume.made.begin(), resume.made.end(), key) == resume.made.end()) resume.made.push_back(key);
      }
      std::string failure;
      if (atomicWrite(resumeFile(), serializeResume(resume), failure) &&
          exec(db, "DELETE FROM resume_points;", failure))
        persist(db, file, failure);
    }
    file_.clear();
    open_ = false;
  }
  closeDb(db);
  if (first == Opened::Failed)
    return false; // an error of the moment leaves the file alone
  if (first == Opened::Broken) {
    // Damaged: kept as .broken (an older one replaced) for whoever wants to
    // look, and a fresh one made so the app goes on.
    const std::string reason = error;
    const std::string aside = file + ".broken";
    if (!replaceFile(file, aside, error)) {
      error = reason + "; cannot set it aside";
      return false;
    }
    std::remove((file + ".old").c_str());
    setAside = true;
    const auto fresh = connect(file, db, true, newer, error);
    closeDb(db);
    if (fresh != Opened::Ok) {
      error = reason + "; a fresh one failed too: " + error;
      return false;
    }
  }
  // Left by builds that let SQLite write the file itself: nothing reads it now.
  std::remove((file + "-journal").c_str());
  file_ = file;
  open_ = true;
  readOnly_ = newer;
  return true;
}
bool DownloadStore::run(bool writes, std::string &error, const std::function<bool(sqlite3 *, std::string &)> &work) {
  error.clear();
  if (file_.empty()) {
    error = "closed";
    return false;
  }
  // Only a write takes the lock: a read sees the file whole either way (a
  // write replaces it by rename, and a read between the two renames takes
  // the .old), and making and removing the lock file is a write itself.
  FileLock lock(file_ + ".lock");
  if (writes && !lock.acquire(error))
    return false;
  sqlite3 *db = nullptr;
  bool newer = false;
  std::string was;
  // Only a write needs the digest (to skip writing bytes that did not
  // change); hashing the whole file at each read was most of a read.
  if (connect(file_, db, false, newer, error, writes ? &was : nullptr) != Opened::Ok)
    return false;
  // Another instance may have put a newer build's file here since.
  readOnly_ = newer;
  if (writes && newer) {
    error = NewerBuild;
    closeDb(db);
    return false;
  }
  bool ok = exec(db, writes ? "BEGIN IMMEDIATE;" : "BEGIN;", error) && work(db, error);
  // Suspended so long that another instance took the lock over: what this
  // one holds is no longer the file, so it rolls back instead of committing.
  if (ok && writes && !lock.owned()) {
    error = "lost the lock to another CookingFoil";
    ok = false;
  }
  ok = ok && exec(db, "COMMIT;", error);
  // Committed in memory: the file takes it whole, or the change is lost
  // (and reported) with the file as it was.
  if (ok && writes)
    ok = persist(db, file_, error, &was);
  if (!ok && error.empty())
    error = sqlite3_errmsg(db);
  closeDb(db); // rolls back whatever did not commit
  return ok;
}
namespace {
history::Entry entryOf(const Statement &rows, bool &readable) {
  history::Entry e;
  auto install = parsePending(rows.column(5));
  readable = bool(install);
  if (!install) return e; // kept in the file, never deleted: this build cannot read it
  e.id = uint64_t(sqlite3_column_int64(rows.get(), 0));
  e.kind = rows.column(2);
  e.outcome = outcomeOf(rows.column(3));
  e.group = sqlite3_column_int(rows.get(), 4);
  e.install = std::move(*install);
  e.payload = rows.column(6);
  e.error = rows.column(7);
  e.at = sqlite3_column_int64(rows.get(), 8);
  e.startedAt = sqlite3_column_int64(rows.get(), 9);
  e.bytes = uint64_t(sqlite3_column_int64(rows.get(), 10));
  e.downloadSeconds = sqlite3_column_double(rows.get(), 11);
  e.installSeconds = sqlite3_column_double(rows.get(), 12);
  return e;
}
constexpr history::Outcome Endings[] = {history::Outcome::Done, history::Outcome::Failed, history::Outcome::Cancelled};
// Rows of one ending, newest first, from before (at, id) when `before`;
// one past `limit` is read to tell whether more are left.
bool readEnding(sqlite3 *db, history::Outcome outcome, bool before, int64_t at, uint64_t id, size_t limit,
                std::vector<std::pair<int64_t, history::Entry>> &out, std::set<uint64_t> &ids, bool &more) {
  Statement rows(db, (std::string(SelectRows) + " WHERE outcome=?" + (before ? " AND (at<? OR (at=? AND id<?))" : "") +
                      " ORDER BY at DESC, id DESC LIMIT ?")
                         .c_str());
  if (!rows) return false;
  int at_ = 1;
  rows.text(at_++, outcomeName(outcome));
  if (before) {
    rows.integer(at_++, at);
    rows.integer(at_++, at);
    rows.integer(at_++, int64_t(id));
  }
  rows.integer(at_, limit == SIZE_MAX ? -1 : int64_t(limit) + 1);
  more = false;
  size_t taken = 0;
  int rc;
  while ((rc = rows.step()) == SQLITE_ROW) {
    if (taken == limit) {
      more = true;
      break;
    }
    ++taken;
    bool readable = false;
    auto e = entryOf(rows, readable);
    if (!readable) continue;
    ids.insert(e.id);
    out.emplace_back(sqlite3_column_int64(rows.get(), 1), std::move(e));
  }
  return rc == SQLITE_DONE || rc == SQLITE_ROW;
}
} // namespace
std::vector<history::Entry> DownloadStore::load(size_t finishedPage) {
  std::lock_guard<std::mutex> held(lock_);
  std::vector<history::Entry> out;
  std::set<uint64_t> ids;
  std::set<history::Outcome> more;
  uint64_t top = 0;
  // Nothing known until a load succeeds, so a save after a failed load
  // deletes nothing it never saw.
  known_.clear();
  std::string error;
  const bool ok = run(false, error, [&](sqlite3 *db, std::string &) {
    {
      Statement highest(db, "SELECT MAX(id) FROM downloads");
      if (!highest || highest.step() != SQLITE_ROW) return false;
      top = uint64_t(sqlite3_column_int64(highest.get(), 0));
    }
    // Waiting and running rows all; of each ending its page. Then the
    // rows read in the list's order (position), as they were saved.
    std::vector<std::pair<int64_t, history::Entry>> read;
    Statement rows(db, (std::string(SelectRows) + " WHERE outcome IN ('running','queued')").c_str());
    if (!rows) return false;
    int rc;
    while ((rc = rows.step()) == SQLITE_ROW) {
      bool readable = false;
      auto e = entryOf(rows, readable);
      if (!readable) continue;
      ids.insert(e.id);
      read.emplace_back(sqlite3_column_int64(rows.get(), 1), std::move(e));
    }
    if (rc != SQLITE_DONE) return false;
    for (const auto outcome : Endings) {
      bool left = false;
      if (!readEnding(db, outcome, false, 0, 0, finishedPage, read, ids, left)) return false;
      if (left) more.insert(outcome);
    }
    std::stable_sort(read.begin(), read.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (auto &[position, e] : read) out.push_back(std::move(e));
    return true;
  });
  if (!ok)
    return {};
  known_ = std::move(ids);
  more_ = std::move(more);
  publishMore();
  topId_ = top;
  return out;
}
std::vector<history::Entry> DownloadStore::older(history::Outcome outcome, int64_t at, uint64_t id, size_t limit,
                                                 bool &more) {
  std::lock_guard<std::mutex> held(lock_);
  std::vector<std::pair<int64_t, history::Entry>> read;
  std::set<uint64_t> ids;
  more = false;
  std::string error;
  const bool ok = run(false, error, [&](sqlite3 *db, std::string &) {
    return readEnding(db, outcome, true, at, id, limit, read, ids, more);
  });
  std::vector<history::Entry> out;
  for (auto &[position, e] : read) out.push_back(std::move(e));
  if (!ok) {
    more = true; // not read now: asked again later
    return {};
  }
  known_.insert(ids.begin(), ids.end());
  if (more) more_.insert(outcome);
  else more_.erase(outcome);
  publishMore();
  return out;
}
void DownloadStore::publishMore() {
  uint32_t mask = 0;
  for (const auto outcome : more_) mask |= 1u << unsigned(outcome);
  moreMask_ = mask;
}
bool DownloadStore::moreOf(history::Outcome outcome) const { return (moreMask_ >> unsigned(outcome)) & 1u; }
uint64_t DownloadStore::topId() const { return topId_; }
bool DownloadStore::removeAll(history::Outcome outcome, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  if (!file_.empty() && readOnly_) {
    error = NewerBuild;
    return false;
  }
  const bool ok = run(true, error, [&](sqlite3 *db, std::string &) {
    Statement remove(db, "DELETE FROM downloads WHERE outcome=?");
    if (!remove) return false;
    remove.text(1, outcomeName(outcome));
    return remove.step() == SQLITE_DONE;
  });
  if (ok) {
    more_.erase(outcome);
    publishMore();
  }
  return ok;
}
bool DownloadStore::save(const std::vector<history::Entry> &entries, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  if (!file_.empty() && readOnly_) {
    error = NewerBuild;
    return false;
  }
  std::set<uint64_t> now;
  const bool ok = run(true, error, [&](sqlite3 *db, std::string &) {
    // The file as it is now, not as this instance last left it: another
    // instance may have written since.
    // Only the rows this save may touch (in the list or known), one by one:
    // the older history it never read stays unread.
    std::map<uint64_t, std::string> inFile;
    {
      Statement row(db, (std::string(SelectRows) + " WHERE id=?").c_str());
      if (!row)
        return false;
      std::set<uint64_t> asked(known_);
      for (const auto &e : entries) asked.insert(e.id);
      for (const auto id : asked) {
        row.integer(1, int64_t(id));
        const int rc = row.step();
        if (rc == SQLITE_ROW) inFile[id] = fingerprint(row);
        else if (rc != SQLITE_DONE) return false;
        row.reset();
      }
    }
    Statement upsert(db, "INSERT INTO downloads(id, position, kind, outcome, grp, install, payload, error, at, started_at,"
                         " bytes, download_seconds, install_seconds) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
                         " ON CONFLICT(id) DO UPDATE SET position=excluded.position, kind=excluded.kind,"
                         " outcome=excluded.outcome, grp=excluded.grp, install=excluded.install, payload=excluded.payload,"
                         " error=excluded.error, at=excluded.at, started_at=excluded.started_at, bytes=excluded.bytes,"
                         " download_seconds=excluded.download_seconds, install_seconds=excluded.install_seconds");
    Statement remove(db, "DELETE FROM downloads WHERE id=?");
    if (!upsert || !remove)
      return false;
    for (size_t position = 0; position < entries.size(); ++position) {
      const auto &e = entries[position];
      const auto install = serializePending(e.install);
      const auto print = fingerprint(int64_t(position), e.kind, outcomeName(e.outcome), e.group, install, e.payload,
                                     e.error, e.at, e.startedAt, int64_t(e.bytes), e.downloadSeconds, e.installSeconds);
      now.insert(e.id);
      const auto there = inFile.find(e.id);
      if (there != inFile.end() && there->second == print)
        continue;
      upsert.integer(1, int64_t(e.id));
      upsert.integer(2, int64_t(position));
      upsert.text(3, e.kind);
      upsert.text(4, outcomeName(e.outcome));
      upsert.integer(5, e.group);
      upsert.text(6, install);
      upsert.text(7, e.payload);
      upsert.text(8, e.error);
      upsert.integer(9, e.at);
      upsert.integer(10, e.startedAt);
      upsert.integer(11, int64_t(e.bytes));
      upsert.real(12, e.downloadSeconds);
      upsert.real(13, e.installSeconds);
      const int rc = upsert.step();
      upsert.reset();
      if (rc != SQLITE_DONE)
        return false;
    }
    for (const auto id : known_)
      if (!now.count(id) && inFile.count(id)) {
        remove.integer(1, int64_t(id));
        const int rc = remove.step();
        remove.reset();
        if (rc != SQLITE_DONE)
          return false;
      }
    return true;
  });
  // Nothing of a failed save stays: its transaction never committed.
  if (ok) {
    for (const auto id : now) topId_ = std::max(topId_.load(), id);
    known_ = std::move(now);
  }
  return ok;
}
// Where a cut-off file stands is written every 30 s while it downloads, so
// it has a small file of its own (cookingfoil.resume.json): rewriting the whole database (the whole
// download history) that often only wore the card. The database's
// resume_points table is what builds before 0.3.97 kept; open() moves its
// rows here, and it is still read in case that could not be done.
std::string DownloadStore::resumeFile() const {
  return std::filesystem::path(file_).replace_extension(".resume.json").string(); // cookingfoil.resume.json
}
std::map<std::string, DownloadStore::ResumePoint> DownloadStore::resumePoints() {
  std::lock_guard<std::mutex> held(lock_);
  if (file_.empty())
    return {};
  Points out;
  std::string error;
  run(false, error, [&](sqlite3 *db, std::string &) {
    out = legacyPoints(db);
    return true;
  });
  for (auto &[key, point] : parseResume(readFile(resumeFile())).points)
    out[key] = std::move(point);
  return out;
}
std::optional<DownloadStore::ResumePoint> DownloadStore::resumePoint(const std::string &key) {
  const auto all = resumePoints();
  const auto found = all.find(key);
  if (found == all.end())
    return std::nullopt;
  return found->second;
}
bool DownloadStore::changeResume(const std::function<void(Points &, std::vector<std::string> &)> &change) {
  std::lock_guard<std::mutex> held(lock_);
  if (file_.empty() || readOnly_)
    return false;
  std::string error;
  FileLock lock(file_ + ".lock");
  if (!lock.acquire(error))
    return false;
  auto resume = parseResume(readFile(resumeFile()));
  change(resume.points, resume.made);
  return atomicWrite(resumeFile(), serializeResume(resume), error); // unchanged: not written
}
std::vector<std::string> DownloadStore::madePlaceholders() {
  std::lock_guard<std::mutex> held(lock_);
  return file_.empty() ? std::vector<std::string>{} : parseResume(readFile(resumeFile())).made;
}
void DownloadStore::dropMadePlaceholder(const std::string &key) {
  changeResume([&](Points &, std::vector<std::string> &made) { made.erase(std::remove(made.begin(), made.end(), key), made.end()); });
}
bool DownloadStore::saveResumePoint(const std::string &key, const ResumePoint &point) {
  return changeResume([&](Points &points, std::vector<std::string> &made) {
    points[key] = point;
    if (std::find(made.begin(), made.end(), key) == made.end()) made.push_back(key);
  });
}
bool DownloadStore::forgetResumePoint(const std::string &key) {
  const bool ok = changeResume([&](Points &points, std::vector<std::string> &) { points.erase(key); });
  // One the database still holds (its move here could not be made) goes too.
  std::lock_guard<std::mutex> held(lock_);
  std::string error;
  run(true, error, [&](sqlite3 *db, std::string &) {
    Statement remove(db, "DELETE FROM resume_points WHERE key=?");
    if (!remove)
      return false;
    remove.text(1, key);
    return remove.step() == SQLITE_DONE;
  });
  return ok;
}
bool DownloadStore::takeSetAside() { return setAside.exchange(false); }
std::optional<DownloadStore::StateRows> DownloadStore::stateRows(std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  StateRows rows;
  const bool ok = run(false, error, [&](sqlite3 *db, std::string &) {
    Statement settings(db, "SELECT key, value FROM settings");
    Statement notes(db, "SELECT server, note FROM pair_notes");
    Statement credentials(db, "SELECT kind, server, value FROM credentials");
    if (!settings || !notes || !credentials)
      return false;
    int rc;
    while ((rc = settings.step()) == SQLITE_ROW) rows.settings[settings.column(0)] = settings.column(1);
    if (rc != SQLITE_DONE) return false;
    while ((rc = notes.step()) == SQLITE_ROW) rows.notes[notes.column(0)] = notes.column(1);
    if (rc != SQLITE_DONE) return false;
    while ((rc = credentials.step()) == SQLITE_ROW)
      rows.credentials[{credentials.column(0), credentials.column(1)}] = credentials.column(2);
    return rc == SQLITE_DONE;
  });
  if (!ok) return std::nullopt;
  return rows;
}
bool DownloadStore::saveStateRows(const StateRows &rows, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  return run(true, error, [&](sqlite3 *db, std::string &failure) {
    if (!exec(db, "DELETE FROM settings; DELETE FROM pair_notes; DELETE FROM credentials;", failure))
      return false;
    Statement setting(db, "INSERT INTO settings(key, value) VALUES(?,?)");
    Statement note(db, "INSERT INTO pair_notes(server, note) VALUES(?,?)");
    Statement credential(db, "INSERT INTO credentials(kind, server, value) VALUES(?,?,?)");
    if (!setting || !note || !credential)
      return false;
    for (const auto &[key, value] : rows.settings) {
      setting.reset();
      setting.text(1, key);
      setting.text(2, value);
      if (setting.step() != SQLITE_DONE) return false;
    }
    for (const auto &[server, text] : rows.notes) {
      note.reset();
      note.text(1, server);
      note.text(2, text);
      if (note.step() != SQLITE_DONE) return false;
    }
    for (const auto &[at, value] : rows.credentials) {
      credential.reset();
      credential.text(1, at.first);
      credential.text(2, at.second);
      credential.text(3, value);
      if (credential.step() != SQLITE_DONE) return false;
    }
    return true;
  });
}
std::optional<DownloadStore::ClientStrings> DownloadStore::clientStrings(const std::string &lang) {
  std::lock_guard<std::mutex> held(lock_);
  std::optional<ClientStrings> out;
  std::string error;
  run(false, error, [&](sqlite3 *db, std::string &) {
    Statement read(db, "SELECT server, version, etag, body FROM client_strings WHERE lang=?");
    if (!read) return false;
    read.text(1, lang);
    const int rc = read.step();
    if (rc == SQLITE_ROW) out = ClientStrings{read.column(0), read.column(1), read.column(2), read.column(3)};
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
  });
  return out;
}
std::map<std::string, DownloadStore::ClientStrings> DownloadStore::allClientStrings() {
  std::lock_guard<std::mutex> held(lock_);
  std::map<std::string, ClientStrings> out;
  std::string error;
  run(false, error, [&](sqlite3 *db, std::string &) {
    Statement read(db, "SELECT lang, server, version, etag, body FROM client_strings");
    if (!read) return false;
    int rc;
    while ((rc = read.step()) == SQLITE_ROW)
      out[read.column(0)] = ClientStrings{read.column(1), read.column(2), read.column(3), read.column(4)};
    return rc == SQLITE_DONE;
  });
  return out;
}
std::vector<ExtraLanguage> DownloadStore::clientLanguages() {
  std::lock_guard<std::mutex> held(lock_);
  std::vector<ExtraLanguage> out;
  std::string error;
  run(false, error, [&](sqlite3 *db, std::string &) {
    Statement read(db, "SELECT code, name, base FROM client_languages ORDER BY rowid");
    if (!read) return false;
    int rc;
    while ((rc = read.step()) == SQLITE_ROW) out.push_back({read.column(0), read.column(1), read.column(2)});
    return rc == SQLITE_DONE;
  });
  return out;
}
bool DownloadStore::saveClientLanguages(const std::vector<ExtraLanguage> &languages, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  return run(true, error, [&](sqlite3 *db, std::string &failure) {
    if (!exec(db, "DELETE FROM client_languages;", failure)) return false;
    Statement write(db, "INSERT OR REPLACE INTO client_languages(code, name, base) VALUES(?,?,?)");
    if (!write) return false;
    for (const auto &language : languages) {
      write.reset();
      write.text(1, language.code);
      write.text(2, language.name);
      write.text(3, language.base);
      if (write.step() != SQLITE_DONE) return false;
    }
    return true;
  });
}
bool DownloadStore::saveClientStrings(const std::string &lang, const ClientStrings &row, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  return run(true, error, [&](sqlite3 *db, std::string &) {
    Statement write(db, "INSERT OR REPLACE INTO client_strings(lang, server, version, etag, body) VALUES(?,?,?,?,?)");
    if (!write) return false;
    write.text(1, lang);
    write.text(2, row.server);
    write.text(3, row.version);
    write.text(4, row.etag);
    write.text(5, row.body);
    return write.step() == SQLITE_DONE;
  });
}
DownloadStore::KeepReads::KeepReads() {
  std::lock_guard<std::mutex> held(keptLock);
  ++keeping;
}
DownloadStore::KeepReads::~KeepReads() {
  std::lock_guard<std::mutex> held(keptLock);
  if (--keeping == 0) keptFiles.clear();
}
} // namespace foil
