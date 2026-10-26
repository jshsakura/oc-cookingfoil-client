#pragma once
// The games 세이브 > 드롭박스 lists: by default the games with backups on
// Dropbox, installed or not (their backups can come down to the console and
// wait there until it is). Y adds this console's games that have something
// to upload.
#include "core/Catalog.hpp"
#include "core/Installed.hpp"
#include "net/Dropbox.hpp"
#include <ctime>
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
  std::time_t newest = 0; // its newest Dropbox backup
};
// Y on 드롭박스: what is on Dropbox, newest backup first or by name, or that
// with this console's games added.
enum class DropboxView { Latest, Name, WithConsole };
// A title id to its library game, or null when the catalog has none.
using GroupLookup = std::function<const Group *(const std::string &titleId)>;
// Whether an installed game has a save or a backup to upload.
using SaveLookup = std::function<bool(uint64_t applicationId)>;
// Latest: games on Dropbox, newest backup first. Name: the same by name.
// WithConsole: games with Dropbox backups first, then installed games that
// `hasSave` keeps (all of them without it), by name. `query` keeps the ones
// whose name or title id matches.
std::vector<DropboxRow> dropboxRows(const std::vector<InstalledGame> &games, const net::DropboxRevisions &remote,
                                    const GroupLookup &lookup, const std::string &query = "",
                                    DropboxView view = DropboxView::WithConsole, const SaveLookup &hasSave = nullptr);
} // namespace foil::ui
