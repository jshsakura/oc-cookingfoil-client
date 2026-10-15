// 세이브 > 드롭박스. Linking follows oc-save-keeper's bridge flow (the phone
// signs in from a QR code, the console polls and trades the code itself),
// and backups use its Dropbox layout, so either app sees the other's.
#include "ui/Application.hpp"
#include "core/Log.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
constexpr float Top = 90, AccountHeight = 72, ListTop = Top + AccountHeight + 12, RowHeight = 84, RowStep = 96;
constexpr int Visible = 5; // the account card above leaves room for five, down to the footer
constexpr uint32_t DefaultRetryMs = 2000, SignInMs = 15 * 60 * 1000; // the bridge keeps a sign-in 15 minutes
constexpr int PopupRows = 4;
std::string when(std::time_t at) {
  std::tm local{};
  localtime_r(&at, &local);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
  return text;
}
// Where the upload list starts: the newest backup made by hand, else the newest.
int newestBackup(const std::vector<saves::LocalBackup> &backups) {
  for (size_t i = 0; i < backups.size(); ++i)
    if (!backups[i].meta.isAutoBackup) return int(i);
  return 0;
}
} // namespace

bool Application::runDropboxJob(const std::string &busy, std::function<std::function<void()>()> job) {
  if (dropboxWorker_.joinable() || !dropbox_) return false;
  dropboxBusy_ = busy;
  dropboxWorker_ = std::thread([this, job = std::move(job)] {
    std::function<void()> finish;
    if (!guarded("dropbox", [&] { finish = job(); }))
      finish = [this] { dropboxError_.message = tr("Something went wrong with Dropbox. Check log.txt."); };
    dropboxFinish_ = std::move(finish);
    dropboxDone_ = true;
  });
  return true;
}

void Application::pollDropbox() {
  if (dropboxDone_.exchange(false)) {
    dropboxWorker_.join();
    dropboxBusy_.clear();
    if (dropboxFinish_) std::exchange(dropboxFinish_, nullptr)();
  }
  // A sign-in waiting for the phone is asked about while the tab is open,
  // as often as the server allows.
  if (!dropboxSession_ || dropboxWorker_.joinable() || screen_ != Screen::Saves || savesTab_ != 2 || !session_)
    return;
  const auto now = SDL_GetTicks();
  if (now >= dropboxExpiresAt_) {
    dropboxSession_.reset();
    dropboxError_.message = tr("The sign-in timed out. Connect again.");
    return;
  }
  if (now < dropboxPollAt_) return;
  const auto server = session_->server();
  const auto state = state_;
  const auto signIn = *dropboxSession_;
  runDropboxJob("status", [this, server, state, signIn]() -> std::function<void()> {
    int retry = 0;
    Error error;
    const auto status = dropbox_->status(server, state, signIn, retry, error);
    if (status != "approved") {
      return [this, status, retry] {
        if (status == "pending") {
          dropboxPollAt_ = SDL_GetTicks() + std::max<uint32_t>(DefaultRetryMs, uint32_t(retry) * 1000);
          return;
        }
        dropboxSession_.reset();
        dropboxError_.message = status == "expired" ? tr("The sign-in timed out. Connect again.")
                                                    : tr("The Dropbox sign-in was not approved.");
      };
    }
    // Approved: take the code and trade it on the same run.
    error = dropbox_->finish(server, state, signIn);
    return [this, error] {
      dropboxSession_.reset();
      if (error) {
        dropboxError_ = error;
        return;
      }
      dropboxLinked_ = dropbox_->linked();
      dropboxAccount_ = dropbox_->account();
      dropboxCursor_ = 0;
      message(tr("Dropbox is connected."));
      listDropbox();
    };
  });
}

void Application::dropboxEnter() {
  if (!dropbox_) {
    dropbox_ = std::make_unique<net::Dropbox>(sdRoot_ + "/switch/cookingfoil/cookingfoil.db", &stop_);
    // The link is read from cookingfoil.db (and decrypted) off the drawing
    // thread; the list follows once it is known.
    runDropboxJob("list", [this]() -> std::function<void()> {
      const bool linked = dropbox_->load();
      const auto account = dropbox_->account();
      return [this, linked, account] {
        dropboxLinked_ = linked;
        dropboxAccount_ = account;
        if (linked && !dropboxListed_) listDropbox();
      };
    });
    return;
  }
  if (dropboxLinked_ && !dropboxListed_) listDropbox();
}

void Application::startDropboxLink() {
  if (!session_ || !connected_) {
    message(tr("Dropbox connects once the server is connected."));
    return;
  }
  const auto server = session_->server();
  const auto state = state_;
  dropboxError_ = {};
  runDropboxJob("start", [this, server, state]() -> std::function<void()> {
    auto started = dropbox_->start(server, state);
    return [this, started] {
      dropboxMissing_ = started.missing;
      if (started.missing) return;
      if (started.error) {
        dropboxError_ = started.error;
        return;
      }
      dropboxSession_ = started.session;
      dropboxQr_.encode(started.session.authorizeUrl);
      const auto now = SDL_GetTicks();
      dropboxPollAt_ = now + std::max<uint32_t>(DefaultRetryMs, uint32_t(started.session.retryAfter) * 1000);
      dropboxExpiresAt_ = now + SignInMs;
    };
  });
}

void Application::listDropbox() {
  if (!dropboxLinked_) return;
  dropboxError_ = {};
  runDropboxJob("list", [this]() -> std::function<void()> {
    auto listed = dropbox_->revisions();
    const bool revoked = dropbox_->revoked();
    return [this, listed, revoked] {
      dropboxListed_ = true;
      if (revoked) {
        dropboxLinked_ = false;
        dropboxRemote_.clear();
      }
      if (listed.error) dropboxError_ = listed.error;
      else dropboxRemote_ = listed.value;
    };
  });
}

const std::vector<net::dropbox::Revision> &Application::dropboxRevisions(uint64_t id) const {
  static const std::vector<net::dropbox::Revision> none;
  const auto found = dropboxRemote_.find(id);
  return found == dropboxRemote_.end() ? none : found->second;
}

void Application::openDropboxGame() {
  const auto rows = dropboxGameRows();
  if (rows.empty()) return;
  dropboxGame_ = rows[dropboxFocus(rows)];
  dropboxLocal_ = saves::listBackups(saves::keeperRoot(sdRoot_), dropboxGame_.applicationId);
  dropboxPick_ = 0;
  dropboxOpen_ = true;
  dropboxChoosing_ = false;
  dropboxMetas_.clear();
  // Which console each Dropbox backup came from, from its small .meta.
  const auto remote = dropboxRemote_.find(dropboxGame_.applicationId);
  if (remote == dropboxRemote_.end() || remote->second.empty() || !dropbox_ || !dropbox_->linked()) return;
  std::vector<std::string> names;
  for (const auto &r : remote->second) names.push_back(r.name);
  const auto id = remote->first;
  const auto root = saves::keeperRoot(sdRoot_);
  runDropboxJob("meta", [this, id, names, root]() -> std::function<void()> {
    auto metas = net::revisionMetas(*dropbox_, id, names);
    const auto device = saves::loadDevice(root, std::time(nullptr), unsigned(std::clock()));
    const auto self = device.error ? std::string() : device.value.id;
    const bool revoked = dropbox_->revoked();
    return [this, metas = std::move(metas), self, revoked, id] {
      if (revoked) dropboxLinked_ = false;
      dropboxDevice_ = self;
      // Only for the game still open.
      if (dropboxOpen_ && dropboxGame_.applicationId == id) dropboxMetas_ = metas;
    };
  });
}

void Application::dropboxTransfer() {
  if (!dropboxOpen_ || dropboxWorker_.joinable()) return;
  const auto game = dropboxGame_;
  const auto root = saves::keeperRoot(sdRoot_);
  const auto &revisions = dropboxRevisions(game.applicationId);
  // The upload row (an installed game's only) opens the console's backups
  // to pick from, even one.
  if (game.installed && dropboxPick_ == 0) {
    if (dropboxLocal_.empty()) {
      message(tr("There is no backup to upload. Back up first in the This console tab."));
      return;
    }
    dropboxChoosing_ = true;
    dropboxUploadPick_ = newestBackup(dropboxLocal_);
    return;
  }
  const int picked = dropboxPick_ - (game.installed ? 1 : 0);
  if (picked < 0 || picked >= int(revisions.size())) return;
  const auto revision = revisions[picked].name;
  if (std::any_of(dropboxLocal_.begin(), dropboxLocal_.end(), [&](const auto &b) { return b.name == revision; })) {
    message(game.installed ? tr("This backup is already on the console. Restore it from the This console tab.")
                           : tr("This backup is already on the console. Install the game, then restore it from the This console tab."));
    return;
  }
  runDropboxJob("download", [this, root, game, revision]() -> std::function<void()> {
    auto got = net::downloadBackup(*dropbox_, root, game.applicationId, revision, std::time(nullptr));
    const bool revoked = dropbox_->revoked();
    return [this, got, revoked, root, id = game.applicationId, installed = game.installed] {
      if (revoked) dropboxLinked_ = false;
      if (got.error) {
        alertTitle_ = tr("Could not download from Dropbox");
        alert_ = got.error.message;
        return;
      }
      backupCounts_.erase(id);
      dropboxLocal_ = saves::listBackups(root, id);
      message(installed ? tr("Downloaded to the console. Restore it from the This console tab.")
                        : tr("Downloaded to the console. Install the game, then restore it from the This console tab."));
    };
  });
}

void Application::uploadDropbox(const saves::LocalBackup &backup) {
  // Only an installed game's backups go up.
  if (!dropboxOpen_ || !dropboxGame_.installed || dropboxWorker_.joinable()) return;
  const auto game = dropboxGame_;
  const auto root = saves::keeperRoot(sdRoot_);
  runDropboxJob("upload", [this, root, game, backup]() -> std::function<void()> {
    // The .meta written for a backup that has none, as save-keeper does.
    saves::BackupMeta meta;
    meta.titleId = game.applicationId;
    meta.titleName = game.name;
    meta.backupName = meta.revisionId = backup.name;
    meta.createdAt = backup.timestamp;
    meta.size = backup.size;
    meta.source = "local";
    const auto device = saves::loadDevice(root, std::time(nullptr), unsigned(std::clock()));
    if (!device.error) {
      meta.deviceId = device.value.id;
      meta.deviceLabel = device.value.label;
      meta.devicePriority = device.value.priority;
    }
    const auto error = net::uploadBackup(*dropbox_, root, game.applicationId, backup, meta);
    const bool revoked = dropbox_->revoked();
    return [this, error, revoked, name = backup.name] {
      if (revoked) dropboxLinked_ = false;
      if (error) {
        alertTitle_ = tr("Could not upload to Dropbox");
        alert_ = error.message;
        return;
      }
      dropboxChoosing_ = false;
      message(trf("Uploaded backup {0} to Dropbox.", {name}));
      listDropbox();
    };
  });
}

bool Application::dropboxFocusable() const {
  if (dropboxLinked_) return true;
  return connected_ && !dropboxMissing_ && !dropboxSession_;
}

void Application::dropboxAction(Action a) {
  if (dropboxUnlinkAsk_) {
    // One decision in the footer: A unlinks (not the press that opened it), B backs out.
    if (a == Action::Back) dropboxUnlinkAsk_ = false;
    if (a != Action::Accept || !settled() || dropboxWorker_.joinable()) return;
    dropboxUnlinkAsk_ = false;
    if (auto error = dropbox_->unlink()) {
      message(error.message);
      return;
    }
    dropboxLinked_ = false;
    dropboxAccount_.clear();
    dropboxRemote_.clear();
    dropboxListed_ = false;
    dropboxError_ = {};
    message(tr("Dropbox is disconnected."));
    return;
  }
  if (dropboxOpen_) {
    if (dropboxWorker_.joinable() && (dropboxBusy_ == "upload" || dropboxBusy_ == "download")) return;
    if (dropboxChoosing_) {
      const int last = int(dropboxLocal_.size()) - 1;
      if (a == Action::Back) dropboxChoosing_ = false;
      if (a == Action::Up) dropboxUploadPick_ = std::max(0, dropboxUploadPick_ - 1);
      if (a == Action::Down) dropboxUploadPick_ = std::min(last, dropboxUploadPick_ + 1);
      if (a == Action::Accept && last >= 0) uploadDropbox(dropboxLocal_[std::clamp(dropboxUploadPick_, 0, last)]);
      return;
    }
    const int last =
        std::max(0, int(dropboxRevisions(dropboxGame_.applicationId).size()) - (dropboxGame_.installed ? 0 : 1));
    if (a == Action::Back) dropboxOpen_ = false;
    if (a == Action::Up) dropboxPick_ = std::max(0, dropboxPick_ - 1);
    if (a == Action::Down) dropboxPick_ = std::min(last, dropboxPick_ + 1);
    if (a == Action::Accept) dropboxTransfer();
    return;
  }
  if (dropboxSession_) {
    // Waiting for the phone: B gives up on this sign-in.
    if (a == Action::Back) {
      dropboxSession_.reset();
      message(tr("Stopped connecting Dropbox."));
    }
    return;
  }
  if (a == Action::Reload) {
    if (dropboxLinked_ && !dropboxWorker_.joinable()) listDropbox();
    return;
  }
  if (!dropboxLinked_) {
    if (a == Action::Accept && !dropboxWorker_.joinable() && !dropboxMissing_) startDropboxLink();
    return;
  }
  // Y unlinks (it asks first); X searches as on the other tabs; A is only
  // ever about a game's backups.
  if (a == Action::Queue) {
    dropboxUnlinkAsk_ = true;
    return;
  }
  if (a == Action::Search) {
    requestSearch();
    return;
  }
  // The cursor is a game's application id, so it stays on that game when
  // the list reorders; up and down walk the games the search keeps.
  const auto rows = dropboxGameRows();
  if (rows.empty()) return;
  int pos = dropboxFocus(rows);
  if (a == Action::Up) pos = std::max(0, pos - 1);
  if (a == Action::Down) pos = std::min(int(rows.size()) - 1, pos + 1);
  dropboxCursor_ = rows[pos].applicationId;
  if (a == Action::Accept) openDropboxGame();
}

// Installed games and games with Dropbox backups, as the 세이브 search keeps them.
std::vector<DropboxRow> Application::dropboxGameRows() const {
  return dropboxRows(installed_.games, dropboxRemote_, [this](const std::string &id) { return findGroup(id); },
                     savesQuery_);
}
int Application::dropboxFocus(const std::vector<DropboxRow> &rows) const {
  for (int i = 0; i < int(rows.size()); ++i)
    if (rows[i].applicationId == dropboxCursor_) return i;
  return 0;
}
std::string Application::dropboxPopupAccept() const {
  if (!dropboxOpen_) return "";
  if (dropboxGame_.installed && dropboxPick_ == 0) return tr("Upload");
  return dropboxRevisions(dropboxGame_.applicationId).empty() ? "" : tr("Get");
}
void Application::dropboxTab() {
  dropboxEnter();
  if (!dropboxLinked_) {
    if (dropboxMissing_) {
      notice(tr("This server has no Dropbox set up."),
             tr("Once the server's admin registers a Dropbox app, you can connect here."), 25);
      return;
    }
    if (!dropboxSession_ && !connected_) {
      notice(tr("Not connected to a server."), tr("Dropbox connects once the server is connected."), 21);
      return;
    }
    if (!dropboxSession_ && inContent()) paint_.ring(260, Top, 992, 466, 20);
    paint_.rect(260, Top, 992, 466, Surface, 20);
    if (dropboxSession_) {
      // The phone opens Dropbox's sign-in from the QR code; this side waits.
      paint_.pill(tr("Awaiting approval"), 290, Top + 30, false, 17);
      paint_.fonts.text(tr("Sign in to Dropbox on your phone"), 290, Top + 86, 28, Ink, true, 580);
      paint_.fonts.wrapped(tr("Scan the QR code and tap Allow in Dropbox; this console finishes on its own. Your password is never kept on the console or the server."),
                           290, Top + 140, 21, Secondary, 570, 4);
      const auto left = (dropboxExpiresAt_ > SDL_GetTicks() ? dropboxExpiresAt_ - SDL_GetTicks() : 0) / 1000;
      paint_.fonts.text(trf("Approve within {0} min.", {std::to_string(left / 60 + 1)}), 290, Top + 290, 18, Secondary);
      dropboxQr_.draw(paint_.renderer, 908, int(Top + 75), 300);
      return;
    }
    // The icon beside the title, what it is for in one line, the three
    // steps with numbered dots, and the button the first step names.
    constexpr float Left = 300, TitleMid = Top + 62;
    paint_.icon(36, Left, TitleMid - 20, Ink, 40);
    paint_.fonts.middle(tr("Back up saves to Dropbox"), Left + 58, TitleMid, 28, Ink, true, 860);
    paint_.fonts.middle(tr("Upload this console's save backups, and restore ones uploaded from other consoles."), Left, TitleMid + 58,
                        20, Secondary, false, 920);
    const char *steps[] = {"1. Press Connect below.", "2. Scan the QR code with your phone and sign in to Dropbox.",
                           "3. Tap Allow, and this console finishes connecting on its own."};
    for (int i = 0; i < 3; ++i) {
      const float mid = Top + 188 + i * 50;
      paint_.rect(Left, mid - 15, 30, 30, Butter, 15);
      paint_.fonts.centered(std::to_string(i + 1), Left + 15, mid - 13, 17, Ink, 30, 1, true);
      // The number is the dot's; the step's own "1. " is left off.
      std::string step = tr(steps[i]);
      if (const auto dot = step.find(". "); dot != std::string::npos && dot <= 2) step = step.substr(dot + 2);
      paint_.fonts.middle(step, Left + 46, mid, 20, Ink, false, 880);
    }
    constexpr float ButtonY = Top + 346, ButtonH = 60;
    const bool starting = dropboxBusy_ == "start";
    const float buttonW = paint_.keyLabelWidth("A", tr("Connect"), 21, true) + 64;
    paint_.rect(Left, ButtonY, buttonW, ButtonH, starting ? Line : Butter, 14);
    paint_.keyLabel("A", tr("Connect"), Left + 32, ButtonY + ButtonH / 2, 21, Ink, true, !starting);
    // A connects; a tap on the card does the same.
    hit(260, Top, 992, 466, [this] {
      sidebar_ = false;
      action(Action::Accept);
    });
    const float noteX = Left + buttonW + 28;
    if (starting) paint_.fonts.middle(tr("Getting ready to connect"), noteX, ButtonY + ButtonH / 2, 19, Secondary, true, 1220 - noteX);
    else if (dropboxError_)
      paint_.fonts.wrapped(dropboxError_.message, noteX, ButtonY + 6, 18, Bad, 1220 - noteX, 2);
    return;
  }
  // The account; X (shown on the card) unlinks, after asking.
  paint_.rect(260, Top, 992, AccountHeight, Surface, 16);
  paint_.icon(36, 282, Top + 23, Ink, 26);
  paint_.fonts.text(dropboxAccount_.empty() ? std::string(tr("Dropbox is connected.")) : dropboxAccount_, 324, Top + 10,
                    20, Ink, true, 640);
  int uploaded = 0;
  for (const auto &[id, revisions] : dropboxRemote_) uploaded += int(revisions.size());
  const std::string status = dropboxBusy_ == "list"  ? std::string(tr("Loading the Dropbox list."))
                             : dropboxError_         ? dropboxError_.message
                                                     : trf("{0} backups on Dropbox", {std::to_string(uploaded)});
  paint_.fonts.text(status, 324, Top + 40, 16, dropboxError_ && dropboxBusy_.empty() ? Bad : Secondary, false, 640);
  const std::string unlink = tr("Disconnect");
  const float uw = paint_.keyLabelWidth("Y", unlink, 17, true);
  paint_.keyLabel("Y", unlink, 1230 - uw, Top + AccountHeight / 2, 17, Secondary, true);
  hit(1230 - uw - 8, Top + 8, uw + 16, AccountHeight - 16, [this] { action(Action::Queue); });
  // Nothing at all to list yet: the console may still be reading its games.
  if (installed_.games.empty() && dropboxRemote_.empty()) {
    if (deviceBusy_)
      for (int n = 0; n < Visible; ++n) paint_.skeleton(260, ListTop + n * RowStep, 992, RowHeight, 16);
    else
      paint_.fonts.text(tr("No games to manage saves for."), 290, ListTop + 20, 19, Secondary);
    return;
  }
  const auto rows = dropboxGameRows();
  if (rows.empty()) {
    paint_.fonts.text(trf("No games match '{0}'.", {savesQuery_}), 290, ListTop + 20, 19, Secondary);
    return;
  }
  const int count = int(rows.size());
  const int focus = dropboxFocus(rows);
  dropboxCursor_ = rows[focus].applicationId;
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("dropbox", 232, ListTop, 1048, 660 - (ListTop), float(std::clamp(focus - Visible / 2, 0, std::max(0, count - Visible))),
                          float((count) - (Visible)), RowStep);
  const int first = int(top);
  paint_.scrollbar(1266, ListTop, RowStep * Visible - 12, top, Visible, float(count));
  const auto root = saves::keeperRoot(sdRoot_);
  clipTo(232, ListTop - 8, 1048, 660 - ListTop + 8);
  for (int n = first; n < count && n <= first + Visible + 1; ++n) {
    const auto &g = rows[n];
    const auto id = g.applicationId;
    const float y = ListTop + (n - top) * RowStep;
    hit(260, y, 992, RowHeight, [this, id] {
      if (inContent() && dropboxCursor_ == id) action(Action::Accept);
      sidebar_ = false;
      dropboxCursor_ = id;
    });
    if (inContent() && n == focus && !dropboxModal()) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    const auto icon = g.installed ? installedIcon(installed_.games[g.game]) : g.icon;
    cover(icon, 278, y + 10, 64, false, g.name);
    // A game the shop does not know is named by its title id, shown once.
    const bool named = g.name != g.titleId;
    if (inContent() && n == focus && !dropboxModal()) marquee(g.name, 360, named ? y + 12 : y + 28, 21, 500);
    else paint_.fonts.text(g.name, 360, named ? y + 12 : y + 28, 21, Ink, true, 500);
    if (named) paint_.fonts.text(g.titleId, 360, y + 46, 16, Secondary);
    auto counted = backupCounts_.find(id);
    if (counted == backupCounts_.end()) counted = backupCounts_.emplace(id, saves::countBackups(root, id)).first;
    const int local = counted->second;
    // Right to left: what Dropbox holds, what the console holds, and a game
    // that is not installed says so.
    float right = 1230;
    const auto badge = [&](const std::string &text, bool on) {
      const float w = paint_.fonts.measure(text, 16, true) + 20;
      right -= w;
      paint_.rect(right, y + 28, w, 28, on ? Good : Line, 8);
      paint_.fonts.middle(text, right + 10, y + 42, 16, on ? Surface : Ink, true);
      right -= 8;
    };
    badge(dropboxListed_ ? trf("Dropbox {0}", {std::to_string(g.remote)}) : std::string(tr("Checking Dropbox")),
          g.remote > 0);
    badge(trf("Console {0}|count", {std::to_string(local)}), local > 0);
    if (!g.installed) badge(tr("Game not installed"), false);
  }
  unclip();
}

void Application::dropboxDialog() {
  if (dropboxUnlinkAsk_) {
    const auto body = modal(700, 20 + 30 + 84 + 16, tr("Disconnect Dropbox?"), tr("Cancel"), "", tr("Disconnect|confirm"), 25);
    paint_.fonts.text(dropboxAccount_.empty() ? std::string(tr("Dropbox")) : dropboxAccount_, body.x + 32, body.y + 20, 21,
                      Ink, true, body.w - 64);
    paint_.fonts.wrapped(tr("Removes the Dropbox link from this console. Backups on Dropbox stay there and show again when you reconnect."),
                         body.x + 32, body.y + 56, 19, Secondary, body.w - 64, 3);
    return;
  }
  if (!dropboxOpen_) return;
  if (dropboxChoosing_) {
    dropboxUploadList();
    return;
  }
  const auto &game = dropboxGame_;
  const auto &revisions = dropboxRevisions(game.applicationId);
  constexpr float Row = 72, Gap = 8;
  // An installed game's first row uploads; a game not installed only
  // takes its Dropbox backups down, to restore once it is installed.
  const int upload = game.installed ? 1 : 0;
  const int total = int(revisions.size()) + upload;
  const int rows = std::min(total, PopupRows + 1);
  const bool working = dropboxBusy_ == "upload" || dropboxBusy_ == "download";
  dropboxPick_ = std::clamp(dropboxPick_, 0, std::max(0, total - 1));
  const float note = game.installed ? 0 : 30;
  const auto body = modal(860, 24 + rows * (Row + Gap) + 40 + note, game.name, working ? "" : tr("Close"), "",
                          dropboxPopupAccept(), 36);
  const float x = body.x + 32, w = body.w - 64;
  // The row at the top follows the cursor, or the finger while it glides.
  const float topRow = glide("dropboxPick", x, body.y + 24, w, rows * (Row + Gap), float(std::clamp(dropboxPick_ - rows / 2, 0, std::max(0, total - rows))),
                          float((total) - (rows)), (Row + Gap));
  const int top = int(topRow);
  clipTo(x, body.y + 24, w, rows * (Row + Gap));
  for (int n = top; n < total && n <= top + rows; ++n) {
    const float y = body.y + 24 + (n - topRow) * (Row + Gap);
    hit(x, y, w, Row, [this, n] {
      dropboxPick_ = n;
      action(Action::Accept);
    });
    if (n == dropboxPick_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    if (n < upload) {
      paint_.icon(36, x + 20, y + 23, Ink, 26);
      paint_.fonts.text(tr("Pick a backup to upload to Dropbox"), x + 60, y + 10, 20, Ink, true, w - 80);
      const std::string detail = dropboxLocal_.empty()
                                     ? std::string(tr("No backups on the console. Back up first in the This console tab."))
                                     : trf("Console {0}|count", {std::to_string(dropboxLocal_.size())});
      paint_.fonts.text(detail, x + 60, y + 40, 16, Secondary, false, w - 80);
      continue;
    }
    const auto &r = revisions[n - upload];
    const bool here = std::any_of(dropboxLocal_.begin(), dropboxLocal_.end(), [&](const auto &b) { return b.name == r.name; });
    paint_.fonts.text(r.name, x + 20, y + 10, 20, Ink, true, w - 220);
    // When and from which console: Dropbox holds every console's uploads.
    std::string from = trf("Dropbox · {0}", {when(r.modified)});
    const auto meta = dropboxMetas_.find(r.name);
    if (meta != dropboxMetas_.end()) {
      const auto &m = meta->second;
      from = when(m.createdAt ? m.createdAt : r.modified) + " · " +
             (!dropboxDevice_.empty() && m.deviceId == dropboxDevice_ ? std::string(tr("This console"))
              : m.deviceLabel.empty()                                  ? std::string(tr("Unknown console"))
                                                                       : trf("Console {0}", {m.deviceLabel}));
      if (!m.userName.empty()) from += " · " + m.userName;
    } else if (dropboxBusy_ == "meta") {
      from += " · " + std::string(tr("Checking console"));
    }
    paint_.fonts.text(from, x + 20, y + 40, 16, Secondary, false, w - 220);
    const std::string side = here ? std::string(tr("On the console")) : formatBytes(r.size);
    paint_.fonts.middle(side, x + w - 20 - paint_.fonts.measure(side, 17, true), y + Row / 2, 17, here ? Good : Ink, true);
  }
  unclip();
  const float line = body.y + 24 + rows * (Row + Gap) + 4;
  if (working)
    paint_.fonts.text(dropboxBusy_ == "upload" ? tr("Uploading to Dropbox.") : tr("Downloading from Dropbox."), x, line,
                      18, Secondary);
  else if (revisions.empty() && dropboxListed_)
    paint_.fonts.text(tr("Dropbox has no backups of this game."), x, line, 18, Secondary);
  else if (revisions.empty())
    paint_.skeleton(x, line - 2, 420, 22, 6); // the Dropbox list is on its way
  else if (!game.installed)
    paint_.fonts.wrapped(tr("The game isn't installed, so backups only come down for now. Install it, then restore from the This console tab."), x,
                         line, 18, Secondary, w, 2);
}
// The console's backups of the open game: A sends the one picked.
void Application::dropboxUploadList() {
  const auto &game = dropboxGame_;
  const auto remote = dropboxRemote_.find(game.applicationId);
  const int total = int(dropboxLocal_.size());
  if (!total) return;
  constexpr float Row = 72, Gap = 8;
  const int rows = std::min(total, PopupRows + 1);
  const bool working = dropboxBusy_ == "upload";
  dropboxUploadPick_ = std::clamp(dropboxUploadPick_, 0, total - 1);
  const auto body = modal(860, 24 + rows * (Row + Gap) + 40, game.name, working ? "" : tr("Back"), "", tr("Upload"), 36);
  const float x = body.x + 32, w = body.w - 64;
  // The row at the top follows the cursor, or the finger while it glides.
  const float topRow = glide("dropboxUpload", x, body.y + 24, w, rows * (Row + Gap), float(std::clamp(dropboxUploadPick_ - rows / 2, 0, std::max(0, total - rows))),
                          float((total) - (rows)), (Row + Gap));
  const int top = int(topRow);
  clipTo(x, body.y + 24, w, rows * (Row + Gap));
  for (int n = top; n < total && n <= top + rows; ++n) {
    const auto &b = dropboxLocal_[n];
    const float y = body.y + 24 + (n - topRow) * (Row + Gap);
    hit(x, y, w, Row, [this, n] {
      dropboxUploadPick_ = n;
      action(Action::Accept);
    });
    if (n == dropboxUploadPick_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.fonts.text(when(b.timestamp), x + 20, y + 10, 20, Ink, true, w - 220);
    paint_.fonts.text(backupDetail(b), x + 20, y + 40, 16, Secondary, false, w - 220);
    // One already on Dropbox is overwritten by the upload.
    const bool there = remote != dropboxRemote_.end() &&
                       std::any_of(remote->second.begin(), remote->second.end(), [&](const auto &r) { return r.name == b.name; });
    const std::string side = there ? std::string(tr("On Dropbox")) : formatBytes(uint64_t(b.size));
    paint_.fonts.middle(side, x + w - 20 - paint_.fonts.measure(side, 17, true), y + Row / 2, 17, there ? Good : Ink, true);
  }
  unclip();
  const float line = body.y + 24 + rows * (Row + Gap) + 4;
  paint_.fonts.text(working ? tr("Uploading to Dropbox.") : tr("A backup already on Dropbox is replaced under the same name."), x, line, 18,
                    Secondary);
}
} // namespace foil::ui
