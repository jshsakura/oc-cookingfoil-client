#include "ui/Application.hpp"
#include "net/Socket.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
namespace foil::ui {
uint64_t Application::queuedBytes() const {
  uint64_t sum = 0;
  for (const auto &q : installQueue_)
    sum += q.file.size;
  return sum;
}
void Application::downloads() {
  const auto p = installer_.progress();
  const auto queued = p.status == install::Status::Idle
                          ? std::string()
                          : trf("{0} left · {1} total",
                                {std::to_string(shownQueue().size() + (installer_.running() ? 1 : 0)),
                                 formatBytes(p.total + queuedBytes())});
  pageTitle(tr("Downloads"), downloadsTab_ == 0 ? queued : std::string());
  // Tabs with their counts: what runs now, what finished, what did not.
  const int running = int(shownQueue().size()) + (installer_.running() || retroDownloading() || queueSkip() ? 1 : 0) +
                      int(patchQueue_.size()) + (patchWorker_.joinable() && !patchBusy_.empty() ? 1 : 0);
  int done = 0, failed = 0, cancelled = 0;
  for (const auto &e : history_) {
    if (extraJob(e)) continue;
    done += e.outcome == history::Outcome::Done;
    failed += e.outcome == history::Outcome::Failed;
    cancelled += e.outcome == history::Outcome::Cancelled;
  }
  headerTabs({tr("Running"), tr("Failed"), tr("Canceled"), tr("Done")}, downloadsTab_, [this](int n) {
               downloadsTab_ = n;
               downloadsRow_ = 0;
             }, 1252, 0, {running, failed, cancelled, done});
  if (downloadsTab_ != 0) {
    downloadsHistory();
    return;
  }
  // One download at a time, so one card at the top for whatever runs (a
  // game, an app, a retro game, a patch), and the queue under it.
  TopCard card;
  if (!topCardNow(p, card)) {
    if (!installQueue_.empty())
      runningList(90);
    else
      notice(tr("Nothing is downloading."), tr("Pick items on a game's page, then press A to install."), 4);
    return;
  }
  topCard(card);
  runningList(294);
}
// What the top card shows now; false when nothing runs or waits paused.
bool Application::topCardNow(const install::Progress &p, TopCard &card) {
  if (p.status == install::Status::Running) {
    card.key = "install:" + p.name;
    card.name = p.name;
    card.icon = p.icon;
    card.phase = p.phase;
    card.percent = p.percent;
    card.bytes = net::receivedBytes();
    card.left = p.total > p.received ? p.total - p.received : 0;
    card.paused = installer_.paused();
    card.onPause = [this] { installer_.setPaused(!installer_.paused()); };
    card.onCancel = [this] {
      cancelRetro_ = false;
      confirmExit_ = false;
      confirmCancel_ = true;
    };
    // The game's parts: done (a check), the one coming down, the rest.
    for (size_t i = 0; i < p.parts.size() && i < 4; ++i) {
      const auto &part = p.parts[i];
      if (part.type == "dlc" &&
          std::any_of(p.parts.begin(), p.parts.begin() + i, [](const Item &item) { return item.type == "dlc"; }))
        continue;
      std::string type = part.type == "update" ? tr("Update|item") : part.type == "dlc" ? "DLC" : tr("Base game");
      size_t last = i;
      if (part.type == "dlc") {
        size_t count = 0;
        for (size_t j = i; j < p.parts.size(); ++j)
          if (p.parts[j].type == "dlc") {
            ++count;
            last = j;
          }
        type += " " + std::to_string(count);
      }
      const bool done = last < p.index;
      const bool active = p.index >= i && p.index <= last;
      card.chips.push_back({done ? type : active ? trf("{0} in progress", {type}) : trf("{0} queued", {type}),
                            done ? 2 : active ? 1 : 0});
    }
    return true;
  }
  const auto running = std::find_if(history_.begin(), history_.end(), [](const history::Entry &e) {
    return !e.kind.empty() && e.kind != "patch" && e.kind != "reapply" && e.outcome == history::Outcome::Running;
  });
  const bool held = queueHeld_ && !installQueue_.empty() && !retroDownloading();
  if (retroDownloading() || held) {
    const bool pack = retroBusy_ == "pack";
    const std::string kind = held ? installQueue_.front().kind : running != history_.end() ? running->kind : "";
    card.key = "small:" + (held ? installQueue_.front().name : running != history_.end() ? running->install.name : retroBusy_);
    card.name = held ? installQueue_.front().name : running != history_.end() ? running->install.name : pack ? "RetroArch" : "";
    card.icon = held ? installQueue_.front().icon : running != history_.end() ? running->install.icon : "";
    const auto what = pack || kind == "retroarch" ? std::string("RetroArch")
                      : kind == "homebrew"         ? std::string(tr("Homebrew"))
                                                   : std::string(tr("Retro game"));
    const uint64_t got = retroReceived_, total = retroBytes_;
    card.percent = total ? std::min(100.0, 100.0 * double(got) / double(total)) : 0;
    card.bytes = got;
    card.left = total > got ? total - got : 0;
    card.paused = held;
    card.phase = held ? std::string(tr("Paused.")) : total ? formatBytes(got) + " / " + formatBytes(total)
                                                         : std::string(tr("Checking which files to get"));
    card.chips.push_back({what, 1});
    card.onPause = [this, held] { pauseSmallDownload(!held); };
    if (!held)
      card.onCancel = [this] {
        cancelRetro_ = true;
        confirmExit_ = false;
        confirmCancel_ = true;
      };
    return true;
  }
  if (patchDownloading()) {
    const bool busy = patchWorker_.joinable() && !patchBusy_.empty();
    UserPatch patch = !patchQueue_.empty() ? patchQueue_.front() : UserPatch{};
    if (busy) {
      const auto known = std::find_if(serverPatches_.begin(), serverPatches_.end(),
                                      [&](const UserPatch &q) { return q.id == patchBusy_; });
      patch = patchQueueNow_.id == patchBusy_ ? patchQueueNow_ : known != serverPatches_.end() ? *known : UserPatch{};
      if (patch.name.empty()) patch.name = patchBusy_;
    }
    std::string game;
    patchGame(patch, game, card.icon);
    card.key = "patch:" + patch.id;
    card.name = game + " · " + patch.name;
    const bool fetching = busy && (patchBusyLabel_ == "Downloading" || patchBusyLabel_ == "Updating");
    const uint64_t got = fetching ? patchReceived_.load() : 0, moving = patchMoving_.load();
    card.percent = !busy ? 0 : !fetching ? (moving ? std::min(100.0, 100.0 * double(patchReceived_.load()) / double(moving)) : 100.0)
                   : patch.size ? std::min(100.0, 100.0 * double(got) / double(patch.size)) : 0;
    card.bytes = got;
    card.left = fetching && patch.size > got ? patch.size - got : 0;
    card.measure = fetching;
    card.phase = !busy ? std::string(tr("Downloads once the game is installed."))
                 : fetching ? formatBytes(got) + " / " + formatBytes(patch.size)
                            : std::string(tr(patchBusyLabel_.c_str()));
    card.chips.push_back({tr("Patch"), 1});
    return true;
  }
  return false;
}
// The card at the top of 진행 중: cover, name, percent (one decimal) and a
// bar that glides, what it is doing with its speed and time left, its
// parts, and 취소 and 일시정지 bottom right (A and Y do the same).
void Application::topCard(const TopCard &c) {
  if (c.key != topCardKey_) {
    topCardKey_ = c.key;
    topCardBytes_ = topCardStartBytes_ = c.bytes;
    topCardMovedAt_ = SDL_GetTicks() / 1000.;
    shownPercent_ = -1;
    downloadSpeed_.reset();
    timeLeft_.reset();
  }
  hit(260, 90, 992, 180, [this] {
    sidebar_ = false;
    headerFocus_.clear();
    downloadsRow_ = 0;
  });
  if (inContent() && downloadsRow_ == 0)
    paint_.ring(260, 90, 992, 180, 20);
  paint_.rect(260, 90, 992, 180, Surface, 20);
  cover(c.icon, 284, 112, 112, false, c.name);
  paint_.fonts.text(c.name, 418, 112, 26, Ink, true, 680);
  // The bar glides toward the figure, which a compressed file moves a whole
  // piece at a time; going back (the next file) jumps.
  const double frameAt = SDL_GetTicks() / 1000.;
  const double goal = std::clamp(c.percent, 0., 100.);
  if (shownPercent_ < 0 || goal < shownPercent_ - 1)
    shownPercent_ = goal;
  else
    shownPercent_ += (goal - shownPercent_) * (1 - std::exp(-std::max(0., frameAt - shownPercentAt_) / 0.8));
  shownPercentAt_ = frameAt;
  // One decimal: a slow download still visibly moves.
  char percent[16];
  std::snprintf(percent, sizeof(percent), "%.1f%%", std::floor(shownPercent_ * 10) / 10);
  paint_.fonts.text(percent, 1228 - paint_.fonts.measure(percent, 26, true), 114, 26, Ink, true);
  paint_.rect(418, 154, 810, 14, Line, 7);
  paint_.rect(418, 154, 810 * shownPercent_ / 100., 14, Butter, 7);
  std::string status = c.phase;
  const double now = SDL_GetTicks() / 1000.;
  downloadSeen_ = now;
  // Nothing arriving for a while: said so, with how long, instead of a
  // number that just stops.
  if (c.bytes != topCardBytes_) {
    topCardBytes_ = c.bytes;
    topCardMovedAt_ = now;
  }
  const double still = now - topCardMovedAt_;
  // The console's own link gone (the Wi-Fi icon's check): said in red, and
  // that it goes on by itself. Otherwise, a while with nothing once bytes had
  // come is said calmly, without counting seconds; before any bytes it is
  // still getting ready (checking the install, putting a resume together).
  const bool linkDown = signal_ < 0 && !c.paused;
  if (linkDown) {
    status = c.phase + " · " + tr("The internet connection dropped. It goes on once it is back.");
    downloadSpeed_.add(now, c.bytes);
  } else if (!c.paused && c.measure && c.bytes > topCardStartBytes_ && still >= 20) {
    status += " · " + std::string(tr("The connection paused. Reconnecting."));
    downloadSpeed_.add(now, c.bytes);
  } else if (!c.paused && c.measure) {
    downloadSpeed_.add(now, c.bytes);
    const double steady = downloadSpeed_.speed(now);
    if (steady < 0) {
      status += std::string(" · ") + tr("Measuring speed");
    } else {
      char speed[64];
      std::snprintf(speed, sizeof(speed), " · %.1f MB/s", steady);
      status += speed;
      status += " · " + install::timeLeftText(timeLeft_.update(now, c.left, steady));
    }
  } else {
    downloadSpeed_.reset();
    timeLeft_.reset();
  }
  paint_.fonts.text(status, 418, 181, 18, linkDown ? Bad : Secondary, false, 810);
  float x = 418;
  for (const auto &[label, state] : c.chips) {
    // A finished part leads with the check mark icon (the font has no ✓).
    const bool done = state == 2, active = state == 1;
    const float mark = done ? 22 : 0;
    const auto width = paint_.fonts.measure(label, 15, true) + 24 + mark;
    paint_.rect(x, 216, width, 32, done ? SDL_Color{227, 242, 234, 255} : active ? Ink : Ground, 8);
    if (done)
      paint_.icon(8, x + 10, 223, Okay, 18);
    paint_.fonts.text(label, x + 12 + mark, 220, 15, done ? Okay : active ? Surface : Secondary, true);
    x += width + 10;
  }
  // 취소 always at the right end, as on every row; 일시정지 (or 이어받기) left of it.
  float bx = 1228;
  if (c.onCancel) {
    const std::string stop = tr("Cancel");
    const float cw = paint_.fonts.measure(stop, 16, true) + 32;
    bx -= cw;
    hit(bx, 212, cw, 40, c.onCancel);
    paint_.rect(bx, 216, cw, 32, Ground, 8);
    paint_.fonts.middle(stop, bx + 16, 232, 16, Ink, true);
    bx -= 10;
  }
  if (c.onPause) {
    const std::string label = c.paused ? tr("Resume") : tr("Pause");
    const float w = paint_.fonts.measure(label, 16, true) + 32;
    bx -= w;
    hit(bx, 212, w, 40, c.onPause);
    paint_.rect(bx, 216, w, 32, c.paused ? Butter : Ground, 8);
    paint_.fonts.middle(label, bx + 16, 232, 16, Ink, true);
  }
}
// Under the installer's card (or from the top): the retro and patch cards
// and 대기 중, one area that glides under a finger or the right stick.
void Application::runningList(float y) {
  constexpr float Bottom = 660;
  if (installQueue_.empty())
    return;
  // The queue, without the paused item already on the card at the top.
  const float content = queueHeight();
  const int picked = downloadsRow_ - 1;
  const float focus = picked < 0 ? 0 : QueueTitle + picked * QueueStep - (Bottom - y) / 2 + QueueStep / 2;
  const float max = std::max(0.f, content - (Bottom - y));
  const float top = glide("running", 232, y, 1048, Bottom - y, std::clamp(focus, 0.f, max), max, 1);
  if (max > 0) paint_.scrollbar(1266, y, Bottom - y - 12, top, Bottom - y, content);
  clipTo(232, y - 8, 1048, Bottom - y + 8);
  queueList(y - top, y);
  unclip();
}
float Application::queueHeight() const {
  const size_t rows = shownQueue().size();
  return rows ? QueueTitle + float(rows) * QueueStep : 0;
}
// 대기 중: what comes next, in order, scrolling under the title. On a row A
// moves it sooner, X later and Y takes it off; the row's buttons do the same.
std::vector<size_t> Application::shownQueue() const {
  std::vector<size_t> out;
  for (size_t i = queueSkip(); i < installQueue_.size(); ++i)
    if (!extraJob(installQueue_[i].kind, installQueue_[i].file)) out.push_back(i);
  return out;
}
// A row one place up or down among the rows shown (extras keep their turns).
void Application::moveShown(int row, bool up) {
  const auto shown = shownQueue();
  const int other = up ? row - 1 : row + 1;
  if (row < 0 || other < 0 || row >= int(shown.size()) || other >= int(shown.size())) return;
  if (history::swapTurns(history_, installQueue_[shown[size_t(other)]].entry, installQueue_[shown[size_t(row)]].entry)) {
    saveHistory();
    rebuildQueue();
  } else
    std::swap(installQueue_[shown[size_t(other)]], installQueue_[shown[size_t(row)]]); // a preview row with no history
  downloadsRow_ = other + 1;
}
// "본편 · SD 카드 · 4.1 GB": what a queued item is, where it goes, its size.
std::string Application::queueRowDetail(const QueuedInstall &q) const {
  const auto part = q.kind == "homebrew"    ? std::string(tr("Homebrew"))
                    : q.kind == "retro"     ? std::string(tr("Retro game"))
                    : q.kind == "retroarch" ? std::string("RetroArch")
                    : q.kind == "patch" || q.kind == "reapply" ? std::string(tr("Patch"))
                    : q.kind == "shortcut"  ? std::string(tr("HOME Menu shortcut"))
                    : q.file.type == "update" ? std::string(tr("Update|item"))
                    : q.file.type == "dlc"    ? std::string("DLC")
                                              : std::string(tr("Base game"));
  const auto where = q.target == "nand" ? tr("Console") : tr("SD card");
  return part + " · " + where + (q.file.size ? " · " + formatBytes(q.file.size) : std::string());
}
void Application::queueList(float y, float viewTop) {
  if (installQueue_.empty())
    return;
  constexpr float Row = 76, Step = QueueStep, Bottom = 660;
  const auto shown = shownQueue();
  const int count = int(shown.size());
  for (int r = 0; r < count; ++r) {
    const size_t i = shown[size_t(r)]; // its place in the queue
    const float ry = y + r * Step;
    // Out of view: neither drawn nor answering a tap (the card above is there).
    if (ry < viewTop - 8 || ry > Bottom)
      continue;
    const auto &q = installQueue_[i];
    hit(260, ry, 992, Row, [this, r] {
      sidebar_ = false;
      headerFocus_.clear();
      downloadsRow_ = r + 1;
    });
    if (inContent() && downloadsRow_ == r + 1) paint_.ring(260, ry, 992, Row, 16);
    paint_.rect(260, ry, 992, Row, Surface, 16);
    cover(q.icon, 280, ry + 10, 56, false, q.name);
    paint_.fonts.text(q.name, 356, ry + 12, 20, Ink, true, 560);
    paint_.fonts.text(queueRowDetail(q), 356, ry + 44, 16, Secondary, false, 560);
    // Buttons right to left: 취소, 아래로 (not on the last), 위로 (not on the first).
    float right = 1228;
    const auto button = [&](const std::string &label, std::function<void()> run) {
      const float w = paint_.fonts.measure(label, 16, true) + 32, x = right - w;
      hit(x, ry + 18, w, 40, std::move(run));
      paint_.rect(x, ry + 22, w, 32, Ground, 8);
      paint_.fonts.middle(label, x + 16, ry + 38, 16, Ink, true);
      right = x - 10;
    };
    button(tr("Cancel"), [this, i] {
      unqueueAsk_ = int(i);
      dialogFocus_ = 0;
    });
    if (r + 1 < count)
      button(tr("Move down"), [this, r] { moveShown(r, false); });
    if (r > 0)
      button(tr("Move up"), [this, r] { moveShown(r, true); });
  }
}
std::vector<int> Application::downloadsList() const {
  std::vector<int> out;
  for (int i = 0; i < int(history_.size()); ++i) {
    if (extraJob(history_[i])) continue;
    const auto o = history_[i].outcome;
    if (o == (downloadsTab_ == 1   ? history::Outcome::Failed
              : downloadsTab_ == 2 ? history::Outcome::Cancelled
                                   : history::Outcome::Done))
      out.push_back(i);
  }
  return out;
}
// "42초", "5분 32초", "1시간 25분".
static std::string durationText(double seconds) {
  const auto s = std::max<int64_t>(1, std::llround(seconds));
  if (s < 60)
    return trf("{0} s", {std::to_string(s)});
  if (s < 3600)
    return trf("{0} min {1} s", {std::to_string(s / 60), std::to_string(s % 60)});
  return trf("{0} h {1} min", {std::to_string(s / 3600), std::to_string(s % 3600 / 60)});
}
// 실패, 취소 and 완료: what ran and how it ended, newest first.
void Application::downloadsHistory() {
  constexpr float Top = 90, Row = 96, Step = 108;
  constexpr int Visible = 5;
  const auto list = downloadsList();
  if (list.empty()) {
    notice(downloadsTab_ == 1   ? tr("No failed downloads.")
           : downloadsTab_ == 2 ? tr("No canceled downloads.")
                                : tr("No finished downloads."),
           downloadsTab_ == 1   ? tr("Failed installs can be tried again here.")
           : downloadsTab_ == 2 ? tr("Canceled downloads can be downloaded again here, in case one was canceled by mistake.")
                                : tr("Finished installs stay here."),
           downloadsTab_ == 1 ? 25 : downloadsTab_ == 2 ? 33 : 24);
    return;
  }
  const int count = int(list.size());
  downloadsRow_ = std::clamp(downloadsRow_, 0, count - 1);
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("downloads", 232, Top, 1048, 660 - (Top), float(std::clamp(downloadsRow_ - Visible / 2, 0, std::max(0, count - Visible))),
                          float((count) - (Visible)), Step);
  // Stretched past the top while it springs back: no row before the first.
  const int first = std::max(0, int(std::floor(top)));
  paint_.scrollbar(1266, Top, Step * Visible - 12, top, Visible, float(count));
  clipTo(232, Top - 8, 1048, 660 - Top + 8);
  for (int n = first; n < count && n <= first + Visible + 1; ++n) {
    const auto &e = history_[list[n]];
    const float y = Top + (n - top) * Step;
    hit(260, y, 992, Row, [this, n] {
      // A second tap opens it, as A does.
      if (inContent() && downloadsRow_ == n) action(Action::Accept);
      sidebar_ = false;
      downloadsRow_ = n;
    });
    if (inContent() && n == downloadsRow_) paint_.ring(260, y, 992, Row, 16);
    paint_.rect(260, y, 992, Row, Surface, 16);
    cover(e.install.icon, 280, y + 12, 72, false, e.install.name);
    paint_.fonts.text(e.install.name, 372, y + 14, 21, Ink, true, 640);
    std::tm local{};
    const std::time_t at = std::time_t(e.at);
    localtime_r(&at, &local);
    char when[32];
    std::strftime(when, sizeof(when), "%m-%d %H:%M", &local);
    uint64_t fileBytes = 0;
    for (const auto &file : e.install.files) fileBytes += file.size;
    std::string meta = std::string(e.install.target == "nand" ? tr("Console") : tr("SD card")) +
                       (fileBytes ? " · " + formatBytes(fileBytes) : std::string()) + " · " + when;
    if (e.kind == "retro") meta = std::string(tr("Retro game")) + " · " + meta;
    if (e.kind == "homebrew") meta = std::string(tr("Homebrew")) + " · " + meta;
    if (e.kind == "patch" || e.kind == "reapply") meta = std::string(tr("Patch")) + " · " + meta;
    if (!e.error.empty()) meta += " · " + e.error;
    // Its HOME Menu shortcut, made right after it, says so here (it has no row).
    if (e.outcome == history::Outcome::Done) {
      const auto shortcutName = trf("{0} shortcut", {e.install.name});
      if (std::any_of(history_.begin(), history_.end(), [&](const history::Entry &x) {
            return extraJob(x) && x.outcome == history::Outcome::Done && x.install.name == shortcutName;
          }))
        meta += " · " + std::string(tr("HOME Menu shortcut made"));
    }
    // What a finished install took: average speed, the download, the install.
    if (e.outcome == history::Outcome::Done && e.downloadSeconds > 0) {
      char speed[32];
      std::snprintf(speed, sizeof(speed), "%.1f", double(e.bytes) / (1024 * 1024) / e.downloadSeconds);
      meta += " · " + trf("Average {0} MB/s", {speed}) + " · " + trf("Download time {0}", {durationText(e.downloadSeconds)});
      if (e.installSeconds > 0)
        meta += " · " + trf("Install time {0}", {durationText(e.installSeconds)});
    }
    paint_.fonts.text(meta, 372, y + 52, 16, Secondary, false, 640);
    const std::string state = e.outcome == history::Outcome::Done        ? tr("Done")
                              : e.outcome == history::Outcome::Cancelled ? tr("Cancelled")
                                                                         : tr("Failed");
    const float w = paint_.fonts.measure(state, 16, true) + 20;
    const bool ok = e.outcome == history::Outcome::Done;
    paint_.rect(1230 - w, y + 14, w, 28, ok ? Good : Line, 8);
    paint_.fonts.middle(state, 1230 - w + 10, y + 28, 16, ok ? Surface : Ink, true);
    uint64_t bytes = 0;
    for (const auto &f : e.install.files) bytes += f.size;
    const auto size = formatBytes(bytes);
    paint_.fonts.text(size, 1230 - paint_.fonts.measure(size, 17, true), y + 54, 17, Ink, true);
  }
  unclip();
}
void Application::downloadsAction(Action a) {
  if (a == Action::Previous || a == Action::Next) {
    downloadsTab_ = (downloadsTab_ + (a == Action::Next ? 1 : 3)) % 4;
    downloadsRow_ = 0;
    return;
  }
  const auto list = downloadsList();
  if (list.empty()) return;
  const int count = int(list.size());
  if (a == Action::Up) downloadsRow_ = std::max(0, downloadsRow_ - 1);
  if (a == Action::Down) downloadsRow_ = std::min(count - 1, downloadsRow_ + 1);
  const int index = list[std::clamp(downloadsRow_, 0, count - 1)];
  // A row opens what it was; from there it can be got again.
  if (a == Action::Accept) {
    openHistoryRow(history_[index]);
    return;
  }
  if (a == Action::Search) {
    history_.erase(history_.begin() + index);
    downloadsRow_ = std::min(downloadsRow_, count - 2);
    saveHistory();
  }
  if (a == Action::Queue) {
    for (auto i = list.rbegin(); i != list.rend(); ++i) history_.erase(history_.begin() + *i);
    downloadsRow_ = 0;
    saveHistory();
  }
}
} // namespace foil::ui
namespace foil::ui {
// A row of 실패, 취소 or 완료 opens what it was: the game's page (from there
// it installs again), the app's card, the retro game. One the server no
// longer lists says so instead.
void Application::openHistoryRow(const history::Entry &entry) {
  const auto gone = [this] {
    alertTitle_.clear();
    alert_ = tr("The file no longer exists on the server.");
  };
  if (!connected_) {
    message(tr("Not connected to a server."));
    return;
  }
  const auto openGame = [&](const std::string &id) {
    const auto *group = id.empty() ? nullptr : findGroup(id);
    if (!group)
      return false;
    sidebar_ = false;
    openDetail(size_t(group - groups_.data()));
    return true;
  };
  const auto openApp = [&](const std::string &payload) {
    const auto apps = homebrew::parse("{\"homebrew\":[" + payload + "]}", "");
    if (apps.empty()) {
      gone();
      return;
    }
    if (!homebrewLoaded_) { // the list comes when the screen shows
      screen(Screen::Homebrew);
      sidebar_ = false;
      return;
    }
    const auto found = std::find_if(homebrew_.begin(), homebrew_.end(),
                                    [&](const homebrew::App &app) { return app.id == apps.front().id; });
    if (found == homebrew_.end()) {
      gone();
      return;
    }
    screen(Screen::Homebrew);
    sidebar_ = false;
    homebrewQuery_.clear();
    // The tab that lists it: 설치됨 when it is on the card, else its kind's.
    const int index = int(found - homebrew_.begin());
    for (const int tab : {0, found->kind == homebrew::Kind::Port ? 2 : 1}) {
      homebrewTab_ = tab;
      const auto rows = homebrewRows();
      const auto at = std::find(rows.begin(), rows.end(), index);
      if (at == rows.end())
        continue;
      selected_ = int(at - rows.begin());
      homebrewAsk_ = true;
      homebrewAskRow_ = homebrewStatus(*found) == homebrew::Status::Installed ? 0 : -1;
      homebrewExtras_ = 0;
      homebrewSaveAsk_ = false;
      return;
    }
  };
  if (entry.kind.empty()) {
    if (!openGame(history::catalogId(entry)))
      gone();
    return;
  }
  if (entry.kind == "homebrew" || entry.kind == "shortcut") {
    openApp(entry.payload);
    return;
  }
  if (entry.kind == "retro") {
    const auto game = retro::parseGameJson(entry.payload);
    if (game.bundleUrl.empty()) {
      gone();
      return;
    }
    screen(Screen::Retro);
    sidebar_ = false;
    askRetroGame(game);
    return;
  }
  if (entry.kind == "patch" || entry.kind == "reapply") {
    const auto patches = net::parsePatches(entry.payload);
    if (patches.empty()) {
      gone();
      return;
    }
    const auto &patch = patches.front();
    if (patchesLoaded_ && std::none_of(serverPatches_.begin(), serverPatches_.end(),
                                       [&](const UserPatch &p) { return p.id == patch.id; })) {
      gone();
      return;
    }
    if (!patch.titleId.empty() && openGame(baseTitleId(patch.titleId)))
      return;
    screen(Screen::Patches);
    sidebar_ = false;
    return;
  }
  // RetroArch: the retro screen, where Y gets it again.
  screen(Screen::Retro);
  sidebar_ = false;
}
} // namespace foil::ui
