#pragma once
// 다운로드's records in SQLite (cookingfoil.db on the SD card): the queue and
// every download that ran, in list order, and where a file that was cut off
// stands so it picks up at that byte. The app's own state (settings, the
// device key and the keys servers gave it) lives in the same file; State.cpp
// reads and checks it.
//
// Two instances can share the file (the app suspended in the background and
// a second one started from the Album), and the console has no file locks
// SQLite can use. So no connection stays open: each call takes a lock file
// next to the database (cookingfoil.db.lock), opens, does its work in one
// transaction, closes and lets go. A lock that cannot be had within about
// two seconds fails the call (the caller's list stays as it is and the next
// save tries again); one older than ten seconds is taken over. Every method
// also takes the mutex, so any thread may call it.
//
// It never takes the app down and never drops what was committed: a file
// that is not a database or fails its check is set aside as ".broken" and a
// fresh one made; a journal left by a cut-off write rolls back on open; every
// write is one transaction that rolls back whole on any error; a file from a
// newer build is read but never written.
#include "core/DownloadHistory.hpp"
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>
struct sqlite3;
namespace foil {
class DownloadStore {
public:
  DownloadStore() = default;
  DownloadStore(const DownloadStore &) = delete;
  DownloadStore &operator=(const DownloadStore &) = delete;
  ~DownloadStore();
  // Checks (or creates) the database, sets a damaged one aside and brings its
  // tables up to date; later calls open it by this path. False only when no
  // file can be opened or made there at all, or another instance holds it.
  bool open(const std::string &path, std::string &error);
  bool isOpen() const;
  // A file from a newer build: it loads, but writes are refused so that
  // build finds it unchanged.
  bool readOnly() const;
  void close();
  // The name SQLite opens: "sdmc:/" is the card's root, which is "/" there.
  static std::string fileName(const std::string &path);
  // The list, top row first (empty when the file cannot be read now).
  std::vector<history::Entry> load();
  // The list as it is now, in one transaction: rows that differ from the
  // file are written, rows this instance loaded or wrote and that are no
  // longer in the list are deleted, rows it never knew are left alone.
  bool save(const std::vector<history::Entry> &entries, std::string &error);
  struct ResumePoint {
    std::string url, etag;
    uint64_t total = 0;
    std::string plan; // net::SegmentPlan::save()
  };
  std::optional<ResumePoint> resumePoint(const std::string &key);
  std::map<std::string, ResumePoint> resumePoints();
  bool saveResumePoint(const std::string &key, const ResumePoint &point);
  bool forgetResumePoint(const std::string &key);
  // <name>.resume.json beside the database file.
  std::string resumeFile() const;
  // Every key a point was saved for (kept in the same file, after its point
  // goes): the console placeholders this app made, so one left without a
  // point can be cleared and the console's own downloads (eShop) are never
  // touched. A key goes once its placeholder is gone (dropMadePlaceholder).
  std::vector<std::string> madePlaceholders();
  void dropMadePlaceholder(const std::string &key);
  // The app's state as rows, unchecked: settings by name, approval notes by
  // server, and credentials by (kind, server).
  struct StateRows {
    std::map<std::string, std::string> settings, notes;
    std::map<std::pair<std::string, std::string>, std::string> credentials;
  };
  std::optional<StateRows> stateRows(std::string &error);
  // Replaces all of them in one transaction.
  bool saveStateRows(const StateRows &rows, std::string &error);
  // A damaged file was set aside and a fresh one made since the last call,
  // by any instance: whatever it held is gone, the keys too.
  static bool takeSetAside();
  // The error a store gives while another instance (or this one) holds the
  // file for longer than it waits: a moment's, not the file's.
  static bool busy(const std::string &error);

private:
  enum class Opened { Ok, Broken, Failed };
  // `read`: the file's bytes as they were read (empty when it had none).
  Opened connect(const std::string &file, sqlite3 *&db, bool check, bool &newer, std::string &error,
                 std::string *read = nullptr);
  // One call's work: lock, open, one transaction, close, unlock.
  bool run(bool writes, std::string &error, const std::function<bool(sqlite3 *, std::string &)> &work);
  // Reads the resume file, lets `change` edit its points and made keys, and
  // writes it back (only when it changed), under the lock.
  bool changeResume(const std::function<void(std::map<std::string, ResumePoint> &, std::vector<std::string> &)> &change);
  mutable std::mutex lock_;
  std::string file_; // empty while closed
  bool readOnly_ = false;
  std::set<uint64_t> known_; // rows this instance loaded or wrote: the only ones its saves delete
};
} // namespace foil
