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
  const int first = std::max(0, selected_ / 6 - 1) * 6;
  for (int n = 0; n < 18 && first + n < int(list.size()); ++n)
    card(groups_[list[first + n]], 260 + n % 6 * 170, 104 + n / 6 * 240,
         inContent() && selected_ == first + n);
  // Rows of six; about two and a half fit on screen.
  paint_.scrollbar(1266, 104, 540, float(first / 6), 2.3f, float((list.size() + 5) / 6));
}
} // namespace foil::ui
