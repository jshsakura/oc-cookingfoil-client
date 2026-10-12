#pragma once
#include "core/Updates.hpp"
#include <array>
#include <atomic>
#include <optional>
namespace foil {
struct InstalledGame {
  uint64_t applicationId = 0;
  std::string titleId, name, publisher, iconBytes, displayVersion;
  uint32_t version = 0;
  // bytes: NS occupied size. lastUpdated: NS record order, larger is newer.
  uint64_t bytes = 0, lastUpdated = 0;
  std::vector<std::string> storage;
};
enum class InstalledSort { Name, Size, Recent };
constexpr int InstalledSortCount = 3;
const char *installedSortLabel(InstalledSort sort);
std::vector<InstalledGame> sortInstalled(std::vector<InstalledGame> games,
                                         InstalledSort sort);
struct InstalledLibrary {
  InstalledSnapshot installed;
  std::vector<InstalledGame> games;
};
struct SaveEntry {
  uint64_t saveId = 0, applicationId = 0, bytes = 0;
  std::array<uint64_t, 2> user{};
  int space = 1, type = 1, rank = 0;
  std::string userName;
  // An account (or the shared save) the game has no save for yet; restoring
  // a backup or a server save creates it first.
  bool missing = false;
  // type is the system's save data type: 1 an account's, 3 the shared save
  // (user {0, 0}) every account plays on.
  bool device() const { return type == 3; }
};
enum class RemovalKind { Game, Save };
struct RemovalRequest {
  RemovalKind kind = RemovalKind::Game;
  uint64_t applicationId = 0;
  SaveEntry save;
};
// Content files a storage holds that no installed record points to: leftovers
// of cut-off installs or of titles removed outside the system. Ids are the
// 16-byte NCA ids as strings. Returns nothing when a storage holds content but
// no record at all could be read, so a failed read never looks like
// "everything is unused".
std::optional<std::vector<std::string>> orphanContents(const std::vector<std::string> &stored,
                                                       const std::vector<std::string> &referenced);
// What a running cleanup is doing, read by the UI while it works.
struct CleanupProgress {
  std::atomic<int> stage{0};          // 0 starting, 1 SD card, 2 console
  std::atomic<int> done{0}, total{0}; // files handled in this storage
  std::atomic<uint64_t> bytes{0};     // found (scan) or freed (delete) so far
};
struct CleanupReport {
  int files = 0;      // orphaned content files
  int placeholders = 0; // unfinished install placeholders
  uint64_t bytes = 0;
};
Error validateRemoval(const RemovalRequest &request,
                      const InstalledLibrary &library,
                      const std::vector<SaveEntry> &saves,
                      uint64_t runningApplication, bool installing);
} // namespace foil
