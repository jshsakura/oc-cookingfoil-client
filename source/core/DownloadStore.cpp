#include "core/DownloadStore.hpp"
#include "core/State.hpp"
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>
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
};
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
void closeDb(sqlite3 *&db) {
  if (db) {
    rollback(db);
    sqlite3_close(db);
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
DownloadStore::~DownloadStore() { close(); }
std::string DownloadStore::fileName(const std::string &path) {
  // SQLite takes a path without "/" at its start as relative and puts the
  // working folder in front, so "sdmc:/…" never opened on the console. There
  // "/…" is the SD card already.
  return path.rfind("sdmc:/", 0) == 0 ? path.substr(5) : path;
}
bool DownloadStore::isOpen() const {
  std::lock_guard<std::mutex> held(lock_);
  return !file_.empty();
}
bool DownloadStore::readOnly() const {
  std::lock_guard<std::mutex> held(lock_);
  return readOnly_;
}
void DownloadStore::close() {
  std::lock_guard<std::mutex> held(lock_);
  file_.clear();
  readOnly_ = false;
  known_.clear();
}
// Opens the file and brings it up to date; `check` also runs quick_check.
// A journal left by a cut-off write is rolled back here, on the first read.
DownloadStore::Opened DownloadStore::connect(const std::string &file, sqlite3 *&db, bool check, bool &newer,
                                             std::string &error) {
  auto failed = [&](int rc) {
    if (error.empty())
      error = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    closeDb(db);
    return damaged(rc) ? Opened::Broken : Opened::Failed;
  };
  error.clear();
  newer = false;
  // No file locks in SQLite (the console's file system has none it can use);
  // the lock file stands in for them.
  const int opened = sqlite3_open_v2(file.c_str(), &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix-none");
  if (opened != SQLITE_OK)
    return failed(opened);
  sqlite3_extended_result_codes(db, 1);
  sqlite3_busy_timeout(db, 2000);
  // A power cut mid-write keeps the last committed state: the journal holds
  // the pages as they were until the commit. No temporary files on the card.
  if (!exec(db, "PRAGMA journal_mode=TRUNCATE; PRAGMA synchronous=FULL; PRAGMA temp_store=MEMORY;", error))
    return failed(sqlite3_errcode(db));
  if (check) {
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
  // Each step and its version number commit together, or not at all.
  for (int step = version; step < Latest; ++step) {
    const std::string sql = std::string("BEGIN IMMEDIATE;") + Migrations[step] + "PRAGMA user_version=" +
                            std::to_string(step + 1) + ";COMMIT;";
    if (!exec(db, sql.c_str(), error)) {
      // The file passed its check, so this is not the file's fault.
      closeDb(db);
      return Opened::Failed;
    }
  }
  return Opened::Ok;
}
bool DownloadStore::open(const std::string &path, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  file_.clear();
  readOnly_ = false;
  known_.clear();
  const std::string file = fileName(path);
  FileLock lock(file + ".lock");
  if (!lock.acquire(error))
    return false;
  sqlite3 *db = nullptr;
  bool newer = false;
  const auto first = connect(file, db, true, newer, error);
  closeDb(db);
  if (first == Opened::Failed)
    return false; // an error of the moment leaves the file alone
  if (first == Opened::Broken) {
    // Damaged: kept as .broken (an older one replaced) for whoever wants to
    // look, and a fresh one made so the app goes on. Its journal goes with
    // it, or the fresh file would have the old pages "rolled back" into it.
    const std::string reason = error;
    const std::string aside = file + ".broken";
    std::remove(aside.c_str());
    if (std::rename(file.c_str(), aside.c_str()) != 0) {
      error = reason + "; cannot set it aside";
      return false;
    }
    const std::string journal = file + "-journal";
    const std::string journalAside = aside + "-journal";
    std::remove(journalAside.c_str());
    if (std::rename(journal.c_str(), journalAside.c_str()) != 0)
      std::remove(journal.c_str());
    if (FILE *left = std::fopen(journal.c_str(), "rb")) {
      std::fclose(left);
      error = reason + "; cannot move its journal";
      return false;
    }
    const auto fresh = connect(file, db, true, newer, error);
    closeDb(db);
    if (fresh != Opened::Ok) {
      error = reason + "; a fresh one failed too: " + error;
      return false;
    }
  }
  file_ = file;
  readOnly_ = newer;
  return true;
}
bool DownloadStore::run(bool writes, std::string &error, const std::function<bool(sqlite3 *, std::string &)> &work) {
  error.clear();
  if (file_.empty()) {
    error = "closed";
    return false;
  }
  FileLock lock(file_ + ".lock");
  if (!lock.acquire(error))
    return false;
  sqlite3 *db = nullptr;
  bool newer = false;
  if (connect(file_, db, false, newer, error) != Opened::Ok)
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
  if (ok && !lock.owned()) {
    error = "lost the lock to another CookingFoil";
    ok = false;
  }
  ok = ok && exec(db, "COMMIT;", error);
  if (!ok && error.empty())
    error = sqlite3_errmsg(db);
  closeDb(db); // rolls back whatever did not commit
  return ok;
}
std::vector<history::Entry> DownloadStore::load() {
  std::lock_guard<std::mutex> held(lock_);
  std::vector<history::Entry> out;
  std::set<uint64_t> ids;
  // Nothing known until a load succeeds, so a save after a failed load
  // deletes nothing it never saw.
  known_.clear();
  std::string error;
  const bool ok = run(false, error, [&](sqlite3 *db, std::string &) {
    Statement rows(db, (std::string(SelectRows) + " ORDER BY position").c_str());
    if (!rows)
      return false;
    int rc;
    while ((rc = rows.step()) == SQLITE_ROW) {
      auto install = parsePending(rows.column(5));
      if (!install)
        continue; // kept in the file, never deleted: this build cannot read it
      history::Entry e;
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
      ids.insert(e.id);
      out.push_back(std::move(e));
    }
    return rc == SQLITE_DONE;
  });
  if (!ok)
    return {};
  known_ = std::move(ids);
  return out;
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
    std::map<uint64_t, std::string> inFile;
    {
      Statement rows(db, SelectRows);
      if (!rows)
        return false;
      int rc;
      while ((rc = rows.step()) == SQLITE_ROW)
        inFile[uint64_t(sqlite3_column_int64(rows.get(), 0))] = fingerprint(rows);
      if (rc != SQLITE_DONE)
        return false;
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
  if (ok)
    known_ = std::move(now);
  return ok;
}
std::optional<DownloadStore::ResumePoint> DownloadStore::resumePoint(const std::string &key) {
  std::lock_guard<std::mutex> held(lock_);
  std::optional<ResumePoint> out;
  std::string error;
  run(false, error, [&](sqlite3 *db, std::string &) {
    Statement read(db, "SELECT url, etag, total, plan FROM resume_points WHERE key=?");
    if (!read)
      return false;
    read.text(1, key);
    const int rc = read.step();
    if (rc == SQLITE_ROW)
      out = ResumePoint{read.column(0), read.column(1), uint64_t(sqlite3_column_int64(read.get(), 2)), read.column(3)};
    return rc == SQLITE_ROW || rc == SQLITE_DONE;
  });
  return out;
}
std::map<std::string, DownloadStore::ResumePoint> DownloadStore::resumePoints() {
  std::lock_guard<std::mutex> held(lock_);
  std::map<std::string, ResumePoint> out;
  std::string error;
  const bool ok = run(false, error, [&](sqlite3 *db, std::string &) {
    Statement read(db, "SELECT key, url, etag, total, plan FROM resume_points");
    if (!read)
      return false;
    int rc;
    while ((rc = read.step()) == SQLITE_ROW)
      out[read.column(0)] = {read.column(1), read.column(2), uint64_t(sqlite3_column_int64(read.get(), 3)), read.column(4)};
    return rc == SQLITE_DONE;
  });
  return ok ? out : std::map<std::string, ResumePoint>{};
}
bool DownloadStore::saveResumePoint(const std::string &key, const ResumePoint &point) {
  std::lock_guard<std::mutex> held(lock_);
  std::string error;
  return run(true, error, [&](sqlite3 *db, std::string &) {
    Statement write(db, "INSERT INTO resume_points(key, url, etag, total, plan, updated_at) VALUES(?,?,?,?,?,?)"
                        " ON CONFLICT(key) DO UPDATE SET url=excluded.url, etag=excluded.etag, total=excluded.total,"
                        " plan=excluded.plan, updated_at=excluded.updated_at");
    if (!write)
      return false;
    write.text(1, key);
    write.text(2, point.url);
    write.text(3, point.etag);
    write.integer(4, int64_t(point.total));
    write.text(5, point.plan);
    write.integer(6, int64_t(std::time(nullptr)));
    return write.step() == SQLITE_DONE;
  });
}
bool DownloadStore::forgetResumePoint(const std::string &key) {
  std::lock_guard<std::mutex> held(lock_);
  std::string error;
  return run(true, error, [&](sqlite3 *db, std::string &) {
    Statement remove(db, "DELETE FROM resume_points WHERE key=?");
    if (!remove)
      return false;
    remove.text(1, key);
    return remove.step() == SQLITE_DONE;
  });
}
} // namespace foil
