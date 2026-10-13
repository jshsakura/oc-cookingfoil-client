#pragma once
// 다운로드's records in SQLite (cookingfoil.db on the SD card): the queue and
// every download that ran, in list order, and where a file that was cut off
// stands so it picks up at that byte. One connection; the UI and the install
// thread both use it, one at a time.
#include "core/DownloadHistory.hpp"
#include <map>
#include <mutex>
#include <optional>
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
  // Opens (or creates) the database and brings its tables up to date.
  bool open(const std::string &path, std::string &error);
  bool isOpen() const { return db_ != nullptr; }
  void close();
  // The list, top row first.
  std::vector<history::Entry> load();
  // The list as it is now, in one transaction: changed rows are written,
  // rows no longer there deleted, the rest left alone.
  bool save(const std::vector<history::Entry> &entries, std::string &error);
  struct ResumePoint {
    std::string url, etag;
    uint64_t total = 0;
    std::string plan; // net::SegmentPlan::save()
  };
  std::optional<ResumePoint> resumePoint(const std::string &key);
  std::map<std::string, ResumePoint> resumePoints();
  bool saveResumePoint(const std::string &key, const ResumePoint &point);
  void forgetResumePoint(const std::string &key);

private:
  bool exec(const char *sql, std::string &error);
  std::mutex lock_;
  sqlite3 *db_ = nullptr;
  std::map<uint64_t, std::string> written_; // each row as last written, to skip the unchanged
};
} // namespace foil
