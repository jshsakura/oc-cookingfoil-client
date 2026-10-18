// RetroArch's page, opened from its row in 다운로드, in the shape of an app's:
// the header, what it is and what is on the card on the left, and on the
// right 전체 다시 받기 over the button that gets it or checks it again.
#include "ui/Application.hpp"
#include "core/I18n.hpp"
#include "core/Json.hpp"
#include "core/Retro.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <ctime>
namespace foil::ui {
namespace {
constexpr int FullRow = 0, Button = 1;
const char *const PackRecordPath = "/switch/cookingfoil/retro-pack.json";
} // namespace
void Application::openRetroArchPage() {
  if (screen_ != Screen::RetroArch) previous_ = screen_;
  notice_.clear();
  headerFocus_.clear();
  unglide();
  screen_ = Screen::RetroArch;
  retroArchPart_ = Button;
  descriptionScroll_ = 0;
  descriptionFocus_ = false;
  // Read once on the way in, off the main thread: the record is large.
  if (!retroArchCount_.valid())
    retroArchCount_ = std::async(std::launch::async, [path = sdRoot_ + PackRecordPath] {
      return retro::parsePackRecord(readFile(path)).files.size();
    });
}
int Application::retroArchQueuedAt() const {
  for (size_t i = 0; i < installQueue_.size(); ++i)
    if (installQueue_[i].kind == "retroarch") return int(i);
  return -1;
}
void Application::retroArchPageAction(Action a) {
  if (a == Action::Back) {
    screen_ = previous_ == Screen::RetroArch ? Screen::Retro : previous_;
    unglide();
    return;
  }
  const bool present = retroArchPresent();
  if (!present) retroArchPart_ = Button;
  if (a == Action::Up && present) retroArchPart_ = FullRow;
  if (a == Action::Down) retroArchPart_ = Button;
  if (a != Action::Accept) return;
  // On its way: the button asks to cancel, as a game's does.
  if (retroBusy_ == "pack") {
    dialogFocus_ = 0;
    cancelRetro_ = true;
    confirmCancel_ = true;
    confirmExit_ = false;
    return;
  }
  if (const int at = retroArchQueuedAt(); at >= 0) {
    dialogFocus_ = 0;
    unqueueAsk_ = at;
    unqueuePick_ = false;
    return;
  }
  if (syncRetroArch(retroArchPart_ == FullRow))
    message(retroArchPart_ == FullRow ? tr("Every RetroArch file comes down again. Your settings stay.")
                                      : tr("Checking RetroArch for new files."));
}
void Application::retroArchPage() {
  if (retroArchCount_.valid() && retroArchCount_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    retroArchFiles_ = retroArchCount_.get();
  const bool present = retroArchPresent();
  // The last time it came down, for its date and size.
  const history::Entry *last = nullptr;
  for (const auto &e : history_)
    if (e.kind == "retroarch" && e.outcome == history::Outcome::Done && (!last || e.at > last->at)) last = &e;
  uint64_t size = 0;
  if (last) {
    for (const auto &f : last->install.files) size += f.size;
    if (!size) size = last->bytes;
  }
  paint_.rect(0, 0, 1280, 212, {10, 10, 14, 255});
  paint_.icon(7, 32, 14, Surface, 20);
  paint_.fonts.middle(previous_ == Screen::Downloads ? tr("Downloads") : tr("Retro games"), 56, 24, 17, Surface);
  hit(24, 4, 200, 40, [this] { action(Action::Back); });
  constexpr int Top = 58;
  cover("", 40, Top, 140, false, "RetroArch", true, true);
  paint_.fonts.text("RetroArch", 208, Top + 4, 38, Surface, true, 1010);
  paint_.fonts.text(tr("Runs the retro games"), 208, Top + 62, 19, {228, 228, 234, 255}, false, 960);
  float px = 208;
  px += paint_.pill(present ? tr("Installed") : tr("Not installed"), px, Top + 100, true, 17) + 10;
  if (size) paint_.pill(formatBytes(size), px, Top + 100, true, 17);
  // The left column: what it is, then what is on the card.
  constexpr int BandTop = 212, Pad = 14;
  int y = BandTop + Pad;
  y += int(paint_.fonts.wrapped(tr("A retro game starts once RetroArch and that game's core are on the SD card. Getting a game brings its core along."),
                                40, y, 20, Ink, 750, 20, false, 1.6f)) + 24;
  struct Fact {
    int icon;
    std::string label, value;
  };
  std::vector<Fact> facts{{24, tr("Status|retroarch"), present ? tr("Installed") : tr("Not installed")}};
  if (retroArchFiles_) facts.push_back({16, tr("Files on the card"), trf("{0} files", {std::to_string(retroArchFiles_)})});
  if (last) {
    std::tm local{};
    const std::time_t at = std::time_t(last->at);
    localtime_r(&at, &local);
    char when[32];
    std::strftime(when, sizeof(when), "%Y.%m.%d %H:%M", &local);
    facts.push_back({11, tr("Last downloaded"), when});
  }
  if (size) facts.push_back({14, tr("Size|info"), formatBytes(size)});
  constexpr float Left = 40, Width = 750, Cell = Width / 3, RowHeight = 66, PathRow = 58;
  const int rows = int((facts.size() + 2) / 3);
  paint_.rect(Left, float(y), Width, rows * RowHeight + 24 + PathRow, Surface, 16);
  for (size_t n = 0; n < facts.size(); ++n) {
    const float x = Left + 20 + (n % 3) * Cell, top = y + 16 + (n / 3) * RowHeight;
    paint_.icon(facts[n].icon, x, top + 1, Secondary, 16);
    paint_.fonts.middle(facts[n].label, x + 22, top + 9, 15, Secondary);
    paint_.fonts.text(facts[n].value, x, top + 24, 18, Ink, true, Cell - 30);
  }
  {
    const float top = y + 16 + rows * RowHeight;
    paint_.rect(Left + 20, top - 6, Width - 40, 1, Line);
    paint_.icon(26, Left + 20, top + 9, Secondary, 16);
    paint_.fonts.middle(tr("Install location"), Left + 42, top + 17, 15, Secondary);
    const float labelW = paint_.fonts.measure(tr("Install location"), 15) + 18;
    paint_.fonts.middle(std::string("/") + retro::RetroArchApp, Left + 42 + labelW, top + 17, 16, Ink, false, Width - 82 - labelW);
  }
  // The right panel: 전체 다시 받기 once it is there, then the button.
  constexpr float X = 840, W = 386, Row = 56, RowTop = 226;
  paint_.rect(818, 172, 430, 484, Surface, 20);
  paint_.fonts.text(tr("Manage|app"), X, 192, 22, Ink, true);
  if (present) {
    hit(X, RowTop, W, Row, [this] {
      retroArchPart_ = FullRow;
      action(Action::Accept);
    });
    if (retroArchPart_ == FullRow) paint_.focusRow(X, RowTop, W, Row, Ground, 12);
    else paint_.rect(X, RowTop, W, Row, Ground, 12);
    paint_.icon(19, X + 16, RowTop + Row / 2 - 12, Ink, 24);
    paint_.fonts.middle(tr("Get all again"), X + 52, RowTop + Row / 2, 18, Ink, true, W - 70);
    paint_.fonts.wrapped(tr("Only changed files and missing cores come down when you check. Get all again fetches every file anew; either way your settings stay."),
                         X, RowTop + Row + 16, 16, Secondary, W, 5);
  } else {
    paint_.fonts.wrapped(tr("RetroArch is not installed. Install it to play retro games."), X, RowTop + 6, 17, Secondary, W, 3);
  }
  paint_.rect(X, 494, W, 1, Line);
  paint_.fonts.middle(tr("Size|info"), X + 12, 530, 18, Secondary);
  const auto sizeText = size ? formatBytes(size) : std::string("-");
  paint_.fonts.middle(sizeText, X + W - 12 - paint_.fonts.measure(sizeText, 24, true), 530, 24, Ink, true);
  buttonHit({X, 568, W, 60}, 14, [this] {
    retroArchPart_ = Button;
    action(Action::Accept);
  });
  if (retroArchPart_ == Button) paint_.ring(X, 568, W, 60, 14);
  const bool downloading = retroBusy_ == "pack";
  if (downloading || retroArchQueuedAt() >= 0) {
    double percent = 0;
    if (downloading && retroBytes_) percent = std::min(100.0, 100.0 * double(retroReceived_.load()) / double(retroBytes_));
    onTheWayButton(X, W, downloading, percent,
                   downloading && retroBytes_ ? 100.0 * double(retroKept_.load()) / double(retroBytes_) : 0);
  } else {
    paint_.rect(X, 568, W, 60, Butter, 14);
    const std::string label = present ? tr("Check for updates|retroarch") : tr("Install RetroArch");
    paint_.keyLabel("A", label, X + (W - paint_.keyLabelWidth("A", label, 22, true)) / 2, 598, 22, Ink, true);
  }
}
} // namespace foil::ui
