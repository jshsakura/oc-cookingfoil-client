#include "core/DownloadStore.hpp"
#include "core/State.hpp"
#include <sqlite3.h>
#include <ctime>
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
// A prepared statement that finalizes itself.
class Statement {
public:
  Statement(sqlite3 *db, const char *sql) { sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr); }
  ~Statement() { sqlite3_finalize(stmt_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  explicit operator bool() const { return stmt_ != nullptr; }
  sqlite3_stmt *get() const { return stmt_; }
  void text(int at, const std::string &value) { sqlite3_bind_text(stmt_, at, value.c_str(), int(value.size()), SQLITE_TRANSIENT); }
  void integer(int at, int64_t value) { sqlite3_bind_int64(stmt_, at, value); }
  void real(int at, double value) { sqlite3_bind_double(stmt_, at, value); }
  std::string column(int at) const {
    const auto *value = sqlite3_column_text(stmt_, at);
    return value ? std::string(reinterpret_cast<const char *>(value), size_t(sqlite3_column_bytes(stmt_, at))) : "";
  }
  int step() { return sqlite3_step(stmt_); }
  void reset() {
    sqlite3_reset(stmt_);
    sqlite3_clear_bindings(stmt_);
  }

private:
  sqlite3_stmt *stmt_ = nullptr;
};
// Everything a row holds, as one string: a row is written only when it differs.
std::string fingerprint(const history::Entry &e, size_t position, const std::string &install) {
  return std::to_string(position) + '\n' + e.kind + '\n' + outcomeName(e.outcome) + '\n' + std::to_string(e.group) + '\n' +
         install + '\n' + e.payload + '\n' + e.error + '\n' + std::to_string(e.at) + '\n' + std::to_string(e.startedAt) +
         '\n' + std::to_string(e.bytes) + '\n' + std::to_string(e.downloadSeconds) + '\n' + std::to_string(e.installSeconds);
}
} // namespace
DownloadStore::~DownloadStore() { close(); }
void DownloadStore::close() {
  if (db_)
    sqlite3_close(db_);
  db_ = nullptr;
  written_.clear();
}
bool DownloadStore::exec(const char *sql, std::string &error) {
  char *message = nullptr;
  if (sqlite3_exec(db_, sql, nullptr, nullptr, &message) == SQLITE_OK)
    return true;
  error = message ? message : sqlite3_errmsg(db_);
  sqlite3_free(message);
  return false;
}
bool DownloadStore::open(const std::string &path, std::string &error) {
  close();
  // One process only, so no file locks (the console's file system has none
  // that SQLite can use).
  if (sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "unix-none") != SQLITE_OK) {
    error = db_ ? sqlite3_errmsg(db_) : "sqlite";
    close();
    return false;
  }
  // A power cut mid-write keeps the last committed list.
  if (!exec("PRAGMA journal_mode=TRUNCATE; PRAGMA synchronous=FULL;", error)) {
    close();
    return false;
  }
  int version = 0;
  {
    Statement read(db_, "PRAGMA user_version");
    if (!read || read.step() != SQLITE_ROW) {
      error = sqlite3_errmsg(db_);
      close();
      return false;
    }
    version = sqlite3_column_int(read.get(), 0);
  }
  const int latest = int(sizeof(Migrations) / sizeof(Migrations[0]));
  for (int step = version; step < latest; ++step) {
    const std::string sql = std::string("BEGIN;") + Migrations[step] + "PRAGMA user_version=" + std::to_string(step + 1) +
                            ";COMMIT;";
    if (!exec(sql.c_str(), error)) {
      std::string ignored;
      exec("ROLLBACK;", ignored);
      close();
      return false;
    }
  }
  return true;
}
std::vector<history::Entry> DownloadStore::load() {
  std::lock_guard<std::mutex> held(lock_);
  std::vector<history::Entry> out;
  written_.clear();
  if (!db_)
    return out;
  Statement rows(db_, "SELECT id, position, kind, outcome, grp, install, payload, error, at, started_at, bytes,"
                      " download_seconds, install_seconds FROM downloads ORDER BY position");
  if (!rows)
    return out;
  while (rows.step() == SQLITE_ROW) {
    auto install = parsePending(rows.column(5));
    if (!install)
      continue;
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
    const auto position = size_t(sqlite3_column_int64(rows.get(), 1));
    written_[e.id] = fingerprint(e, position, rows.column(5));
    out.push_back(std::move(e));
  }
  return out;
}
bool DownloadStore::save(const std::vector<history::Entry> &entries, std::string &error) {
  std::lock_guard<std::mutex> held(lock_);
  if (!db_) {
    error = "closed";
    return false;
  }
  if (!exec("BEGIN IMMEDIATE;", error))
    return false;
  Statement upsert(db_, "INSERT INTO downloads(id, position, kind, outcome, grp, install, payload, error, at, started_at,"
                        " bytes, download_seconds, install_seconds) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)"
                        " ON CONFLICT(id) DO UPDATE SET position=excluded.position, kind=excluded.kind,"
                        " outcome=excluded.outcome, grp=excluded.grp, install=excluded.install, payload=excluded.payload,"
                        " error=excluded.error, at=excluded.at, started_at=excluded.started_at, bytes=excluded.bytes,"
                        " download_seconds=excluded.download_seconds, install_seconds=excluded.install_seconds");
  Statement remove(db_, "DELETE FROM downloads WHERE id=?");
  auto fail = [&] {
    error = sqlite3_errmsg(db_);
    std::string ignored;
    exec("ROLLBACK;", ignored);
    return false;
  };
  if (!upsert || !remove)
    return fail();
  std::map<uint64_t, std::string> now;
  for (size_t position = 0; position < entries.size(); ++position) {
    const auto &e = entries[position];
    const auto install = serializePending(e.install);
    auto print = fingerprint(e, position, install);
    const auto before = written_.find(e.id);
    if (before == written_.end() || before->second != print) {
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
      if (upsert.step() != SQLITE_DONE)
        return fail();
      upsert.reset();
    }
    now[e.id] = std::move(print);
  }
  for (const auto &[id, print] : written_)
    if (!now.count(id)) {
      remove.integer(1, int64_t(id));
      if (remove.step() != SQLITE_DONE)
        return fail();
      remove.reset();
    }
  if (!exec("COMMIT;", error))
    return fail();
  written_ = std::move(now);
  return true;
}
std::optional<DownloadStore::ResumePoint> DownloadStore::resumePoint(const std::string &key) {
  std::lock_guard<std::mutex> held(lock_);
  if (!db_)
    return std::nullopt;
  Statement read(db_, "SELECT url, etag, total, plan FROM resume_points WHERE key=?");
  if (!read)
    return std::nullopt;
  read.text(1, key);
  if (read.step() != SQLITE_ROW)
    return std::nullopt;
  return ResumePoint{read.column(0), read.column(1), uint64_t(sqlite3_column_int64(read.get(), 2)), read.column(3)};
}
std::map<std::string, DownloadStore::ResumePoint> DownloadStore::resumePoints() {
  std::lock_guard<std::mutex> held(lock_);
  std::map<std::string, ResumePoint> out;
  if (!db_)
    return out;
  Statement read(db_, "SELECT key, url, etag, total, plan FROM resume_points");
  if (!read)
    return out;
  while (read.step() == SQLITE_ROW)
    out[read.column(0)] = {read.column(1), read.column(2), uint64_t(sqlite3_column_int64(read.get(), 3)), read.column(4)};
  return out;
}
bool DownloadStore::saveResumePoint(const std::string &key, const ResumePoint &point) {
  std::lock_guard<std::mutex> held(lock_);
  if (!db_)
    return false;
  Statement write(db_, "INSERT INTO resume_points(key, url, etag, total, plan, updated_at) VALUES(?,?,?,?,?,?)"
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
}
void DownloadStore::forgetResumePoint(const std::string &key) {
  std::lock_guard<std::mutex> held(lock_);
  if (!db_)
    return;
  Statement remove(db_, "DELETE FROM resume_points WHERE key=?");
  if (!remove)
    return;
  remove.text(1, key);
  remove.step();
}
} // namespace foil
