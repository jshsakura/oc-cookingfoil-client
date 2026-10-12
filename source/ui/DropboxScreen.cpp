// 세이브 > 드롭박스. Linking follows oc-save-keeper's bridge flow (the phone
// signs in from a QR code, the console polls and trades the code itself),
// and backups use its Dropbox layout, so either app sees the other's.
#include "ui/Application.hpp"
#include "core/Log.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
constexpr float Top = 90, AccountHeight = 72, ListTop = Top + AccountHeight + 12, RowHeight = 84, RowStep = 96;
constexpr int Visible = 4;
constexpr uint32_t DefaultRetryMs = 2000, SignInMs = 15 * 60 * 1000; // the bridge keeps a sign-in 15 minutes
constexpr int PopupRows = 4;
std::string when(std::time_t at) {
  std::tm local{};
  localtime_r(&at, &local);
  char text[32];
  std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
  return text;
}
// The backup a game's upload sends: the newest made by hand, else the newest.
const saves::LocalBackup *newestBackup(const std::vector<saves::LocalBackup> &backups) {
  for (const auto &b : backups)
    if (!b.meta.isAutoBackup) return &b;
  return backups.empty() ? nullptr : &backups.front();
}
} // namespace

bool Application::runDropboxJob(const std::string &busy, std::function<std::function<void()>()> job) {
  if (dropboxWorker_.joinable() || !dropbox_) return false;
  dropboxBusy_ = busy;
  dropboxWorker_ = std::thread([this, job = std::move(job)] {
    std::function<void()> finish;
    if (!guarded("dropbox", [&] { finish = job(); }))
      finish = [this] { dropboxError_.message = tr("드롭박스 작업 중 오류가 났습니다. log.txt를 확인합니다."); };
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
    dropboxError_.message = tr("로그인 시간이 지났습니다. 다시 연결합니다.");
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
        dropboxError_.message = status == "expired" ? tr("로그인 시간이 지났습니다. 다시 연결합니다.")
                                                    : tr("드롭박스 로그인이 승인되지 않았습니다.");
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
      dropboxRow_ = 0;
      message(tr("드롭박스에 연결했습니다."));
      listDropbox();
    };
  });
}

void Application::dropboxEnter() {
  if (!dropbox_) {
    dropbox_ = std::make_unique<net::Dropbox>(sdRoot_ + "/switch/cookingfoil/dropbox.json", &stop_);
    dropboxLinked_ = dropbox_->load();
    dropboxAccount_ = dropbox_->account();
  }
  if (dropboxLinked_ && !dropboxListed_) listDropbox();
}

void Application::startDropboxLink() {
  if (!session_ || !connected_) {
    message(tr("서버에 연결한 뒤 드롭박스를 연결합니다."));
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

void Application::openDropboxGame() {
  if (dropboxRow_ < 0 || dropboxRow_ >= int(installed_.games.size())) return;
  dropboxLocal_ = saves::listBackups(saves::keeperRoot(sdRoot_), installed_.games[dropboxRow_].applicationId);
  dropboxPick_ = 0;
  dropboxOpen_ = true;
  dropboxMetas_.clear();
  // Which console each Dropbox backup came from, from its small .meta.
  const auto remote = dropboxRemote_.find(installed_.games[dropboxRow_].applicationId);
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
      if (dropboxOpen_ && dropboxRow_ >= 0 && dropboxRow_ < int(installed_.games.size()) &&
          installed_.games[dropboxRow_].applicationId == id)
        dropboxMetas_ = metas;
    };
  });
}

void Application::dropboxTransfer() {
  if (!dropboxOpen_ || dropboxRow_ < 0 || dropboxRow_ >= int(installed_.games.size())) return;
  if (dropboxWorker_.joinable()) return;
  const auto game = installed_.games[dropboxRow_];
  const auto root = saves::keeperRoot(sdRoot_);
  const auto remote = dropboxRemote_.find(game.applicationId);
  if (dropboxPick_ == 0) {
    const auto *newest = newestBackup(dropboxLocal_);
    if (!newest) {
      message(tr("올릴 백업이 없습니다. 내 기기 탭에서 먼저 백업합니다."));
      return;
    }
    const auto backup = *newest;
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
          alertTitle_ = tr("드롭박스에 올리지 못했습니다");
          alert_ = error.message;
          return;
        }
        message(trf("{0} 백업을 드롭박스에 올렸습니다.", {name}));
        listDropbox();
      };
    });
    return;
  }
  if (remote == dropboxRemote_.end() || dropboxPick_ > int(remote->second.size())) return;
  const auto revision = remote->second[dropboxPick_ - 1].name;
  if (std::any_of(dropboxLocal_.begin(), dropboxLocal_.end(), [&](const auto &b) { return b.name == revision; })) {
    message(tr("이 백업은 이미 내 기기에 있습니다. 내 기기 탭에서 복원합니다."));
    return;
  }
  runDropboxJob("download", [this, root, game, revision]() -> std::function<void()> {
    auto got = net::downloadBackup(*dropbox_, root, game.applicationId, revision, std::time(nullptr));
    const bool revoked = dropbox_->revoked();
    return [this, got, revoked, root, id = game.applicationId] {
      if (revoked) dropboxLinked_ = false;
      if (got.error) {
        alertTitle_ = tr("드롭박스에서 받지 못했습니다");
        alert_ = got.error.message;
        return;
      }
      backupCounts_.erase(id);
      dropboxLocal_ = saves::listBackups(root, id);
      message(tr("내 기기로 받았습니다. 내 기기 탭에서 이 백업으로 복원합니다."));
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
    message(tr("드롭박스 연결을 해제했습니다."));
    return;
  }
  if (dropboxOpen_) {
    if (dropboxWorker_.joinable() && (dropboxBusy_ == "upload" || dropboxBusy_ == "download")) return;
    const auto remote = dropboxRow_ >= 0 && dropboxRow_ < int(installed_.games.size())
                            ? dropboxRemote_.find(installed_.games[dropboxRow_].applicationId)
                            : dropboxRemote_.end();
    const int last = remote == dropboxRemote_.end() ? 0 : int(remote->second.size());
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
      message(tr("드롭박스 연결을 취소했습니다."));
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
  // The cursor is a game's index; up and down walk the games the search keeps.
  const auto rows = dropboxGameRows();
  if (rows.empty()) return;
  auto at = std::find(rows.begin(), rows.end(), dropboxRow_);
  int pos = at == rows.end() ? 0 : int(at - rows.begin());
  if (a == Action::Up) pos = std::max(0, pos - 1);
  if (a == Action::Down) pos = std::min(int(rows.size()) - 1, pos + 1);
  dropboxRow_ = rows[pos];
  if (a == Action::Accept) openDropboxGame();
}

// The installed games the 세이브 search keeps, as indexes into the list.
std::vector<int> Application::dropboxGameRows() const {
  std::vector<int> rows;
  for (int i = 0; i < int(installed_.games.size()); ++i)
    if (textMatches({installed_.games[i].name, installed_.games[i].titleId}, savesQuery_)) rows.push_back(i);
  return rows;
}
void Application::dropboxTab() {
  dropboxEnter();
  if (!dropboxLinked_) {
    if (dropboxMissing_) {
      notice(tr("서버에 드롭박스가 설정되어 있지 않습니다."),
             tr("서버 관리자가 드롭박스 앱을 등록하면 여기서 연결합니다."), 25);
      return;
    }
    if (!dropboxSession_ && !connected_) {
      notice(tr("서버에 연결되어 있지 않습니다."), tr("서버에 연결한 뒤 드롭박스를 연결합니다."), 21);
      return;
    }
    if (!dropboxSession_ && inContent()) paint_.ring(260, Top, 992, 466, 20);
    paint_.rect(260, Top, 992, 466, Surface, 20);
    if (dropboxSession_) {
      // The phone opens Dropbox's sign-in from the QR code; this side waits.
      paint_.pill(tr("승인 대기"), 290, Top + 30, false, 17);
      paint_.fonts.text(tr("휴대폰으로 드롭박스에 로그인합니다"), 290, Top + 86, 28, Ink, true, 580);
      paint_.fonts.wrapped(tr("QR 코드를 찍어 드롭박스에서 허용을 누르면 이 본체가 알아서 연결을 마칩니다. 비밀번호는 본체나 서버에 남지 않습니다."),
                           290, Top + 140, 21, Secondary, 570, 4);
      const auto left = (dropboxExpiresAt_ > SDL_GetTicks() ? dropboxExpiresAt_ - SDL_GetTicks() : 0) / 1000;
      paint_.fonts.text(trf("{0}분 안에 승인합니다.", {std::to_string(left / 60 + 1)}), 290, Top + 290, 18, Secondary);
      dropboxQr_.draw(paint_.renderer, 908, int(Top + 75), 300);
      return;
    }
    paint_.icon(36, 290, Top + 32, Ink, 40);
    paint_.fonts.text(tr("드롭박스에 세이브를 백업합니다"), 290, Top + 96, 28, Ink, true, 900);
    paint_.fonts.wrapped(tr("드롭박스를 연결하면 이 본체의 세이브 백업을 올리고, 다른 본체나 oc-save-keeper가 올린 백업을 받아 내 기기 탭에서 복원합니다."),
                         290, Top + 150, 21, Secondary, 900, 3);
    paint_.fonts.wrapped(tr("연결하기를 누르면 QR 코드가 나옵니다. 휴대폰으로 찍어 드롭박스에 로그인합니다."), 290, Top + 240,
                         19, Secondary, 900, 2);
    // A in the footer connects; a tap on the card does the same.
    hit(260, Top, 992, 466, [this] {
      sidebar_ = false;
      action(Action::Accept);
    });
    if (dropboxBusy_ == "start") paint_.fonts.text(tr("연결을 준비하고 있습니다"), 290, Top + 330, 19, Ink, true);
    if (dropboxError_) paint_.fonts.wrapped(dropboxError_.message, 290, Top + 370, 18, Bad, 900, 2);
    return;
  }
  // The account; X (shown on the card) unlinks, after asking.
  paint_.rect(260, Top, 992, AccountHeight, Surface, 16);
  paint_.icon(36, 282, Top + 23, Ink, 26);
  paint_.fonts.text(dropboxAccount_.empty() ? std::string(tr("드롭박스에 연결했습니다.")) : dropboxAccount_, 324, Top + 10,
                    20, Ink, true, 640);
  int uploaded = 0;
  for (const auto &[id, revisions] : dropboxRemote_) uploaded += int(revisions.size());
  const std::string status = dropboxBusy_ == "list"  ? std::string(tr("드롭박스 목록을 불러오고 있습니다."))
                             : dropboxError_         ? dropboxError_.message
                                                     : trf("드롭박스에 백업 {0}개", {std::to_string(uploaded)});
  paint_.fonts.text(status, 324, Top + 40, 16, dropboxError_ && dropboxBusy_.empty() ? Bad : Secondary, false, 640);
  const std::string unlink = tr("연결 해제");
  const float uw = paint_.keyLabelWidth("Y", unlink, 17, true);
  paint_.keyLabel("Y", unlink, 1230 - uw, Top + AccountHeight / 2, 17, Secondary, true);
  hit(1230 - uw - 8, Top + 8, uw + 16, AccountHeight - 16, [this] { action(Action::Queue); });
  if (installed_.games.empty()) {
    // Still reading the console: the rows' shape, pulsing.
    if (deviceBusy_)
      for (int n = 0; n < Visible; ++n) paint_.skeleton(260, ListTop + n * RowStep, 992, RowHeight, 16);
    else
      paint_.fonts.text(tr("세이브를 관리할 게임이 없습니다."), 290, ListTop + 20, 19, Secondary);
    return;
  }
  const auto rows = dropboxGameRows();
  if (rows.empty()) {
    paint_.fonts.text(trf("'{0}'에 맞는 게임이 없습니다.", {savesQuery_}), 290, ListTop + 20, 19, Secondary);
    return;
  }
  if (std::find(rows.begin(), rows.end(), dropboxRow_) == rows.end()) dropboxRow_ = rows.front();
  const int count = int(rows.size());
  const int focus = int(std::find(rows.begin(), rows.end(), dropboxRow_) - rows.begin());
  const int first = std::clamp(focus - Visible / 2, 0, std::max(0, count - Visible));
  paint_.scrollbar(1266, ListTop, RowStep * Visible - 12, float(first), Visible, float(count));
  const auto root = saves::keeperRoot(sdRoot_);
  for (int n = first; n < count && n < first + Visible; ++n) {
    const int i = rows[n];
    const auto &g = installed_.games[i];
    const float y = ListTop + (n - first) * RowStep;
    hit(260, y, 992, RowHeight, [this, i] {
      if (inContent() && dropboxRow_ == i) action(Action::Accept);
      sidebar_ = false;
      dropboxRow_ = i;
    });
    if (inContent() && i == dropboxRow_ && !dropboxModal()) paint_.ring(260, y, 992, RowHeight, 16);
    paint_.rect(260, y, 992, RowHeight, Surface, 16);
    std::string icon = "installed:" + g.titleId;
    if (!g.iconBytes.empty()) images_->provide(icon, g.iconBytes);
    else {
      const auto *match = findGroup(g.titleId);
      icon = match ? match->representative.icon : "";
    }
    cover(icon, 278, y + 10, 64, false, g.name);
    paint_.fonts.text(g.name, 360, y + 12, 21, Ink, true, 560);
    paint_.fonts.text(g.titleId, 360, y + 46, 16, Secondary);
    auto counted = backupCounts_.find(g.applicationId);
    if (counted == backupCounts_.end())
      counted = backupCounts_.emplace(g.applicationId, saves::countBackups(root, g.applicationId)).first;
    const auto remote = dropboxRemote_.find(g.applicationId);
    const int local = counted->second, cloud = remote == dropboxRemote_.end() ? 0 : int(remote->second.size());
    // Right to left: what Dropbox holds, then what the console holds.
    float right = 1230;
    const auto badge = [&](const std::string &text, bool on) {
      const float w = paint_.fonts.measure(text, 16, true) + 20;
      right -= w;
      paint_.rect(right, y + 28, w, 28, on ? Good : Line, 8);
      paint_.fonts.middle(text, right + 10, y + 42, 16, on ? Surface : Ink, true);
      right -= 8;
    };
    badge(dropboxListed_ ? trf("드롭박스 {0}개", {std::to_string(cloud)}) : std::string(tr("드롭박스 확인 중")), cloud > 0);
    badge(trf("내 기기 {0}개", {std::to_string(local)}), local > 0);
  }
}

void Application::dropboxDialog() {
  if (dropboxUnlinkAsk_) {
    const auto body = modal(700, 20 + 30 + 84 + 16, tr("드롭박스 연결을 해제합니까?"), tr("취소"), "", tr("해제합니다"), 25);
    paint_.fonts.text(dropboxAccount_.empty() ? std::string(tr("드롭박스")) : dropboxAccount_, body.x + 32, body.y + 20, 21,
                      Ink, true, body.w - 64);
    paint_.fonts.wrapped(tr("이 본체에서 드롭박스 연결 정보를 지웁니다. 드롭박스에 올린 백업은 그대로 남고, 다시 연결하면 또 보입니다."),
                         body.x + 32, body.y + 56, 19, Secondary, body.w - 64, 3);
    return;
  }
  if (!dropboxOpen_ || dropboxRow_ < 0 || dropboxRow_ >= int(installed_.games.size())) return;
  const auto &game = installed_.games[dropboxRow_];
  const auto remote = dropboxRemote_.find(game.applicationId);
  static const std::vector<net::dropbox::Revision> none;
  const auto &revisions = remote == dropboxRemote_.end() ? none : remote->second;
  constexpr float Row = 72, Gap = 8;
  const int total = int(revisions.size()) + 1;
  const int rows = std::min(total, PopupRows + 1);
  const bool working = dropboxBusy_ == "upload" || dropboxBusy_ == "download";
  dropboxPick_ = std::clamp(dropboxPick_, 0, total - 1);
  const auto body = modal(860, 24 + rows * (Row + Gap) + 40, game.name, working ? "" : tr("닫기"), "",
                          dropboxPick_ == 0 ? tr("올리기") : tr("받기"), 36);
  const float x = body.x + 32, w = body.w - 64;
  const int top = std::clamp(dropboxPick_ - rows / 2, 0, std::max(0, total - rows));
  const auto *newest = newestBackup(dropboxLocal_);
  for (int n = top; n < top + rows; ++n) {
    const float y = body.y + 24 + (n - top) * (Row + Gap);
    hit(x, y, w, Row, [this, n] {
      dropboxPick_ = n;
      action(Action::Accept);
    });
    if (n == dropboxPick_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    if (n == 0) {
      paint_.icon(36, x + 20, y + 23, Ink, 26);
      paint_.fonts.text(tr("최신 백업을 드롭박스에 올립니다"), x + 60, y + 10, 20, Ink, true, w - 80);
      const bool there = newest && std::any_of(revisions.begin(), revisions.end(),
                                                [&](const auto &r) { return r.name == newest->name; });
      const std::string detail = !newest ? std::string(tr("내 기기에 백업이 없습니다. 내 기기 탭에서 먼저 백업합니다."))
                                 : there ? trf("{0} · 이미 드롭박스에 있어 덮어씁니다", {when(newest->timestamp)})
                                         : when(newest->timestamp) + " · " + formatBytes(uint64_t(newest->size));
      paint_.fonts.text(detail, x + 60, y + 40, 16, Secondary, false, w - 80);
      continue;
    }
    const auto &r = revisions[n - 1];
    const bool here = std::any_of(dropboxLocal_.begin(), dropboxLocal_.end(), [&](const auto &b) { return b.name == r.name; });
    paint_.fonts.text(r.name, x + 20, y + 10, 20, Ink, true, w - 220);
    // When and from which console: Dropbox holds every console's uploads.
    std::string from = trf("드롭박스 · {0}", {when(r.modified)});
    const auto meta = dropboxMetas_.find(r.name);
    if (meta != dropboxMetas_.end()) {
      const auto &m = meta->second;
      from = when(m.createdAt ? m.createdAt : r.modified) + " · " +
             (!dropboxDevice_.empty() && m.deviceId == dropboxDevice_ ? std::string(tr("이 본체"))
              : m.deviceLabel.empty()                                  ? std::string(tr("알 수 없는 기기"))
                                                                       : trf("{0} 기기", {m.deviceLabel}));
      if (!m.userName.empty()) from += " · " + m.userName;
    } else if (dropboxBusy_ == "meta") {
      from += " · " + std::string(tr("기기 확인 중"));
    }
    paint_.fonts.text(from, x + 20, y + 40, 16, Secondary, false, w - 220);
    const std::string side = here ? std::string(tr("내 기기에 있음")) : formatBytes(r.size);
    paint_.fonts.middle(side, x + w - 20 - paint_.fonts.measure(side, 17, true), y + Row / 2, 17, here ? Good : Ink, true);
  }
  const float line = body.y + 24 + rows * (Row + Gap) + 4;
  if (working)
    paint_.fonts.text(dropboxBusy_ == "upload" ? tr("드롭박스에 올리고 있습니다.") : tr("드롭박스에서 받고 있습니다."), x, line,
                      18, Secondary);
  else if (revisions.empty() && dropboxListed_)
    paint_.fonts.text(tr("드롭박스에 이 게임의 백업이 없습니다."), x, line, 18, Secondary);
  else if (revisions.empty())
    paint_.skeleton(x, line - 2, 420, 22, 6); // the Dropbox list is on its way
}
} // namespace foil::ui
