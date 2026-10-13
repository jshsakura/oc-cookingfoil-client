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
  paint_.fonts.middle(query.empty() ? tr("검색어 입력") : query, x + 88, 48, 18,
                      query.empty() ? Secondary : Ink, !query.empty(), w - 106);
}
void Application::library() {
  const auto &list = libraryList();
  // Nothing listed: the cursor waits on the search box.
  if (list.empty() && inContent()) headerFocus_ = "search";
  auto count = std::to_string(list.size());
  if (!query_.empty())
    count += " · " + trf("'{0}' 검색 결과", {query_});
  searchBar(query_);
  // Filter and sort live in a popup (Y); the button says what is applied.
  std::string chipTitle = tr("전체");
  if (!activeChips_.empty() && activeChips_.front() < int(chips_.size())) {
    chipTitle = chips_[activeChips_.front()].title;
    if (activeChips_.size() > 1)
      chipTitle = trf("{0} 외 {1}", {chipTitle, std::to_string(activeChips_.size() - 1)});
  }
  const auto summary = chipTitle + " · " + sortLabel(librarySort_);
  const float viewWidth = std::min(330.f, paint_.keyLabelWidth("Y", summary, 18, true) + 66);
  const float viewX = SearchX - 16 - viewWidth;
  pageTitle(tr("모든 게임"), count, viewX);
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
    notice(tr("조건에 맞는 게임이 없습니다."), tr("Y로 필터를 바꾸거나 X로 검색어를 바꿉니다."), 23);
    return;
  }
  // Rows of six; about two and a half fit on screen. The row at the top
  // follows the cursor, or the finger (fractional while it glides).
  constexpr float Top = 104, RowHeight = 240;
  const int rows = int((list.size() + 5) / 6);
  const float top = glide("library", 232, Top - 8, 1048, 660 - Top + 8, float(std::max(0, selected_ / 6 - 1)),
                          float(rows - 2), RowHeight);
  clipTo(232, Top - 8, 1048, 660 - Top + 8);
  for (int row = int(top); row < int(top) + 4 && row < rows; ++row)
    for (int col = 0; col < 6 && row * 6 + col < int(list.size()); ++col) {
      const int n = row * 6 + col;
      card(groups_[list[n]], 260 + col * 170, Top + (row - top) * RowHeight, inContent() && selected_ == n);
    }
  unclip();
  paint_.scrollbar(1266, 104, 540, top, 2.3f, float(rows));
}
} // namespace foil::ui
