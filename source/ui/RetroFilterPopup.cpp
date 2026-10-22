// 레트로 게임's filter popup: the server's groups of options as chips that
// wrap, each group under its name, the sorts in a column to the right.
// Nothing here knows a filter by name; the server's book says it all.
#include "ui/Application.hpp"
#include "core/I18n.hpp"
#include <algorithm>
#include <cmath>
namespace foil::ui {
namespace {
constexpr float PopupWidth = 1080, SortWidth = 240, ChipH = 40, ChipGap = 10, HeadingH = 34, GroupGap = 12, MostBody = 430;
constexpr float ChipArea = PopupWidth - 96 - SortWidth; // the chips' width, left of the sorts
constexpr int ChipText = 17, CountText = 14;
constexpr uint64_t MB = 1024 * 1024;
retro::Texts mine(const char *key) { return {{"", tr(key)}}; }
} // namespace
// The client's own few, for a server that words none.
retro::FilterBook Application::retroFilterBook() const {
  if (!retroBook_.empty()) return retroBook_;
  retro::FilterBook book;
  book.groups = {
      {"rating", mine("Rating|group"), true,
       {{"80", mine("Rated 80+"), 0, {{"minRating", "80"}}}, {"90", mine("Rated 90+"), 0, {{"minRating", "90"}}}}},
      {"bezel", mine("Bezel"), false, {{"own", mine("Own bezel"), 0, {{"bezel", "1"}}}}},
      {"size", mine("Size|info"), true,
       {{"1mb", mine("Under 1 MB"), 0, {{"maxSize", std::to_string(MB)}}},
        {"10mb", mine("Under 10 MB"), 0, {{"maxSize", std::to_string(10 * MB)}}},
        {"100mb", mine("100 MB and up"), 0, {{"minSize", std::to_string(100 * MB)}}}}},
  };
  book.sorts = {{"rating", {{"", sortLabel(SortMode::Score)}}},
                {"name", {{"", sortLabel(SortMode::Name)}}},
                {"size", {{"", sortLabel(SortMode::Largest)}}},
                {"added", {{"", sortLabel(SortMode::Recent)}}}};
  return book;
}
void Application::clearRetroFilters() {
  retroBook_ = {};
  retroBookSystem_.clear();
  retroPicks_.clear();
}
std::string Application::retroFilterSummary() const {
  const auto book = retroFilterBook();
  const auto picks = retro::keptPicks(book, retroPicks_);
  std::string summary = tr("All");
  if (!picks.empty()) summary = retro::textIn(retro::findOption(book, picks[0].first, picks[0].second)->text, languageCode());
  if (picks.size() > 1) summary = trf("{0} +{1}", {summary, std::to_string(picks.size() - 1)});
  const auto sort = std::find_if(book.sorts.begin(), book.sorts.end(), [&](const auto &s) { return s.key == retroSortKey_; });
  if (sort != book.sorts.end()) summary += " · " + retro::textIn(sort->text, languageCode());
  return summary;
}
// Chip 0 is 전체 (no filter); the rest flow left to right under their group.
Application::FilterLayout Application::retroFilterLayout(float width) {
  FilterLayout out;
  const auto book = retroFilterBook();
  const auto lang = languageCode();
  float x = 0, y = 0;
  int line = 0;
  const auto place = [&](const std::string &group, const std::string &option, const std::string &label, int count) {
    const std::string number = count > 0 ? std::to_string(count) : "";
    float w = paint_.fonts.measure(label, ChipText, true) + 32;
    if (!number.empty()) w += paint_.fonts.measure(number, CountText) + 8;
    w = std::min(w, width);
    if (x > 0 && x + w > width) {
      x = 0;
      y += ChipH + ChipGap;
      ++line;
    }
    out.chips.push_back({group, option, label, number, x, y, w, line});
    x += w + ChipGap;
  };
  place("", "", tr("All"), 0);
  for (const auto &group : book.groups) {
    y += ChipH + GroupGap;
    out.headings.push_back({y, retro::textIn(group.text, lang)});
    y += HeadingH;
    x = 0;
    ++line;
    for (const auto &option : group.options) place(group.key, option.key, retro::textIn(option.text, lang), option.count);
  }
  out.height = y + ChipH;
  return out;
}
void Application::retroFilterPopup() {
  const auto book = retroFilterBook();
  const auto lang = languageCode();
  const float sortsH = 44 + book.sorts.size() * 44.f;
  auto layout = retroFilterLayout(ChipArea);
  const float visible = std::min(MostBody, std::max(layout.height + 44 + 16, sortsH + 16));
  const auto body = modal(PopupWidth, visible, tr("Filter and sort"), tr("Cancel"), tr("Apply"), "", 10);
  const float left = body.x + 32, area = ChipArea;
  const float sortLeft = body.x + PopupWidth - 32 - SortWidth, top = body.y + 44, view = visible - 44 - 8;
  paint_.fonts.middle(tr("Filter"), left, body.y + 26, 16, Secondary, true);
  paint_.fonts.middle(tr("Sort"), sortLeft, body.y + 26, 16, Secondary, true);
  paint_.rect(sortLeft - 18, body.y + 16, 1, visible - 24, Line);
  // The cursor's chip stays in view, its group's name with it.
  filterFocus_ = std::clamp(filterFocus_, 0, int(layout.chips.size()) - 1);
  const auto &focus = layout.chips[size_t(filterFocus_)];
  const float headed = focus.x == 0 && focus.option != "" ? focus.y - HeadingH : focus.y;
  if (headed - filterScroll_ < 0) filterScroll_ = headed;
  if (focus.y + ChipH - filterScroll_ > view) filterScroll_ = focus.y + ChipH - view;
  filterScroll_ = std::clamp(filterScroll_, 0.f, std::max(0.f, layout.height - view));
  clipTo(left - 10, top - 6, area + 20, view + 16);
  for (const auto &[y, name] : layout.headings)
    paint_.fonts.middle(name, left, top + y - filterScroll_ + HeadingH / 2 - 2, 15, Secondary, true, area);
  for (int n = 0; n < int(layout.chips.size()); ++n) {
    const auto &chip = layout.chips[size_t(n)];
    const float x = left + chip.x, y = top + chip.y - filterScroll_;
    if (y + ChipH < top - 10 || y > top + view + 10) continue;
    const bool on = chip.group.empty() ? draftPicks_.empty()
                                       : std::count(draftPicks_.begin(), draftPicks_.end(), std::make_pair(chip.group, chip.option));
    const bool focused = popupColumn_ == 0 && filterFocus_ == n;
    if (y >= top - 4 && y + ChipH <= top + view + 4)
      hit(x, y, chip.w, ChipH, [this, n] {
        popupColumn_ = 0;
        filterFocus_ = n;
        retroFilterAction(Action::Accept);
      });
    // The cursor rings the chip, so it shows on a picked one too.
    if (focused) paint_.ring(x, y, chip.w, ChipH, 20);
    paint_.rect(x, y, chip.w, ChipH, on ? Butter : Line, 20);
    paint_.rect(x + 1, y + 1, chip.w - 2, ChipH - 2, on ? Butter : Surface, 19);
    const float labelW = paint_.fonts.measure(chip.label, ChipText, true);
    paint_.fonts.middle(chip.label, x + 16, y + ChipH / 2, ChipText, Ink, on, chip.w - 32);
    if (!chip.count.empty())
      paint_.fonts.middle(chip.count, x + 16 + labelW + 8, y + ChipH / 2 + 1, CountText, on ? Ink : Secondary);
  }
  unclip();
  if (layout.height > view) {
    const float bar = view * view / layout.height, at = filterScroll_ / layout.height * view;
    paint_.rect(left + area + 10, top + at, 4, bar, Line, 2);
  }
  for (int n = 0; n < int(book.sorts.size()); ++n) {
    const float y = top + n * 44.f, h = 38, mid = y + h / 2;
    const bool on = book.sorts[size_t(n)].key == draftSortKey_, focused = popupColumn_ == 1 && popupRow_ == n;
    hit(sortLeft, y, SortWidth, h, [this, n] {
      popupColumn_ = 1;
      popupRow_ = n;
      retroFilterAction(Action::Accept);
    });
    const SDL_Color fill = on ? SDL_Color{255, 241, 204, 255} : focused ? Ground : Surface;
    if (focused) paint_.focusRow(sortLeft, y, SortWidth, h, fill, 10);
    else paint_.rect(sortLeft, y, SortWidth, h, fill, 10);
    paint_.rect(sortLeft + 12, mid - 10, 20, 20, on ? Ink : SDL_Color{176, 174, 167, 255}, 10);
    paint_.rect(sortLeft + 14, mid - 8, 16, 16, on ? Butter : Surface, 8);
    if (on) paint_.rect(sortLeft + 18, mid - 4, 8, 8, Ink, 4);
    paint_.fonts.middle(retro::textIn(book.sorts[size_t(n)].text, lang), sortLeft + 42, mid, 18, Ink, on, SortWidth - 52);
  }
}
// Up and down go to the chip nearest the cursor on the line above or below.
void Application::retroFilterAction(Action a) {
  const auto book = retroFilterBook();
  const auto layout = retroFilterLayout(ChipArea);
  const int chips = int(layout.chips.size()), sorts = int(book.sorts.size());
  filterFocus_ = std::clamp(filterFocus_, 0, chips - 1);
  const auto &here = layout.chips[size_t(filterFocus_)];
  const auto nearest = [&](int line) {
    int best = -1;
    float gap = 1e9f;
    for (int n = 0; n < chips; ++n) {
      const auto &c = layout.chips[size_t(n)];
      const float d = std::fabs((c.x + c.w / 2) - (here.x + here.w / 2));
      if (c.line == line && d < gap) best = n, gap = d;
    }
    return best;
  };
  if (popupColumn_ == 0) {
    if (a == Action::Up && here.line > 0) filterFocus_ = std::max(0, nearest(here.line - 1));
    if (a == Action::Down) {
      if (const int below = nearest(here.line + 1); below >= 0) filterFocus_ = below;
    }
    if (a == Action::Left && filterFocus_ > 0 && layout.chips[size_t(filterFocus_ - 1)].line == here.line) --filterFocus_;
    if (a == Action::Right) {
      if (filterFocus_ + 1 < chips && layout.chips[size_t(filterFocus_ + 1)].line == here.line) ++filterFocus_;
      else if (sorts > 0) popupColumn_ = 1, popupRow_ = std::clamp(popupRow_, 0, sorts - 1);
    }
  } else {
    if (a == Action::Up) popupRow_ = std::max(0, popupRow_ - 1);
    if (a == Action::Down) popupRow_ = std::min(sorts - 1, popupRow_ + 1);
    if (a == Action::Left) popupColumn_ = 0;
  }
  if (a == Action::Accept) {
    if (popupColumn_ == 1 && popupRow_ < sorts) draftSortKey_ = book.sorts[size_t(popupRow_)].key;
    else if (popupColumn_ == 0 && here.group.empty()) draftPicks_.clear();
    else if (popupColumn_ == 0) draftPicks_ = retro::togglePick(book, draftPicks_, here.group, here.option);
  }
  if (a == Action::Queue) {
    viewPopup_ = false;
    selected_ = 0;
    retroPicks_ = draftPicks_;
    retroSortKey_ = draftSortKey_;
    retroGames_.clear();
    retroListSystem_.clear();
    retroTotal_ = 0;
    if (!retroListWorker_.joinable()) loadRetroPage(false);
  }
  if (a == Action::Back) viewPopup_ = false;
}
} // namespace foil::ui
