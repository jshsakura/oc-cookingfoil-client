#include "ui/Application.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
constexpr float ShelfHeight = 283, BannerHeight = 64, CardStep = 170, Card = 150;
constexpr int Columns = 6;
} // namespace
// The retro home: RetroArch first while it is missing, then one shelf per
// system with its best rated games and a 모두 보기 card at the end.
void Application::retroHome() {
  pageTitle(tr("레트로 게임"), "", 0);
  // Y gets RetroArch, or checks it for new files once it is there.
  const std::string get = retroBusy_ == "pack"  ? tr("설치 취소")
                          : retroArchPresent() ? tr("RetroArch 업데이트")
                                               : tr("RetroArch 설치");
  const float getW = paint_.keyLabelWidth("Y", get, 19, true) + 40;
  headerItem("retroarch", 1252 - getW, 24, getW, 48, [this] { action(Action::Queue); });
  paint_.rect(1252 - getW, 24, getW, 48, Surface, 12);
  paint_.keyLabel("Y", get, 1272 - getW, 48, 19, Ink, true);
  const int rows = int(retroShelves_.size());
  retroRow_ = std::clamp(retroRow_, 0, std::max(0, rows - 1));
  if (retroRow_ >= 0 && retroRow_ < rows)
    retroCol_ = std::clamp(retroCol_, 0, int(retroShelves_[retroRow_].games.size()));
  if (retroShelves_.empty()) {
    retroBanner(90); // RetroArch's state, always on the home
    const float top = 178;
    if (!connected_)
      notice(tr("서버에 연결되어 있지 않습니다."), tr("서버에 연결한 뒤 레트로 게임을 불러옵니다."), 21);
    else if (retroMissing_)
      notice(tr("이 서버는 레트로 게임을 제공하지 않습니다."), tr("서버에 레트로 폴더를 만들면 여기에 나타납니다."), 35);
    else if (retroError_ && retroRetryAt_)
      notice(tr("서버가 레트로 파일을 읽고 있습니다."), tr("잠시 뒤 자동으로 다시 불러옵니다."), 35);
    else if (retroError_)
      notice(tr("레트로 게임 목록을 불러오지 못했습니다."), retroError_.message, 25);
    else if (!retroBusy_.empty() || !retroLoaded_) {
      paint_.skeleton(260, top, 240, 28, 8);
      paint_.skeletonCards(260, top + 46, 6);
    } else
      notice(tr("레트로 게임이 없습니다."), tr("서버의 레트로 폴더에 롬을 넣으면 여기에 나타납니다."), 35);
    return;
  }
  // The focused shelf rides at the top once past the first; the banner
  // scrolls away with the rest.
  const float base = 90 + BannerHeight + 24;
  const float scroll = inContent() && retroRow_ >= 1 ? base - 90 + retroRow_ * ShelfHeight : 0;
  // Everything under the title scrolls; the title stays.
  SDL_Rect page;
  SDL_RenderGetClipRect(paint_.renderer, &page);
  const SDL_Rect below{232, 84, 1048, 576};
  SDL_RenderSetClipRect(paint_.renderer, &below);
  retroBanner(90 - scroll);
  for (int n = 0; n < rows; ++n) {
    const float y = base + n * ShelfHeight - scroll;
    if (y > 660) break;
    if (y + ShelfHeight < 90) continue;
    const auto &shelf = retroShelves_[n];
    const bool focused = inContent() && retroRow_ == n;
    // Header: the system's icon on a white tile, its name, its game count.
    constexpr float HeaderMid = 17;
    float x = 260;
    if (!shelf.system.iconUrl.empty()) {
      paint_.rect(x, y + HeaderMid - 17, 34, 34, {255, 255, 255, 255}, 9);
      images_->draw(shelf.system.iconUrl, x + 4, y + HeaderMid - 13, 26, 26, 4);
      x += 44;
    }
    x = paint_.fonts.middle(retro::systemName(shelf.system, languageCode()), x, y + HeaderMid, 26, Ink, true, 600);
    paint_.pill(std::to_string(shelf.system.romCount), x + 12, y + HeaderMid - 13);
    if (focused) paint_.keyChip("R", tr("모두 보기"), 1252, y + HeaderMid);
    hit(1100, y, 160, 34, [this, n] { openRetroList(n); });
    const int count = int(shelf.games.size());
    const int first = focused ? std::max(0, retroCol_ - (Columns - 1)) : 0;
    for (int col = 0; col < Columns && first + col <= count; ++col) {
      const int i = first + col;
      const float cx = 260 + CardStep * col, cy = y + 46;
      const bool on = focused && retroCol_ == i;
      hit(cx, cy, Card, Card + 30, [this, n, i, on] {
        sidebar_ = false;
        retroRow_ = n;
        retroCol_ = i;
        if (on) action(Action::Accept);
      });
      if (i == count) {
        // The shelf's last card: the system's whole list.
        if (on) paint_.ring(cx, cy, Card, Card);
        paint_.rect(cx, cy, Card, Card, Surface, 14);
        paint_.icon(9, cx + Card / 2 - 18, cy + 46, Ink, 36);
        const std::string all = tr("모두 보기");
        paint_.fonts.middle(all, cx + (Card - paint_.fonts.measure(all, 18, true)) / 2, cy + 112, 18, Ink, true);
        continue;
      }
      const auto &game = shelf.games[i];
      cover(thumbnailUrl(game.iconUrl), cx, cy, int(Card), on, game.name, true);
      // Size top left and score top right, as on 모든 게임's cards.
      if (game.size > 0) {
        const auto label = formatBytes(game.size);
        const float w = paint_.fonts.measure(label, 13, true) + 10, h = 19;
        paint_.rect(cx + 6, cy + 6, w, h, {22, 22, 26, 190}, 5);
        paint_.fonts.middle(label, cx + 11, cy + 6 + h / 2, 13, Surface, true);
      }
      scoreBadge(game.rating, cx + Card - 6, cy + 6);
      // On the card already: a check on the bottom right.
      if (retroInstalled(game) && !retroBezelMissing(game)) {
        paint_.rect(cx + Card - 30, cy + Card - 30, 24, 24, Good, 12);
        paint_.icon(8, cx + Card - 27, cy + Card - 27, Surface, 18);
      }
      if (retroBusy_ == game.id) {
        const float ratio = retroBytes_ ? std::min(1.f, float(retroReceived_.load()) / float(retroBytes_)) : 0.f;
        paint_.rect(cx + 10, cy + Card - 18, Card - 20, 8, {22, 22, 26, 190}, 4);
        paint_.rect(cx + 10, cy + Card - 18, std::max(8.f, (Card - 20) * ratio), 8, Butter, 4);
      }
      // Two lines under the cover, as on 홈's shelves.
      paint_.fonts.wrapped(game.name, cx, cy + 160, 18, Ink, Card, 2);
    }
  }
  SDL_RenderSetClipRect(paint_.renderer, SDL_RectEmpty(&page) ? nullptr : &page);
  paint_.scrollbar(1266, 90, 560, float(std::max(0, retroRow_)), 1.6f, float(rows));
}
void Application::retroHomeAction(Action a) {
  const int rows = int(retroShelves_.size());
  const auto games = [&] { return retroRow_ >= 0 && retroRow_ < rows ? int(retroShelves_[retroRow_].games.size()) : 0; };
  switch (a) {
  case Action::Up:
    retroRow_ = std::max(0, retroRow_ - 1);
    retroCol_ = std::min(retroCol_, games());
    break;
  case Action::Down:
    retroRow_ = std::min(rows - 1, retroRow_ + 1);
    retroCol_ = std::min(retroCol_, games());
    break;
  case Action::Left:
    if (retroCol_ > 0) retroCol_--;
    else sidebar_ = true;
    break;
  case Action::Right:
    retroCol_ = std::min(games(), retroCol_ + 1);
    break;
  case Action::Next:
    openRetroList(retroRow_);
    break;
  case Action::Accept:
    if (retroCol_ >= games()) openRetroList(retroRow_);
    else if (const auto *game = retroFocused()) askRetroGame(*game);
    break;
  default:
    break;
  }
}
} // namespace foil::ui
