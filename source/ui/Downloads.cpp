#include <cctype>
#include "ui/Application.hpp"
#include "core/Forwarder.hpp"
#include "net/Socket.hpp"
#include "net/Tuner.hpp"
#include "platform/Storage.hpp"
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
  const int running = int(downloadsRunning());
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
  const double now = SDL_GetTicks() / 1000.;
  if (!topCardNow(p, card)) {
    // Between one download and the next: the queue keeps its place under
    // where the card was, instead of jumping up and back down.
    constexpr double Between = 1.5;
    if (!installQueue_.empty() && now - cardShownAt_ < Between) {
      // The card just done stays a moment, still, until the next one comes.
      auto still = lastCard_;
      still.onPause = still.onCancel = still.onDetails = nullptr;
      still.measure = false;
      runningList(294, &still);
    }
    else if (!installQueue_.empty())
      runningList(90);
    else
      notice(tr("Nothing is downloading."),
             std::string(tr("Pick items on a game's page, then press A to install.")) +
                 // As an application the queue goes on under the HOME Menu (platform::keepRunningInBackground).
                 (platform::applicationMode() ? "\n" + std::string(tr("Downloads go on even after you go to the HOME Menu.")) : ""),
             4);
    return;
  }
  // A new card rises from the queue's first row, where it waited, and the
  // rows under it move up a place, eased.
  constexpr double Rise = 0.32;
  // Only one taking another's place rises; the first one is simply there.
  if (card.key != topCardKey_ && !card.extra && !topCardKey_.empty() && now - cardShownAt_ < 5) cardChangedAt_ = now;
  cardShownAt_ = now;
  lastCard_ = card;
  const float t = float(std::clamp((now - cardChangedAt_) / Rise, 0.0, 1.0));
  const float left = (1 - t) * (1 - t) * (1 - t); // what is still to go
  runningList(294, &card, left * (294 + QueueTitle - 90), left * QueueStep);
}
// What the top card shows now; false when nothing runs or waits paused.
bool Application::topCardNow(const install::Progress &p, TopCard &card) {
  if (p.status == install::Status::Running) {
    card.key = "install:" + p.name;
    card.extra = !p.parts.empty() && p.parts.front().type == "shortcut";
    card.name = p.name;
    card.icon = p.icon;
    card.phase = p.phase;
    card.percent = p.percent;
    card.bytes = net::receivedBytes();
    card.left = p.total > p.received ? p.total - p.received : 0;
    card.paused = installer_.paused();
    card.onPause = [this] { installer_.setPaused(!installer_.paused()); };
    card.onDetails = runningDetails();
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
    card.extra = kind == "shortcut";
    card.name = held ? installQueue_.front().name : running != history_.end() ? running->install.name : pack ? "RetroArch" : "";
    card.icon = held ? installQueue_.front().icon : running != history_.end() ? running->install.icon : "";
    const auto what = pack || kind == "retroarch" ? std::string("RetroArch")
                      : kind == "homebrew"         ? std::string(tr("Homebrew"))
                      : held && kind.empty()       ? std::string(tr("Game")) // a game held at start
                                                   : std::string(tr("Retro game"));
    // What is left to get, not the whole app: files already on the card are
    // counted apart (they would read as gigabits a second, and a few changed
    // files as a 900 MB download).
    // The bar is the whole app; the part already on the card shows lighter.
    const uint64_t kept = retroKept_, all = retroReceived_, whole = retroBytes_;
    const uint64_t got = all - std::min(kept, all), total = whole - std::min(kept, whole);
    card.percent = whole ? std::min(100.0, 100.0 * double(all) / double(whole)) : 0;
    card.skipped = whole ? std::min(card.percent, 100.0 * double(kept) / double(whole)) : 0;
    card.bytes = got;
    card.left = total > got ? total - got : 0;
    card.paused = held;
    const uint64_t checking = cardCheck_.total, checked = std::min<uint64_t>(cardCheck_.done, checking);
    card.phase = held        ? std::string(tr("Paused."))
                 : checking  ? trf("Checking the files already on the card ({0} / {1})",
                                   {formatBytes(checked), formatBytes(checking)})
                 : total     ? formatBytes(got) + " / " + formatBytes(total)
                             : std::string(tr("Checking which files to get"));
    // Nothing comes in while it checks: no speed to measure yet.
    card.measure = !held && total && !checking;
    // Why it is less than the app's size: what the card has is not got again.
    if (!held && total && kept) card.phase += " · " + trf("{0} already here", {formatBytes(kept)});
    card.chips.push_back({what, 1});
    // Asked to stop, it says so at once and takes no more presses until the
    // task has wound down (the file being checked or written finishes).
    if (!held && retroCancel_) {
      card.phase = retroPausing_ ? tr("Pausing. What came so far is kept.") : tr("Cancelling.");
      card.measure = false;
      return true;
    }
    card.onPause = [this, held] { pauseSmallDownload(!held); };
    // Paused, it still opens its page: the held item's own history row.
    card.onDetails = held ? queuedDetails(installQueue_.front()) : runningDetails();
    card.onCancel = [this, held] {
      dialogFocus_ = 0;
      if (held) { // paused before it began: taken off the queue, after asking
        unqueueAsk_ = 0;
        unqueuePick_ = false;
        return;
      }
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
void Application::topCard(const TopCard &c, float dy) {
  if (c.key != topCardKey_) {
    topCardKey_ = c.key;
    topCardBytes_ = topCardStartBytes_ = c.bytes;
    topCardMovedAt_ = SDL_GetTicks() / 1000.;
    shownPercent_ = -1;
    downloadSpeed_.reset();
    timeLeft_.reset();
  }
  hit(260, dy + 90, 992, 180, [this] {
    sidebar_ = false;
    headerFocus_.clear();
    downloadsRow_ = 0;
  });
  if (inContent() && downloadsRow_ == 0)
    paint_.ring(260, dy + 90, 992, 180, 20);
  paint_.rect(260, dy + 90, 992, 180, Surface, 20);
  cover(c.icon, 284, dy + 112, 112, false, c.name);
  paint_.fonts.text(c.name, 418, dy + 112, 26, Ink, true, 680);
  // The bar glides toward the figure, which a compressed file moves a whole
  // piece at a time; going back (the next file) jumps.
  const double frameAt = SDL_GetTicks() / 1000.;
  const double goal = std::clamp(c.percent, 0., 100.);
  // Back by more than a step: it picks up from the block that was cut off
  // (an earlier resume point), and the line under the bar says so a moment.
  if (shownPercent_ >= 0 && goal < shownPercent_ - 1)
    resumedNoteUntil_ = frameAt + 6;
  if (shownPercent_ < 0 || goal < shownPercent_ - 1)
    shownPercent_ = goal;
  else
    shownPercent_ += (goal - shownPercent_) * (1 - std::exp(-std::max(0., frameAt - shownPercentAt_) / 0.8));
  shownPercentAt_ = frameAt;
  // One decimal: a slow download still visibly moves.
  char percent[16];
  std::snprintf(percent, sizeof(percent), "%.1f%%", std::floor(shownPercent_ * 10) / 10);
  paint_.fonts.text(percent, 1228 - paint_.fonts.measure(percent, 26, true), dy + 114, 26, Ink, true);
  paint_.rect(418, dy + 154, 810, 14, Line, 7);
  // The fill never narrower than the bar is tall: a rounded end needs that
  // much, or the first percents draw flat and grow round.
  paint_.rect(418, dy + 154, std::max(14., 810 * shownPercent_ / 100.), 14, Butter, 7);
  // Files already on the card, passed rather than fetched: the bar's start
  // in a paler butter, so a quick finish reads as what it is.
  if (c.skipped > 0)
    paint_.rect(418, dy + 154, std::max(14., 810 * std::min(c.skipped, shownPercent_) / 100.), 14, SDL_Color{255, 226, 150, 255}, 7);
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
  if (now < resumedNoteUntil_ && !c.paused)
    status += " · " + std::string(tr("Picking up from the block that was cut off."));
  // The console's own link gone (the Wi-Fi icon's check): said in red, and
  // that it goes on by itself. Otherwise, a while with nothing once bytes had
  // come is said calmly, without counting seconds; before any bytes it is
  // still getting ready (checking the install, putting a resume together).
  const bool linkDown = signal_ < 0 && !c.paused;
  // The server restarting: a pause, not a failure, and it goes on by itself.
  const bool restarting = !linkDown && !c.paused && net::serverRestarting();
  if (restarting) {
    status = std::string(tr("Paused.")) + " · " + tr("The server is restarting. It goes on by itself.");
    downloadSpeed_.add(now, c.bytes);
  } else if (linkDown) {
    status = c.phase + " · " + tr("The internet connection dropped. It goes on once it is back.");
    downloadSpeed_.add(now, c.bytes);
  } else if (!c.paused && c.measure && c.bytes > topCardStartBytes_ && still >= 20) {
    status += " · " + std::string(tr("Reconnecting"));
    downloadSpeed_.add(now, c.bytes);
  } else if (!c.paused && c.measure) {
    downloadSpeed_.add(now, c.bytes);
    const double steady = downloadSpeed_.speed(now);
    // Nothing measured yet, or nothing came this moment: no "0 bps".
    if (steady <= 0) {
      status += std::string(" · ") + tr("Measuring speed");
    } else {
      // In the unit its size calls for, and in bits as speed test apps
      // count them; steady is in 1024 x 1024 bytes a second.
      status += " · " + formatSpeed(steady * 1048576.0);
      // The time left waits for the connections to be found (or a minute):
      // from the first, slower speed it read an hour, then jumped to minutes.
      // Even with them found, a few seconds of this card's own speed come
      // first: right after the files on the card were checked, the first
      // reading is tiny and read as days.
      constexpr double TimeLeftWait = 60, FirstReadings = 6, Unbelievable = 7 * 86400;
      if (timeLeftFrom_ < 0) timeLeftFrom_ = now;
      if (!timeLeftShown_ && now - timeLeftFrom_ >= FirstReadings &&
          (net::tuningFound() || now - timeLeftFrom_ >= TimeLeftWait))
        timeLeftShown_ = true;
      // Until then it says what it is doing rather than a number that will jump.
      const double left = timeLeftShown_ ? timeLeft_.update(now, c.left, downloadSpeed_.average()) : -1;
      status += " · " + (timeLeftShown_ ? install::timeLeftText(left > Unbelievable ? -1 : left)
                                        : std::string(net::tuningFound() ? tr("Estimating time left")
                                                                         : tr("Tuning the connection")));
    }
  } else {
    downloadSpeed_.reset();
    timeLeft_.reset();
    timeLeftShown_ = false;
    timeLeftFrom_ = -1;
  }
  paint_.fonts.text(status, 418, dy + 181, 18, linkDown ? Bad : restarting ? Pending : Secondary, false, 810);
  float x = 418;
  for (const auto &[label, state] : c.chips) {
    // A finished part leads with the check mark icon (the font has no check mark glyph).
    const bool done = state == 2, active = state == 1;
    const float mark = done ? 22 : 0;
    const auto width = paint_.fonts.measure(label, 15, true) + 24 + mark;
    paint_.rect(x, dy + 216, width, 32, done ? SDL_Color{227, 242, 234, 255} : active ? Ink : Ground, 8);
    if (done)
      paint_.icon(8, x + 10, dy + 223, Okay, 18);
    paint_.fonts.text(label, x + 12 + mark, dy + 220, 15, done ? Okay : active ? Surface : Secondary, true);
    x += width + 10;
  }
  // 취소 always at the right end, as on every row; 일시정지 (or 이어받기) left of it.
  float bx = 1228;
  if (c.onCancel) {
    const std::string stop = tr("Cancel");
    const float cw = paint_.fonts.measure(stop, 16, true) + 32;
    bx -= cw;
    buttonHit({bx, dy + 216, cw, 32}, 8, c.onCancel);
    paint_.rect(bx, dy + 216, cw, 32, Ground, 8);
    paint_.fonts.middle(stop, bx + 16, dy + 232, 16, Ink, true);
    bx -= 10;
  }
  if (c.onPause) {
    const std::string label = c.paused ? tr("Resume") : tr("Pause");
    const float w = paint_.fonts.measure(label, 16, true) + 32;
    bx -= w;
    buttonHit({bx, dy + 216, w, 32}, 8, c.onPause);
    paint_.rect(bx, dy + 216, w, 32, c.paused ? Butter : Ground, 8);
    paint_.fonts.middle(label, bx + 16, dy + 232, 16, Ink, true);
    bx -= 10;
  }
  // Its page, only when it has one.
  if (c.onDetails) {
    const std::string label = tr("Details");
    const float w = paint_.fonts.measure(label, 16, true) + 32;
    bx -= w;
    buttonHit({bx, dy + 216, w, 32}, 8, c.onDetails);
    paint_.rect(bx, dy + 216, w, 32, Ground, 8);
    paint_.fonts.middle(label, bx + 16, dy + 232, 16, Ink, true);
  }
}
const history::Entry *Application::runningEntry() const {
  // The installer's is a game's (no kind); the other downloads have one.
  const bool game = installer_.running();
  const auto found = std::find_if(history_.begin(), history_.end(), [&](const history::Entry &e) {
    return e.outcome == history::Outcome::Running && e.kind.empty() == game && e.kind != "patch" && e.kind != "reapply";
  });
  return found == history_.end() ? nullptr : &*found;
}
std::function<void()> Application::runningDetails() {
  if (installer_.running()) {
    // A game: its page, found by the id its first part carries.
    const auto p = installer_.progress();
    if (p.parts.empty()) return nullptr;
    const auto &file = p.parts.front();
    const auto *group = findGroup(!file.baseId.empty() ? file.baseId : !file.titleId.empty() ? file.titleId : file.url);
    if (!group) return nullptr;
    return [this, index = size_t(group - groups_.data())] {
      sidebar_ = false;
      openDetail(index);
    };
  }
  const auto *entry = runningEntry();
  if (!entry || !hasPage(*entry)) return nullptr;
  return [this, id = entry->id] {
    for (const auto &e : history_)
      if (e.id == id) return openHistoryRow(e);
  };
}
std::function<void()> Application::queuedDetails(const QueuedInstall &item) {
  const auto found = std::find_if(history_.begin(), history_.end(), [&](const auto &e) { return e.id == item.entry; });
  if (found == history_.end() || !hasPage(*found)) return nullptr;
  return [this, id = item.entry] {
    for (const auto &e : history_)
      if (e.id == id) return openHistoryRow(e);
  };
}
bool Application::hasPage(const history::Entry &entry) const {
  if (entry.kind.empty()) {
    const auto id = history::catalogId(entry);
    return !id.empty() && findGroup(id);
  }
  if (entry.kind == "homebrew" || entry.kind == "shortcut")
    return !homebrew::parse("{\"homebrew\":[" + entry.payload + "]}", "").empty();
  if (entry.kind == "retro") return !retro::parseGameJson(entry.payload).bundleUrl.empty();
  return entry.kind == "retroarch";
}
// Under the installer's card (or from the top): the retro and patch cards
// and 대기 중, one area that glides under a finger or the right stick.
// The card (when one runs) and the queue under it scroll as one page: a
// finger anywhere on it moves it, and it stretches at its ends as every list.
void Application::runningList(float y, const TopCard *card, float cardDy, float shift) {
  constexpr float Bottom = 660, CardTop = 90;
  const float from = card ? CardTop : y, view = Bottom - from;
  const float lead = card ? y - CardTop : 0; // the card's part of the page
  // The queue, without the paused item already on the card at the top.
  const float content = lead + queueHeight();
  const int picked = downloadsRow_ - 1;
  const float focus = picked < 0 ? 0 : lead + QueueTitle + picked * QueueStep - view / 2 + QueueStep / 2;
  const float max = std::max(0.f, content - view);
  const float top = glide("running", 232, from, 1048, view, std::clamp(focus, 0.f, max), max, 1);
  if (max > 0) paint_.scrollbar(1266, from, view - 12, top, view, content);
  clipTo(232, from - 8, 1048, view + 8);
  if (!installQueue_.empty()) queueList(from + lead - top + shift, from);
  // Over the rows: a rising card passes in front of them.
  if (card) {
    // A row's cover may have let the clip go: set again, not stacked (one
    // unclip below ends it; a second push stayed and cut the menu's taps).
    unclip();
    clipTo(232, from - 8, 1048, view + 8);
    topCard(*card, cardDy - top);
  }
  unclip();
}
float Application::queueHeight() const {
  const size_t rows = shownQueue().size();
  return rows ? QueueTitle + float(rows) * QueueStep : 0;
}
// 대기 중: what comes next, in order, scrolling under the title. On a row A
// moves it sooner, X later and Y takes it off; the row's buttons do the same.
// What 진행 중 lists: the queue, the job running now and the patches'. The
// menu's badge on 다운로드 shows the same count.
size_t Application::downloadsRunning() const {
  return shownQueue().size() + (installer_.running() || retroDownloading() || queueSkip() ? 1 : 0) + patchQueue_.size() +
         (patchWorker_.joinable() && !patchBusy_.empty() ? 1 : 0);
}
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
      buttonHit({x, ry + 22, w, 32}, 8, std::move(run));
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
namespace {
history::Outcome tabOutcome(int tab) {
  return tab == 1 ? history::Outcome::Failed : tab == 2 ? history::Outcome::Cancelled : history::Outcome::Done;
}
} // namespace
// The next page of this tab's ended rows, older than the oldest read, off
// the drawing thread; they join the list as they were before.
void Application::readOlderHistory() {
  const auto outcome = tabOutcome(downloadsTab_);
  if (historyPageJob_.busy() || !store_.isOpen() || !store_.moreOf(outcome)) return;
  int64_t at = INT64_MAX;
  uint64_t id = UINT64_MAX;
  for (const auto &e : history_)
    if (e.outcome == outcome && (e.at < at || (e.at == at && e.id < id))) {
      at = e.at;
      id = e.id;
    }
  historyPageJob_.run("history", [this, outcome, at, id]() -> Job::Finish {
    bool more = false;
    auto page = store_.older(outcome, at, id, HistoryPage, more);
    return [this, page = std::move(page)]() mutable {
      std::set<uint64_t> have;
      for (const auto &e : history_) have.insert(e.id);
      for (auto &e : page)
        if (have.insert(e.id).second) history_.push_back(std::move(e));
    };
  }, [] {});
}
// 비우기: the rows read go from the list, and the ones never read from the file.
void Application::clearHistoryTab(history::Outcome outcome) {
  if (historyPageJob_.busy()) return; // a page on its way would bring some back
  history_.erase(std::remove_if(history_.begin(), history_.end(),
                                [&](const history::Entry &e) { return e.outcome == outcome && !extraJob(e); }),
                 history_.end());
  saveHistory();
  historyPageJob_.run("history", [this, outcome]() -> Job::Finish {
    std::string error;
    if (!store_.removeAll(outcome, error)) logLine("downloads: " + error);
    return [] {};
  }, [] {});
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
  // Near the end of what is read, the next page is asked for.
  if (first + Visible + 10 >= count) readOlderHistory();
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
    if (e.outcome == history::Outcome::Done && e.downloadSeconds > 0 && e.bytes > 0) {
      meta += " · " + trf("Average {0}|speed", {formatSpeed(double(e.bytes) / e.downloadSeconds)}) + " · " + trf("Download time {0}", {durationText(e.downloadSeconds)});
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
  // A failed or canceled row goes again at once, first in line, from where
  // it stopped; a finished one opens what it was.
  if (a == Action::Accept) {
    if (history_[index].outcome == history::Outcome::Done)
      openHistoryRow(history_[index]);
    else
      retryHistoryRow(history_[index]);
    return;
  }
  if (a == Action::Search) {
    history_.erase(history_.begin() + index);
    downloadsRow_ = std::min(downloadsRow_, count - 2);
    saveHistory();
  }
  if (a == Action::Queue) {
    clearHistoryTab(tabOutcome(downloadsTab_));
    downloadsRow_ = 0;
  }
}
} // namespace foil::ui
namespace foil::ui {
void Application::retryHistoryRow(history::Entry entry) {
  if (!connected_) {
    message(tr("Not connected to a server."));
    return;
  }
  if (entry.install.files.empty())
    return;
  const auto &file = entry.install.files.front();
  // The row it was gives way to the new one (startInstall and queueJob take
  // out an older failed or canceled row of the same file).
  // Again from the server it came from.
  if (entry.kind.empty()) {
    auto files = entry.install.files;
    for (auto &f : files)
      if (f.server.empty()) f.server = entry.install.server;
    if (auto error = startInstall(files, entry.install.name, entry.install.icon, entry.install.target, {}, true))
      alert_ = error.message;
  } else {
    queueJob(entry.kind, entry.install.name, entry.install.icon, file.url, file.size, entry.payload, 0, true,
             entry.install.server);
  }
  downloadsRow_ = std::min(downloadsRow_, std::max(0, int(downloadsList().size()) - 1));
}
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
    // The server may list it under another id now (rebuilt): found by where
    // it installs, or its name.
    homebrewOpenLike_ = apps.front();
    if (!openHomebrewPage(apps.front().id))
      gone();
  };
  // A shortcut's row opens the app it is for (the server's shortcut is not a
  // game of the catalog, and one made here keeps no app in its row).
  if (entry.kind == "shortcut" || (entry.kind.empty() && !entry.install.files.empty() &&
                                   forwarder::shortcutId(forwarder::parseHexId(entry.install.files.front().titleId)))) {
    const auto id = shortcutApp(entry);
    if (!id.empty() && openHomebrewPage(id)) return;
    if (entry.kind == "shortcut") openApp(entry.payload); // a row from before plans: the app's JSON
    else gone();
    return;
  }
  if (entry.kind.empty()) {
    if (!openGame(history::catalogId(entry)))
      gone();
    return;
  }
  if (entry.kind == "homebrew") {
    openApp(entry.payload);
    return;
  }
  if (entry.kind == "retro") {
    const auto game = retro::parseGameJson(entry.payload);
    if (game.bundleUrl.empty()) {
      gone();
      return;
    }
    sidebar_ = false;
    openRetroDetail(game);
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
  sidebar_ = false;
  openRetroArchPage();
}
bool Application::openHomebrewPage(const std::string &id) {
  if (!homebrewLoaded_) {
    // The list comes when the screen shows; the page opens then (pollHomebrew).
    homebrewOpenId_ = id;
    screen(Screen::Homebrew);
    sidebar_ = false;
    return true;
  }
  homebrewOpenId_.clear();
  auto found = std::find_if(homebrew_.begin(), homebrew_.end(), [&](const homebrew::App &app) { return app.id == id; });
  if (found == homebrew_.end() && homebrewOpenLike_) found = homebrewLike(*homebrewOpenLike_);
  homebrewOpenLike_.reset();
  if (found == homebrew_.end())
    return false;
  if (screen_ != Screen::Homebrew)
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
    homebrewReturn_ = Screen::Downloads; // B goes back there
    descriptionScroll_ = 0;
    descriptionFocus_ = false;
    unglide();
    homebrewAsk_ = true;
    homebrewAskRow_ = homebrewStatus(*found) == homebrew::Status::Installed ? 0 : -1;
    homebrewExtras_ = 0;
    homebrewSaveAsk_ = false;
    return true;
  }
  return false;
}
// The server's entry for an app known from before, when its id changed (the
// server rebuilt it): the same install path, else the same name.
std::vector<homebrew::App>::iterator Application::homebrewLike(const homebrew::App &old) {
  const auto lower = [](std::string text) {
    for (auto &c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
  };
  auto found = std::find_if(homebrew_.begin(), homebrew_.end(), [&](const homebrew::App &app) { return app.id == old.id; });
  if (found == homebrew_.end() && !old.installPath.empty())
    found = std::find_if(homebrew_.begin(), homebrew_.end(), [&](const homebrew::App &app) {
      return !app.cardOnly && lower(app.installPath) == lower(old.installPath);
    });
  if (found == homebrew_.end() && !old.name.empty())
    found = std::find_if(homebrew_.begin(), homebrew_.end(),
                         [&](const homebrew::App &app) { return !app.cardOnly && app.name == old.name; });
  return found;
}
} // namespace foil::ui
