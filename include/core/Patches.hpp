#pragma once
#include <functional>
#include "core/Types.hpp"
#include <cstdio>
#include <map>
#include <set>
namespace foil {
// A user patch the server offers (feature: user-patches). Its files are laid
// out from the SD card root, e.g. atmosphere/contents/<TID>/romfs/...
struct UserPatch {
  std::string id, titleId, name, version, gameVersion, author, description, hash;
  std::vector<std::string> kinds; // romfs, exefs, cheats, exefs_patches, fpslocker, homebrew
  // An app's patch has no Title ID: every file goes below switch/<app>/.
  std::string homebrewApp;
  uint64_t size = 0;
  int files = 0;
  int64_t updatedAt = 0;
  // The game the patch is for, from the server's titledb, for games the
  // shop itself does not list. Names are ko, en, ja, zh; any may be empty.
  std::string gameName, gameIcon;
  std::vector<std::string> gameNames;
};
// Whether a patch for titleId may write this SD-root relative path. Only the
// game's own contents folder, exefs patches and FPSLocker patches qualify.
bool patchPathAllowed(const std::string &titleId, const std::string &path);
// The same for any patch: an app's patch may write only below switch/<app>/.
bool patchPathAllowed(const UserPatch &patch, const std::string &path);
// Cheat files hold one build's cheats; enabled patches share one file.
bool isCheatPath(const std::string &path);
const char *patchKindLabel(const std::string &kind);

// Unpacks a ustar stream under root as it arrives. Paths outside
// patchPathAllowed, links and special files abort the whole archive.
class TarWriter {
public:
  TarWriter(std::string root, std::string titleId);
  // Any archive whose every path `allow` accepts (retro game bundles).
  TarWriter(std::string root, std::function<bool(const std::string &)> allow);
  ~TarWriter();
  // Entries `skip` names are read past without being written (files that
  // must stay as they are on the card).
  void skip(std::function<bool(const std::string &)> skip) { skip_ = std::move(skip); }
  bool feed(const char *data, size_t size);
  // True when the archive ended with its two empty blocks.
  bool finish();
  const std::string &error() const { return error_; }

private:
  bool header();
  void readPax();
  bool fail(const std::string &message);
  bool openFile(const std::string &path);
  std::string root_, titleId_, error_, block_, paxPath_;
  std::function<bool(const std::string &)> allow_, skip_;
  std::FILE *file_ = nullptr;
  uint64_t remaining_ = 0, padding_ = 0;
  bool pax_ = false, paxGlobal_ = false, ended_ = false;
  std::string paxData_;
  int zeroBlocks_ = 0;
};

struct PatchRecord {
  std::string id, titleId, name, version, hash;
  // An app's patch: files the app already had at its paths are set aside
  // while it is on, not a conflict, and come back when it goes off.
  std::string homebrewApp;
  bool on = false;
  // Set while files move, so an interrupted enable or disable can finish.
  std::string pending; // "", "enable" or "disable"
  std::vector<std::string> files;  // moved into place when on
  std::vector<std::string> cheats; // merged into place when on
  uint64_t bytes = 0;
  int64_t order = 0; // enable order; cheats merge in this order
};
struct PatchConflict {
  std::string path, owner; // owner is a patch id, empty for the user's own file
};

// Downloaded patches live in a store on the SD card; enabling moves their
// files into place and a ledger remembers every path, so disabling takes back
// exactly those files and nothing the user put there.
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
  // is swapped for the new one and enabled again when nothing conflicts.
  bool commit(const UserPatch &patch, Error &error);
  std::vector<PatchConflict> conflicts(const std::string &id) const;
  bool enable(const std::string &id, Error &error);
  bool disable(const std::string &id, Error &error);
  bool remove(const std::string &id, Error &error);

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
  // An app's file in _base an app patch that is on (or moving) set aside.
  bool setAside(const std::string &path) const;
  std::string sdRoot_, storeRoot_;
  std::map<std::string, PatchRecord> records_;
  std::set<std::string> cheatPaths_; // cheat files we currently write
  int64_t order_ = 0;
};
} // namespace foil
