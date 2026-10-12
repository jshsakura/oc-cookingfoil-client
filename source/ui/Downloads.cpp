#include "ui/Application.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
namespace foil::ui {
void Application::downloads() {
  const auto p = installer_.progress();
  const auto queued = p.status == install::Status::Idle
                          ? std::string()
                          : trf("{0}개 남음 · 합계 {1}",
                                {std::to_string(p.waiting.size() + (installer_.running() ? 1 : 0)),
                                 formatBytes(p.total)});
  pageTitle(tr("다운로드"), downloadsTab_ == 0 ? queued : std::string());
  // Tabs with their counts: what runs now, what finished, what did not.
  const int running = (installer_.running() ? int(p.waiting.size()) + 1 : 0) + (retroDownloading() ? 1 : 0);
  int done = 0, failed = 0;
  for (const auto &e : history_) {
    done += e.outcome == history::Outcome::Done;
    failed += e.outcome == history::Outcome::Failed || e.outcome == history::Outcome::Cancelled;
  }
  headerTabs({tr("진행 중"), tr("완료"), tr("실패")}, downloadsTab_, [this](int n) {
               downloadsTab_ = n;
               downloadsRow_ = 0;
             }, 1252, 0, {running, done, failed});
  if (downloadsTab_ != 0) {
    downloadsHistory();
    return;
  }
  const auto target = p.target == "nand" ? tr("본체") : tr("SD 카드");
  if (p.status == install::Status::Idle) {
    // A retro game or RetroArch can run with no install beside it.
    if (retroDownloading()) retroDownloadCard(82);
    else notice(tr("다운로드 중인 파일이 없습니다."), tr("상세 화면에서 설치할 항목을 고른 뒤 A로 설치합니다."), 4);
    return;
  }
  if (p.status == install::Status::Running)
    paint_.ring(260, 82, 992, 180, 20);
  paint_.rect(260, 82, 992, 180, Surface, 20);
  cover(p.icon, 284, 104, 112, false, p.name);
  paint_.fonts.text(p.name, 418, 104, 26, Ink, true, 680);
  const auto percent = std::to_string(int(std::round(p.percent))) + "%";
  paint_.fonts.text(percent, 1228 - paint_.fonts.measure(percent, 26, true),
                    106, 26, Ink, true);
  paint_.rect(418, 146, 810, 14, Line, 7);
  const auto filled = 810 * std::clamp(p.percent, 0., 100.) / 100.;
  paint_.rect(418, 146, filled, 14,
              p.status == install::Status::Succeeded ? Okay : Butter, 7);
  if (filled > 0 && filled < 810)
    paint_.rect(418 + filled - 1, 146, 2, 14, Ink);
  std::string status = p.phase;
  if (p.speed > 0 && p.status == install::Status::Running) {
    char speed[64];
    std::snprintf(speed, sizeof(speed), " · %.1f MB/s", p.speed);
    const auto seconds =
        uint64_t((p.total > p.received ? p.total - p.received : 0) /
                 (p.speed * 1024 * 1024));
    status += speed;
    status += " · " + trf("약 {0}분 남음",
                          {std::to_string(std::max<uint64_t>(1, std::lround(seconds / 60.)))});
  }
  paint_.fonts.text(status, 418, 173, 18, Secondary, false, 810);
  float x = 418;
  for (size_t i = 0; i < p.parts.size() && i < 4; ++i) {
    const auto &part = p.parts[i];
    if (part.type == "dlc" &&
        std::any_of(p.parts.begin(), p.parts.begin() + i,
                    [](const Item &item) { return item.type == "dlc"; }))
      continue;
    std::string type = part.type == "update" ? tr("업데이트|항목")
                       : part.type == "dlc"  ? "DLC"
                                             : tr("본편");
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
                       : active ? trf("{0} 진행 중", {type})
                                : trf("{0} 대기", {type});
    const auto width = paint_.fonts.measure(label, 15, true) + 24;
    paint_.rect(x, 208, width, 32,
                done     ? SDL_Color{227, 242, 234, 255}
                : active ? Ink
                         : Ground,
                8);
    paint_.fonts.text(label, x + 12, 212, 15,
                      done     ? Okay
                      : active ? Surface
                               : Secondary,
                      true);
    x += width + 10;
  }
  // How it ended, in one strip under the card; 완료 and 실패 keep the rest.
  int y = 286;
  if (p.status == install::Status::Succeeded || p.status == install::Status::Cancelled ||
      p.status == install::Status::Failed) {
    const bool ok = p.status == install::Status::Succeeded, failed = p.status == install::Status::Failed;
    const std::string text = ok       ? std::string(tr("설치했습니다. 홈 메뉴에서 실행할 수 있습니다."))
                             : failed ? p.error
                                      : std::string(tr("설치를 취소했습니다."));
    const bool twoLines = paint_.fonts.measure(text, 18, true) > 900;
    const float h = twoLines ? 80 : 56;
    paint_.rect(260, y, 992, h, ok ? SDL_Color{227, 242, 234, 255} : Surface, 14);
    paint_.icon(ok ? 24 : failed ? 25 : 33, 282, y + h / 2 - 12, ok ? Okay : failed ? Bad : Secondary, 24);
    if (twoLines) paint_.fonts.wrapped(text, 320, y + 14, 18, ok ? Okay : Ink, 910, 2, true);
    else paint_.fonts.middle(text, 320, y + h / 2, 18, ok ? Okay : Ink, true, 910);
    y += int(h) + 24;
  }
  if (retroDownloading()) y += int(retroDownloadCard(float(y))) + 30;
  if (!p.waiting.empty()) {
    paint_.fonts.text(tr("대기 중"), 260, y, 26, Ink, true);
    y += 40;
    for (size_t i = 0; i < p.waiting.size() && i < 2; ++i) {
      const auto &q = p.waiting[i];
      paint_.rect(260, y, 992, 104, Surface, 16);
      cover(q.icon, 284, y + 16, 72, false, q.name);
      paint_.fonts.text(q.name, 376, y + 22, 22, Ink, true, 690);
      paint_.fonts.text(trf("{0} · 설치 대기", {target}), 376, y + 60, 18,
                        Secondary);
      const auto size = formatBytes(q.size);
      paint_.fonts.text(size, 1228 - paint_.fonts.measure(size, 20, true),
                        y + 39, 20, Ink, true);
      y += 134;
    }
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
  const auto what = pack ? std::string(tr("RetroArch 설치")) : app ? std::string(tr("홈브류")) : std::string(tr("레트로 게임|항목"));
  const auto line = total ? trf("{0} · SD 카드 · {1} / {2}", {what, formatBytes(got), formatBytes(total)})
                          : trf("{0} · 받을 파일을 확인하고 있습니다", {what});
  paint_.fonts.text(line, 386, y + 82, 17, Secondary, false, 842);
  return Height;
}
std::vector<int> Application::downloadsList() const {
  std::vector<int> out;
  for (int i = 0; i < int(history_.size()); ++i) {
    const auto o = history_[i].outcome;
    if (downloadsTab_ == 1 ? o == history::Outcome::Done
                           : o == history::Outcome::Failed || o == history::Outcome::Cancelled)
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
    notice(downloadsTab_ == 1 ? tr("완료한 다운로드가 없습니다.") : tr("실패한 다운로드가 없습니다."),
           downloadsTab_ == 1 ? tr("설치를 마치면 여기에 남습니다.") : tr("실패하거나 취소한 설치는 여기에서 다시 시도합니다."),
           downloadsTab_ == 1 ? 24 : 25);
    return;
  }
  const int count = int(list.size());
  downloadsRow_ = std::clamp(downloadsRow_, 0, count - 1);
  const int first = std::clamp(downloadsRow_ - Visible / 2, 0, std::max(0, count - Visible));
  paint_.scrollbar(1266, Top, Step * Visible - 12, float(first), Visible, float(count));
  for (int n = first; n < count && n <= first + Visible; ++n) {
    const auto &e = history_[list[n]];
    const float y = Top + (n - first) * Step;
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
    std::string meta = std::string(e.install.target == "nand" ? tr("본체") : tr("SD 카드")) + " · " + when;
    if (e.kind == "retro") meta = std::string(tr("레트로 게임|항목")) + " · " + meta;
    if (e.kind == "homebrew") meta = std::string(tr("홈브류")) + " · " + meta;
    if (!e.error.empty()) meta += " · " + e.error;
    paint_.fonts.text(meta, 372, y + 52, 16, Secondary, false, 640);
    const std::string state = e.outcome == history::Outcome::Done        ? tr("완료")
                              : e.outcome == history::Outcome::Cancelled ? tr("취소됨")
                                                                         : tr("실패");
    const float w = paint_.fonts.measure(state, 16, true) + 20;
    const bool ok = e.outcome == history::Outcome::Done;
    paint_.rect(1230 - w, y + 14, w, 28, ok ? Good : Line, 8);
    paint_.fonts.middle(state, 1230 - w + 10, y + 28, 16, ok ? Surface : Ink, true);
    uint64_t bytes = 0;
    for (const auto &f : e.install.files) bytes += f.size;
    const auto size = formatBytes(bytes);
    paint_.fonts.text(size, 1230 - paint_.fonts.measure(size, 17, true), y + 54, 17, Ink, true);
  }
}
void Application::downloadsAction(Action a) {
  if (a == Action::Previous || a == Action::Next) {
    downloadsTab_ = (downloadsTab_ + (a == Action::Next ? 1 : 2)) % 3;
    downloadsRow_ = 0;
    return;
  }
  const auto list = downloadsList();
  if (list.empty()) return;
  const int count = int(list.size());
  if (a == Action::Up) downloadsRow_ = std::max(0, downloadsRow_ - 1);
  if (a == Action::Down) downloadsRow_ = std::min(count - 1, downloadsRow_ + 1);
  const int index = list[std::clamp(downloadsRow_, 0, count - 1)];
  if (a == Action::Accept && downloadsTab_ == 2) {
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
    // Again from the start; the old entry gives way to the new one.
    if (installer_.running()) {
      message(tr("지금 설치가 끝난 뒤 다시 시도합니다."));
      return;
    }
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
