#include "ui/DropboxRows.hpp"
#include "core/SaveBackups.hpp"
#include <algorithm>
#include <cctype>
#include <set>
namespace foil::ui {
namespace {
std::string lowered(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}
const std::vector<net::dropbox::Revision> *backupsOn(const net::DropboxRevisions &remote, uint64_t id) {
  const auto found = remote.find(id);
  return found == remote.end() || found->second.empty() ? nullptr : &found->second;
}
} // namespace

std::vector<DropboxRow> dropboxRows(const std::vector<InstalledGame> &games, const net::DropboxRevisions &remote,
                                    const GroupLookup &lookup, const std::string &query, DropboxView view,
                                    const SaveLookup &hasSave) {
  std::vector<DropboxRow> rows;
  std::set<uint64_t> seen;
  for (int i = 0; i < int(games.size()); ++i) {
    const auto &g = games[i];
    const auto *backups = backupsOn(remote, g.applicationId);
    // Off Dropbox, a game is listed only with this console's games added,
    // and only when it has something to upload.
    if (!backups && (view != DropboxView::WithConsole || (hasSave && !hasSave(g.applicationId)))) continue;
    if (!seen.insert(g.applicationId).second) continue;
    rows.push_back({g.applicationId, g.titleId, g.name, "", true, i, backups ? int(backups->size()) : 0,
                    backups ? backups->front().modified : 0});
  }
  for (const auto &[id, revisions] : remote) {
    if (revisions.empty() || !seen.insert(id).second) continue;
    DropboxRow row{id, saves::titleHex(id), "", "", false, -1, int(revisions.size()), revisions.front().modified};
    const Group *group = lookup ? lookup(row.titleId) : nullptr;
    row.name = group ? group->representative.name : row.titleId;
    if (group) row.icon = group->representative.icon;
    rows.push_back(std::move(row));
  }
  rows.erase(std::remove_if(rows.begin(), rows.end(),
                            [&](const DropboxRow &r) { return !textMatches({r.name, r.titleId}, query); }),
             rows.end());
  std::stable_sort(rows.begin(), rows.end(), [view](const DropboxRow &a, const DropboxRow &b) {
    if ((a.remote > 0) != (b.remote > 0)) return a.remote > 0;
    if (view == DropboxView::Latest && a.newest != b.newest) return a.newest > b.newest;
    const auto an = lowered(a.name), bn = lowered(b.name);
    return an != bn ? an < bn : a.titleId < b.titleId;
  });
  return rows;
}
} // namespace foil::ui
