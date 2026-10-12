#pragma once
// The games 세이브 > 드롭박스 lists: every installed game, and every game
// with backups on Dropbox even when it is not installed (its backups can
// come down to the console and wait there until it is).
#include "core/Catalog.hpp"
#include "core/Installed.hpp"
#include "net/Dropbox.hpp"
#include <functional>
#include <string>
#include <vector>
namespace foil::ui {
struct DropboxRow {
  uint64_t applicationId = 0;
  std::string titleId, name;
  std::string icon; // the catalog's icon, for a game not installed
  bool installed = false;
  int game = -1;  // its index in the installed games, when installed
  int remote = 0; // its backups on Dropbox
};
// A title id to its shop game, or null when the catalog has none.
using GroupLookup = std::function<const Group *(const std::string &titleId)>;
// Games with Dropbox backups first, then by name; `query` keeps the ones
// whose name or title id matches.
std::vector<DropboxRow> dropboxRows(const std::vector<InstalledGame> &games, const net::DropboxRevisions &remote,
                                    const GroupLookup &lookup, const std::string &query = "");
} // namespace foil::ui
