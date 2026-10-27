#include "ui/Application.hpp"
#include "ui/Theme.hpp"
#include <algorithm>
#include <iterator>
namespace foil::ui {
// Row 0 follows the console; row i is ThemeNames[i - 1].
int Application::themeCount() { return 1 + int(std::size(ThemeNames)); }
int Application::themeRowOf(const std::string &theme) {
  for (size_t i = 0; i < std::size(ThemeNames); ++i)
    if (theme == ThemeNames[i]) return int(i) + 1;
  return 0;
}
const Palette &Application::themePalette(int row) const {
  if (row > 0 && row < themeCount()) return *ThemePalettes[size_t(row - 1)];
  return consoleDark_ ? DarkPalette : LightPalette;
}
std::string Application::themeLabel(int row) {
  switch (row) {
  case 0: return tr("Same as the console");
  case 1: return tr("Beige|theme");
  case 2: return tr("Black|theme");
  case 3: return tr("Navy|theme");
  case 4: return tr("Purple|theme");
  case 5: return tr("Green|theme");
  case 6: return tr("Pink|theme");
  }
  return "";
}
void Application::openThemePicker() {
  themePopup_ = true;
  themeRow_ = themeRowOf(state_.theme);
}
// Moving through the rows shows each palette at once; B goes back to the one
// chosen, A keeps the one shown.
void Application::themeAction(Action a) {
  const int rows = ThemeRows, count = themeCount();
  const int before = themeRow_;
  if (a == Action::Up && themeRow_ % rows > 0) --themeRow_;
  if (a == Action::Down && themeRow_ % rows < rows - 1 && themeRow_ + 1 < count) ++themeRow_;
  if (a == Action::Left && themeRow_ >= rows) themeRow_ -= rows;
  if (a == Action::Right && themeRow_ + rows < count) themeRow_ += rows;
  if (themeRow_ != before) usePalette(themePalette(themeRow_));
  if (a == Action::Accept) {
    themePopup_ = false;
    state_.theme = themeRow_ > 0 ? ThemeNames[size_t(themeRow_ - 1)] : "";
    applyTheme();
    saveOwnState();
  }
  if (a == Action::Back) {
    themePopup_ = false;
    applyTheme();
  }
}
// A palette in small: its page, a card on it, a line of its text and butter.
void Application::themeSwatch(const Palette &colors, float x, float y, float w, float h) {
  paint_.rect(x - 1, y - 1, w + 2, h + 2, Line, 7);
  paint_.rect(x, y, w, h, colors.ground, 6);
  paint_.rect(x + 6, y + 6, w - 12, h - 12, colors.surface, 4);
  paint_.rect(x + 11, y + 11, w - 34, 4, colors.ink, 2);
  paint_.rect(x + 11, y + h - 15, w - 44, 4, colors.secondary, 2);
  paint_.rect(x + w - 18, y + h - 18, 8, 8, Butter, 4);
}
void Application::themeDialog() {
  constexpr float Row = 60, Gap = 8;
  const int count = themeCount(), rows = ThemeRows, columns = (count + rows - 1) / rows;
  const auto body = modal(820, 16 + rows * (Row + Gap) + 8, tr("Theme"), tr("Close"), "", "", 49);
  const float w = (body.w - 48 - Gap * 2 * (columns - 1)) / columns;
  const int chosen = themeRowOf(state_.theme);
  for (int i = 0; i < count; ++i) {
    const float x = body.x + 24 + (i / rows) * (w + Gap * 2), y = body.y + 16 + (i % rows) * (Row + Gap);
    rowHit(x, y, w, Row, 12, [this, i] {
      if (themeRow_ != i) {
        themeRow_ = i;
        usePalette(themePalette(i));
      }
      action(Action::Accept);
    });
    if (i == themeRow_) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    themeSwatch(themePalette(i), x + 16, y + (Row - 36) / 2, 52, 36);
    const bool inUse = i == chosen;
    paint_.fonts.middle(themeLabel(i), x + 84, y + Row / 2, 21, Ink, true, w - 84 - (inUse ? 52 : 12));
    if (inUse) paint_.icon(8, x + w - 44, y + (Row - 26) / 2, Good, 26);
  }
}
} // namespace foil::ui
