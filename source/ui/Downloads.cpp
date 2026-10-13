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
                                {std::to_string(installQueue_.size() + (installer_.running() ? 1 : 0)),
                                 formatBytes(p.total + queuedBytes())});
  pageTitle(tr("Downloads"), downloadsTab_ == 0 ? queued : std::string());
  // Tabs with their counts: what runs now, what finished, what did not.
  const int running = int(installQueue_.size()) + (installer_.running() ? 1 : 0) + (retroDownloading() ? 1 : 0) +
                      int(patchQueue_.size()) + (patchWorker_.joinable() && !patchBusy_.empty() ? 1 : 0);
  int done = 0, failed = 0, cancelled = 0;
  for (const auto &e : history_) {
    done += e.outcome == history::Outcome::Done;
    failed += e.outcome == history::Outcome::Failed;
    cancelled += e.outcome == history::Outcome::Cancelled;
  }
  headerTabs({tr("Running"), tr("Done"), tr("Failed"), tr("Canceled")}, downloadsTab_, [this](int n) {
               downloadsTab_ = n;
               downloadsRow_ = 0;
             }, 1252, 0, {running, done, failed, cancelled});
  if (downloadsTab_ != 0) {
    downloadsHistory();
    return;
  }
  const auto target = p.target == "nand" ? tr("Console") : tr("SD card");
  if (p.status == install::Status::Idle) {
    // A retro game or RetroArch can run with no install beside it.
    float y = 90;
    if (retroDownloading()) y += retroDownloadCard(y) + 30;
    if (patchDownloading()) patchDownloadCard(y);
    else if (!retroDownloading()) notice(tr("Nothing is downloading."), tr("Pick items on a game's page, then press A to install."), 4);
    return;
  }
  if (p.status == install::Status::Running && inContent() && downloadsRow_ == 0)
    paint_.ring(260, 90, 992, 180, 20);
  paint_.rect(260, 90, 992, 180, Surface, 20);
  cover(p.icon, 284, 112, 112, false, p.name);
  paint_.fonts.text(p.name, 418, 112, 26, Ink, true, 680);
  const auto percent = std::to_string(int(std::round(p.percent))) + "%";
  paint_.fonts.text(percent, 1228 - paint_.fonts.measure(percent, 26, true),
                    114, 26, Ink, true);
  paint_.rect(418, 154, 810, 14, Line, 7);
  const auto filled = 810 * std::clamp(p.percent, 0., 100.) / 100.;
  const bool paused = p.status == install::Status::Running && installer_.paused();
  paint_.rect(418, 154, filled, 14,
              p.status == install::Status::Succeeded ? Okay : paused ? Secondary : Butter, 7);
  if (filled > 0 && filled < 810)
    paint_.rect(418 + filled - 1, 154, 2, 14, Ink);
  std::string status = p.phase;
  const double now = SDL_GetTicks() / 1000.;
  // Away from this screen for a while: start the count again.
  if (now - downloadSeen_ > 1) {
    downloadSpeed_.reset();
    timeLeft_.reset();
  }
  downloadSeen_ = now;
  if (p.status == install::Status::Running && !paused) {
    downloadSpeed_.add(now, net::receivedBytes());
    const double steady = downloadSpeed_.speed(now);
    const double shown = steady >= 0 ? steady : p.speed;
    if (shown > 0 || steady >= 0) {
      char speed[64];
      std::snprintf(speed, sizeof(speed), " · %.1f MB/s", shown);
      status += speed;
      const double left = steady >= 0 ? timeLeft_.update(now, p.total > p.received ? p.total - p.received : 0, steady) : -1;
      status += " · " + install::timeLeftText(left);
    }
  } else {
    downloadSpeed_.reset();
    timeLeft_.reset();
  }
  paint_.fonts.text(status, 418, 181, 18, Secondary, false, 810);
  float x = 418;
  for (size_t i = 0; i < p.parts.size() && i < 4; ++i) {
    const auto &part = p.parts[i];
    if (part.type == "dlc" &&
        std::any_of(p.parts.begin(), p.parts.begin() + i,
                    [](const Item &item) { return item.type == "dlc"; }))
      continue;
    std::string type = part.type == "update" ? tr("Update|item")
                       : part.type == "dlc"  ? "DLC"
                                             : tr("Base game");
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
    const bool done = last < p.index || p.status == install::Status::Succeeded;
    const bool active =
        p.index >= i && p.index <= last && p.status == install::Status::Running;
    const auto label = done     ? std::string("✓ ") + type
                       : active ? trf("{0} in progress", {type})
                                : trf("{0} queued", {type});
    const auto width = paint_.fonts.measure(label, 15, true) + 24;
    paint_.rect(x, 216, width, 32,
                done     ? SDL_Color{227, 242, 234, 255}
                : active ? Ink
                         : Ground,
                8);
    paint_.fonts.text(label, x + 12, 220, 15,
                      done     ? Okay
                      : active ? Surface
                               : Secondary,
                      true);
    x += width + 10;
  }
  // Pause or resume, and cancel, bottom right of the card (A and B do the same).
  if (p.status == install::Status::Running) {
    const std::string label = paused ? tr("Resume") : tr("Pause");
    const float w = paint_.fonts.measure(label, 16, true) + 32;
    const float bx = 1228 - w;
    hit(bx, 212, w, 40, [this] { installer_.setPaused(!installer_.paused()); });
    paint_.rect(bx, 216, w, 32, paused ? Butter : Ground, 8);
    paint_.fonts.middle(label, bx + 16, 232, 16, Ink, true);
    const std::string stop = tr("Cancel");
    const float cw = paint_.fonts.measure(stop, 16, true) + 32;
    const float cx = bx - 10 - cw;
    hit(cx, 212, cw, 40, [this] {
      cancelRetro_ = false;
      confirmExit_ = false;
      confirmCancel_ = true;
    });
    paint_.rect(cx, 216, cw, 32, Ground, 8);
    paint_.fonts.middle(stop, cx + 16, 232, 16, Ink, true);
  }
  // How it ended, in one strip under the card; 완료 and 실패 keep the rest.
  int y = 294;
  if (p.status == install::Status::Succeeded || p.status == install::Status::Cancelled ||
      p.status == install::Status::Failed) {
    const bool ok = p.status == install::Status::Succeeded, failed = p.status == install::Status::Failed;
    const std::string text = ok       ? std::string(tr("Installed. You can launch it from the HOME Menu."))
                             : failed ? p.error
                                      : std::string(tr("Install canceled."));
    const bool twoLines = paint_.fonts.measure(text, 18, true) > 900;
    const float h = twoLines ? 80 : 56;
    paint_.rect(260, y, 992, h, ok ? SDL_Color{227, 242, 234, 255} : Surface, 14);
    paint_.icon(ok ? 24 : failed ? 25 : 33, 282, y + h / 2 - 12, ok ? Okay : failed ? Bad : Secondary, 24);
    if (twoLines) paint_.fonts.wrapped(text, 320, y + 14, 18, ok ? Okay : Ink, 910, 2, true);
    else paint_.fonts.middle(text, 320, y + h / 2, 18, ok ? Okay : Ink, true, 910);
    y += int(h) + 24;
  }
  if (retroDownloading()) y += int(retroDownloadCard(float(y))) + 30;
  if (patchDownloading()) y += int(patchDownloadCard(float(y))) + 30;
  if (!installQueue_.empty()) {
    paint_.fonts.text(tr("Queued"), 260, y, 26, Ink, true);
    y += 40;
    // As many as fit above the footer; the count says the rest.
    // The picked row stays in view: the list starts late enough to show it.
    const int fits = std::max(1, (660 - y) / 88);
    const size_t first = size_t(std::max(0, downloadsRow_ - fits));
    size_t shown = 0;
    for (size_t i = first; i < installQueue_.size(); ++i) {
      const auto &q = installQueue_[i];
      if (y + 88 > 660)
        break;
      const bool picked = inContent() && downloadsRow_ == int(i) + 1;
      hit(260, y, 992, 76, [this, i] {
        sidebar_ = false;
        headerFocus_.clear();
        downloadsRow_ = int(i) + 1;
      });
      if (picked) paint_.ring(260, y, 992, 76, 16);
      paint_.rect(260, y, 992, 76, Surface, 16);
      cover(q.icon, 280, y + 10, 56, false, q.name);
      paint_.fonts.text(q.name, 356, y + 12, 20, Ink, true, 700);
      const auto part = q.file.type == "update" ? std::string(tr("Update|item"))
                        : q.file.type == "dlc"  ? std::string("DLC")
                                                : std::string(tr("Base game"));
      const auto where = q.target == "nand" ? tr("Console") : tr("SD card");
      paint_.fonts.text(part + " · " + where, 356, y + 44, 16, Secondary);
      const auto size = formatBytes(q.file.size);
      const std::string stop = tr("Cancel");
      const float cw = paint_.fonts.measure(stop, 16, true) + 32, cx = 1228 - cw;
      hit(cx, y + 18, cw, 40, [this, i] { unqueue(i); });
      paint_.rect(cx, y + 22, cw, 32, Ground, 8);
      paint_.fonts.middle(stop, cx + 16, y + 38, 16, Ink, true);
      float sizeRight = cx - 20;
      if (i > 0) { // one place sooner
        const std::string up = tr("Move up");
        const float uw = paint_.fonts.measure(up, 16, true) + 32, ux = cx - 10 - uw;
        hit(ux, y + 18, uw, 40, [this, i] {
          moveUp(i);
          downloadsRow_ = int(i);
        });
        paint_.rect(ux, y + 22, uw, 32, Ground, 8);
        paint_.fonts.middle(up, ux + 16, y + 38, 16, Ink, true);
        sizeRight = ux - 20;
      }
      paint_.fonts.text(size, sizeRight - paint_.fonts.measure(size, 18, true), y + 28, 18, Ink, true);
      y += 88;
      ++shown;
    }
    if (first + shown < installQueue_.size())
      paint_.fonts.text(trf("and {0} more", {std::to_string(installQueue_.size() - first - shown)}), 260, y, 18, Secondary);
  }
}
// A retro game, RetroArch or a homebrew app coming down beside the installer: what it is,
// how far, and B to stop it.
float Application::retroDownloadCard(float y) {
  constexpr float Height = 120;
  const auto running = std::find_if(history_.begin(), history_.end(), [](const history::Entry &e) {
    return !e.kind.empty() && e.outcome == history::Outcome::Running;
  });
  const bool pack = retroBusy_ == "pack";
  const std::string name = running != history_.end() ? running->install.name : pack ? "RetroArch" : "";
  const std::string icon = running != history_.end() ? running->install.icon : "";
  const uint64_t got = retroReceived_, total = retroBytes_;
  const double ratio = total ? std::min(1.0, double(got) / double(total)) : 0.0;
  if (!installer_.running()) paint_.ring(260, y, 992, Height, 18);
  paint_.rect(260, y, 992, Height, Surface, 18);
  if (icon.empty()) {
    paint_.rect(284, y + 20, 80, 80, Ground, 14);
    paint_.icon(running != history_.end() && running->kind == "homebrew" ? 22 : 35, 308, y + 44, Ink, 32);
  } else
    cover(icon, 284, y + 20, 80, false, name, true);
  paint_.fonts.text(name, 386, y + 20, 22, Ink, true, 700);
  const auto percent = std::to_string(int(std::round(ratio * 100))) + "%";
  paint_.fonts.text(percent, 1228 - paint_.fonts.measure(percent, 22, true), y + 20, 22, Ink, true);
  paint_.rect(386, y + 60, 842, 10, Line, 5);
  paint_.rect(386, y + 60, std::max(10.0, 842 * ratio), 10, Butter, 5);
  const bool app = running != history_.end() && running->kind == "homebrew";
  const auto what = pack ? std::string(tr("Install RetroArch")) : app ? std::string(tr("Homebrew")) : std::string(tr("Retro game"));
  const auto line = total ? trf("{0} · SD card · {1} / {2}", {what, formatBytes(got), formatBytes(total)})
                          : trf("{0} · Checking which files to get", {what});
  paint_.fonts.text(line, 386, y + 82, 17, Secondary, false, 842);
  return Height;
}
std::vector<int> Application::downloadsList() const {
  std::vector<int> out;
  for (int i = 0; i < int(history_.size()); ++i) {
    const auto o = history_[i].outcome;
    if (o == (downloadsTab_ == 1   ? history::Outcome::Done
              : downloadsTab_ == 2 ? history::Outcome::Failed
                                   : history::Outcome::Cancelled))
      out.push_back(i);
  }
  return out;
}
// 완료 and 실패: what was installed and how it ended, newest first.
void Application::downloadsHistory() {
  constexpr float Top = 90, Row = 96, Step = 108;
  constexpr int Visible = 5;
  const auto list = downloadsList();
  if (list.empty()) {
    notice(downloadsTab_ == 1   ? tr("No finished downloads.")
           : downloadsTab_ == 2 ? tr("No failed downloads.")
                                : tr("No canceled downloads."),
           downloadsTab_ == 1   ? tr("Finished installs stay here.")
           : downloadsTab_ == 2 ? tr("Failed installs can be tried again here.")
                                : tr("Canceled downloads can be downloaded again here, in case one was canceled by mistake."),
           downloadsTab_ == 1 ? 24 : downloadsTab_ == 2 ? 25 : 33);
    return;
  }
  const int count = int(list.size());
  downloadsRow_ = std::clamp(downloadsRow_, 0, count - 1);
  // The row at the top follows the cursor, or the finger while it glides.
  const float top = glide("downloads", 232, Top, 1048, 660 - (Top), float(std::clamp(downloadsRow_ - Visible / 2, 0, std::max(0, count - Visible))),
                          float((count) - (Visible)), Step);
  const int first = int(top);
  paint_.scrollbar(1266, Top, Step * Visible - 12, top, Visible, float(count));
  clipTo(232, Top - 8, 1048, 660 - Top + 8);
  for (int n = first; n < count && n <= first + Visible + 1; ++n) {
    const auto &e = history_[list[n]];
    const float y = Top + (n - top) * Step;
    hit(260, y, 992, Row, [this, n] {
      if (inContent() && downloadsRow_ == n && downloadsTab_ == 2) action(Action::Accept);
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
    std::string meta = std::string(e.install.target == "nand" ? tr("Console") : tr("SD card")) + " · " + when;
    if (e.kind == "retro") meta = std::string(tr("Retro game")) + " · " + meta;
    if (e.kind == "homebrew") meta = std::string(tr("Homebrew")) + " · " + meta;
    if (!e.error.empty()) meta += " · " + e.error;
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
  if (a == Action::Accept && (downloadsTab_ == 2 || downloadsTab_ == 3)) {
    // Retro downloads start again in the retro worker; the old entry goes
    // only once the new one is under way.
    if (!history_[index].kind.empty()) {
      const auto entry = history_[index];
      const auto apps = entry.kind == "homebrew" ? homebrew::parse("{\"homebrew\":[" + entry.payload + "]}", "")
                                                 : std::vector<homebrew::App>{};
      const bool started = entry.kind == "retro"      ? getRetroGame(retro::parseGameJson(entry.payload), true)
                           : entry.kind == "homebrew" ? !apps.empty() && getHomebrew(apps.front())
                                                      : syncRetroArch();
      if (!started) return;
      const auto old = std::find_if(history_.begin(), history_.end(), [&](const history::Entry &e) { return e.id == entry.id; });
      if (old != history_.end()) history_.erase(old);
      saveHistory();
      downloadsTab_ = 0;
      return;
    }
    // Into the queue again; the old row gives way to the new one.
    const auto install = history_[index].install;
    history_.erase(history_.begin() + index);
    if (auto error = startInstall(install.files, install.name, install.icon, install.target)) {
      alert_ = error.message;
      saveHistory();
      return;
    }
    downloadsTab_ = 0;
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
