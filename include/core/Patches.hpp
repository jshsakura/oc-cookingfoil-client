#pragma once
#include "core/Votes.hpp"
#include "core/ArchiveWriter.hpp"
#include <atomic>
#include <chrono>
#include <functional>
#include "core/Types.hpp"
#include <cstdio>
#include <map>
#include <optional>
#include <set>
namespace foil {
// A user patch the server offers (feature: user-patches). Its files are laid
// out from the SD card root, e.g. atmosphere/contents/<TID>/romfs/...
struct UserPatch {
  std::string id, titleId, name, version, gameVersion, author, description, hash;
  std::string server; // the configured server it comes from (its url); empty: the one in use
  std::vector<std::string> kinds; // romfs, exefs, cheats, exefs_patches, fpslocker, homebrew
  // An app's patch has no Title ID: every file goes below switch/<app>/.
  std::string homebrewApp;
  uint64_t size = 0;
  int files = 0;
  int64_t updatedAt = 0;
  // The game's version number the patch was made for (an installed update's
  // number, 2097152 for v2); -1 when the server does not say.
  int64_t updateVersion = -1;
  bool required = false; // the game does not run without it
  bool verified = false; // an admin checked it
  votes::Votes votes;
  // The game the patch is for, from the server's titledb, for games the
  // shop itself does not list. Names are ko, en, ja, zh; any may be empty.
  std::string gameName, gameIcon;
  std::vector<std::string> gameNames;
  // The patch's own pictures, full addresses or shop paths (made full when
  // drawn, as gameIcon): an icon (empty: the game's), a wide picture and up
  // to 8 screenshots.
  std::string iconUrl, bannerUrl;
  std::vector<std::string> screenshots;
};
// Whether a patch for titleId may write this SD-root relative path. Only the
// game's own contents folder, exefs patches and FPSLocker patches qualify.
bool patchPathAllowed(const std::string &titleId, const std::string &path);
// The same for any patch: an app's patch may write only below switch/<app>/.
bool patchPathAllowed(const UserPatch &patch, const std::string &path);
// Cheat files hold one build's cheats; enabled patches share one file.
bool isCheatPath(const std::string &path);
const char *patchKindLabel(const std::string &kind);
// A game's base Title ID, upper case, from the ID of the game or of its
// update or DLC (the low 13 bits hold those). Text that is not a Title ID
// comes back upper-cased as it is.
std::string baseTitleId(const std::string &titleId);
// What a patch belongs to: its game's base Title ID, or "app:<folder>" for an
// app's patch (homebrew::ownerKey).
std::string patchOwner(const UserPatch &patch);
// Whether the patch belongs to `owner`: a game's Title ID in any case (an
// update's or DLC's too) or "app:<folder>".
bool patchBelongs(const UserPatch &patch, const std::string &owner);
std::vector<UserPatch> patchesFor(const std::vector<UserPatch> &patches, const std::string &owner);
// A version as written by hand, for comparing: no surrounding space and no
// leading "v" ("  v1.2.0 " -> "1.2.0").
std::string normalizeVersion(const std::string &version);
// Whether the patch suits the game version being installed: a patch for any
// version (no game_version, or "any") always does, and so does every patch
// when that version is unknown.
bool patchFitsVersion(const UserPatch &patch, const std::string &version);

// Unpacks a ustar stream under root as it arrives. Paths outside
// patchPathAllowed, links and special files abort the whole archive.
class TarWriter : public ArchiveWriter {
public:
  TarWriter(std::string root, std::string titleId);
  // Any archive whose every path `allow` accepts (retro game bundles).
  TarWriter(std::string root, std::function<bool(const std::string &)> allow);
  ~TarWriter() override;
  // Entries `skip` names are read past without being written (files that
  // must stay as they are on the card).
  void skip(std::function<bool(const std::string &)> skip) { skip_ = std::move(skip); }
  bool feed(const char *data, size_t size) override;
  // True when the archive ended with its two empty blocks.
  bool finish() override;
  const std::string &error() const override { return error_; }
  // Resuming: a fresh writer told where in the stream its first byte sits,
  // which must be a boundary an earlier writer reported.
  void startAt(uint64_t offset) override { fed_ = boundary_ = offset; }
  // Stream offset after the last whole entry (its padding included): every
  // file before it is complete on the card, so a download can continue here.
  uint64_t boundary() const override { return boundary_; }
  // Bytes the entry being read still expects.
  uint64_t entryLeft() const { return remaining_; }
  // Closes and removes the file an unfinished entry was writing.
  void abandon() override;

private:
  bool header();
  void readPax();
  bool fail(const std::string &message);
  bool openFile(const std::string &path, uint64_t size);
  std::string root_, titleId_, error_, block_, paxPath_, current_;
  std::function<bool(const std::string &)> allow_, skip_;
  std::FILE *file_ = nullptr;
  uint64_t remaining_ = 0, padding_ = 0, fed_ = 0, boundary_ = 0;
  std::optional<uint64_t> paxSize_;
  bool pax_ = false, paxGlobal_ = false, ended_ = false;
  std::string paxData_;
  int zeroBlocks_ = 0;
};
// A ustar size field (12 bytes): octal, or GNU base-256 when the first byte's
// high bit is set (sizes past 8 GiB). UINT64_MAX when unreadable.
uint64_t tarSize(const char *field);

// A file as a patch left it on the card: its size and, when the card keeps
// it through a rename, its modified time (0 when unknown, then not compared).
// A foreign mark says the file at the path is the user's, not the patch's.
struct FileMark {
  uint64_t size = 0;
  int64_t mtime = 0;
  static FileMark foreignFile() { return {UINT64_MAX, 0}; }
  bool foreign() const { return size == UINT64_MAX; }
  bool operator==(const FileMark &other) const { return size == other.size && mtime == other.mtime; }
};
// The mark of the file at a full path now, or nothing when there is none.
std::optional<FileMark> fileMark(const std::string &fullPath);
// Whether a file marked `now` is still the one marked `then`.
bool sameFile(const FileMark &then, const FileMark &now);
struct PatchRecord {
  std::string id, titleId, name, version, hash;
  std::string homebrewApp; // an app's patch: its files go below switch/<app>/
  bool on = false;
  // Set while files move, so an interrupted enable or disable can finish.
  std::string pending; // "", "enable" or "disable"
  std::vector<std::string> files;  // moved into place when on
  std::vector<std::string> cheats; // merged into place when on
  uint64_t bytes = 0;
  int64_t order = 0; // enable order; cheats merge in this order
  // Paths where a patch turned on later puts its own copy instead, and which
  // one. Worked out from the enable order; the ledger keeps a copy to read.
  std::map<std::string, std::string> shadowed;
  // Folders the pending enable or disable plans to move whole, one rename
  // each, instead of file by file. Kept with the pending mark so a move cut
  // short finishes the same way.
  std::vector<std::string> roots;
  // Files this patch put on the card, as they were then. A file there that
  // no longer matches is the user's, and stays the user's. Ledgers from
  // before marks have none, and their files count as the patch's.
  std::map<std::string, FileMark> marks;
};
// What a patch picked with a game install still needs, given what the SD
// store holds of it: fetch it, fetch its newer build, switch it on, or nothing.
enum class PatchStep { Get, Update, Enable, Skip };
PatchStep patchStep(const UserPatch &patch, const PatchRecord *record);
struct PatchConflict {
  std::string path, owner; // owner is a patch id, empty for the user's own file
};

// Downloaded patches live in a store on the SD card; enabling moves their
// files into place and a ledger remembers every path, so disabling takes back
// exactly those files and nothing the user put there.
//
// Patches layer. Where several that are on share a path, the one turned on
// last puts its copy on the card and the others keep theirs in their own
// store; turning it off brings back the copy of the latest one still on. A
// file the user put there is set aside under _base/<path> while any patch
// covers the path and comes back when the last one goes off.
class PatchStore {
public:
  PatchStore(std::string sdRoot, std::string storeRoot);
  // Reads the ledger and finishes any move a power loss interrupted.
  bool load(Error &error);
  const std::map<std::string, PatchRecord> &records() const { return records_; }
  const PatchRecord *find(const std::string &id) const;
  // An empty folder the download unpacks into before commit.
  std::string staging(const std::string &id) const;
  // Takes a finished staging folder into the store. An enabled older version
  // is swapped for the new one and enabled again in its old place in the order.
  bool commit(const UserPatch &patch, Error &error);
  // What the patch shares with others that are on (owner: that patch) and
  // with the user's own files (owner empty). Only for telling: enable goes
  // ahead and puts its own copy on top.
  std::vector<PatchConflict> conflicts(const std::string &id) const;
  bool enable(const std::string &id, Error &error);
  bool disable(const std::string &id, Error &error);
  bool remove(const std::string &id, Error &error);
  // Puts back the enabled patch's files missing from the card from its store.
  // `left` counts the ones its store lacks too: those need a new download.
  bool reapply(const std::string &id, size_t &left, Error &error);
  const std::string &sdRoot() const { return sdRoot_; }
  const std::string &storeRoot() const { return storeRoot_; }
  // Called as enable or disable moves files: files done so far, and all of
  // them. A folder moved whole counts every file in it at once.
  void progress(std::function<void(size_t, size_t)> report) { progress_ = std::move(report); }

private:
  std::string sd(const std::string &path) const;
  std::string stored(const std::string &id, const std::string &path) const;
  std::string base(const std::string &path) const;
  bool save(Error &error);
  bool move(const std::string &from, const std::string &to, Error &error);
  void prune(const std::string &path) const;
  bool finishEnable(PatchRecord &record, Error &error);
  bool finishDisable(PatchRecord &record, Error &error);
  bool rebuildCheat(const std::string &path, Error &error);
  bool turnOn(PatchRecord &record, int64_t order, Error &error);
  // The patch whose copy belongs at path: the latest one turned on.
  const PatchRecord *provider(const std::string &path) const;
  // The patch whose copy is on the card at path: one on or moving that lists
  // the path, whose store is missing it and whose mark (when it has one)
  // still matches the file. Anything else there is the user's.
  const PatchRecord *holder(const std::string &path) const;
  // Records the file the patch just put on the card at path.
  void mark(const std::string &id, const std::string &path);
  // Moves files so the card holds what provider() says, the user's own file
  // when no patch covers the path, or nothing.
  bool place(const std::string &path, Error &error);
  void markShadows();
  // The folders of record that may move whole: none of them on the card yet
  // (enabling) or back in its store yet (disabling), and no other patch on
  // or moving and no set-aside user file under them.
  std::vector<std::string> wholeRoots(const PatchRecord &record, bool enabling) const;
  // Whether the folder at dir holds exactly the record's files below root.
  bool holdsOnly(const PatchRecord &record, const std::string &root, const std::string &dir) const;
  // Whether every file of record below root the ledger marked is still as
  // the patch left it on the card.
  bool unchanged(const PatchRecord &record, const std::string &root) const;
  // Places each file not under a folder that moved whole, reporting progress.
  bool placeRest(const PatchRecord &record, const std::vector<std::string> &whole, Error &error);
  std::string sdRoot_, storeRoot_;
  std::function<void(size_t, size_t)> progress_;
  std::map<std::string, PatchRecord> records_;
  std::set<std::string> cheatPaths_; // cheat files we currently write
  int64_t order_ = 0;
};

// What the light check found of one enabled patch's files on the card.
struct PatchHealth {
  size_t missing = 0; // not on the card
  size_t changed = 0; // on the card but not as the patch left it: the user's
};
// Looks at the card for the files the ledger says enabled patches have there,
// and nothing else. Works from a copy of the records, so it runs on its own
// thread while the store stays with the patch worker. It stops when its
// budget is spent or `stop` is set and goes on from there the next run.
class PatchChecker {
public:
  PatchChecker(std::string sdRoot, std::string storeRoot, const std::map<std::string, PatchRecord> &records);
  // True once every file has been looked at.
  bool run(size_t maxLooks, std::chrono::milliseconds budget, const std::atomic<bool> *stop = nullptr);
  bool finished() const { return next_ >= work_.size(); }
  // Per patch id; a patch shows up once the check has reached it.
  const std::map<std::string, PatchHealth> &health() const { return health_; }
  size_t looks() const { return looks_; } // file system lookups so far
  size_t files() const { return work_.size(); }

private:
  struct Item {
    std::string id, path;
    std::optional<FileMark> mark;
  };
  std::string sdRoot_, storeRoot_;
  std::vector<Item> work_;
  size_t next_ = 0, looks_ = 0;
  std::map<std::string, PatchHealth> health_;
  // Whether each folder that moves whole is on the card, so the files below
  // a missing one need no lookup each.
  std::map<std::string, bool> cardRoots_;
};
} // namespace foil
