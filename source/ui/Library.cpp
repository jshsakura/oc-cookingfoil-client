#include "ui/Application.hpp"
#include <algorithm>
namespace foil::ui {
// The search box at the top right, shared with 레트로 게임's list: key
// badge, icon, text. An active query reads as content, not as a placeholder.
void Application::searchBar(const std::string &query, float x, float w) {
  headerItem("search", x, 24, w, 48, [this] { searchRequested_ = true; }, x == SearchX);
  paint_.rect(x, 24, w, 48, Surface, 12);
  paint_.key("X", x + 18, 48);
  paint_.icon(1, x + 60, 38, query.empty() ? Secondary : Ink, 20);
  paint_.fonts.middle(query.empty() ? tr("Search") : query, x + 88, 48, 18,
                      query.empty() ? Secondary : Ink, !query.empty(), w - 106);
}
void Application::library() {
  const auto &list = libraryList();
  // Nothing listed: the cursor waits on the search box.
  if (list.empty() && inContent()) headerFocus_ = "search";
  const auto count = countLabel(list.size(), query_);
  searchBar(query_);
  // Filter and sort live in a popup (Y); the button says what is applied.
  std::string chipTitle = tr("All");
  if (!activeChips_.empty() && activeChips_.front() < int(chips_.size())) {
    chipTitle = chips_[activeChips_.front()].title;
    if (activeChips_.size() > 1)
      chipTitle = trf("{0} +{1}", {chipTitle, std::to_string(activeChips_.size() - 1)});
  }
  const auto summary = chipTitle + " · " + sortLabel(librarySort_);
  const float viewWidth = std::min(330.f, paint_.keyLabelWidth("Y", summary, 18, true) + 66);
  const float viewX = SearchX - 16 - viewWidth;
  pageTitle(tr("All games"), count, viewX);
  headerItem("filter", viewX, 24, viewWidth, 48, [this] { action(Action::Queue); });
  paint_.rect(viewX, 24, viewWidth, 48, Surface, 12);
  paint_.key("Y", viewX + 18, 48);
  paint_.icon(10, viewX + 60, 38, Ink, 20);
  paint_.fonts.middle(summary, viewX + 88, 48, 18, Ink, true, viewWidth - 106);
  if (groups_.empty() && session_ && !error_) {
    paint_.skeletonCards(260, 104, 12); // the catalog is still on its way
    return;
  }
  if (list.empty()) {
    notice(tr("No games match."), tr("Press Y to change filters or X to change the search."), 23);
    return;
  }
  // Rows of six; about two and a half fit on screen. The row at the top
  // follows the cursor, or the finger (fractional while it glides).
  constexpr float Top = 104, RowHeight = CardRowStep;
  const int rows = int((list.size() + 5) / 6);
  const float top = glide("library", 232, Top - 8, 1048, 660 - Top + 8, float(std::max(0, selected_ / 6 - 1)),
                          float(rows - 2), RowHeight);
  clipTo(232, Top - 8, 1048, 660 - Top + 8);
  for (int row = std::max(0, int(top)); row < int(top) + 4 && row < rows; ++row)
    for (int col = 0; col < 6 && row * 6 + col < int(list.size()); ++col) {
      const int n = row * 6 + col;
      card(groups_[list[n]], 260 + col * 170, Top + (row - top) * RowHeight, inContent() && selected_ == n, true,
           [this, n] { selected_ = n; });
    }
  unclip();
  paint_.scrollbar(1266, 104, 540, top, 2.3f, float(rows));
}
} // namespace foil::ui
