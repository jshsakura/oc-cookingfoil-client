#include "ui/Application.hpp"
#include <cmath>
#include <algorithm>
#include <array>
namespace foil::ui {
// Icon that says what a home row is about.
int themeIcon(const Theme &t) {
  if (t.onSale) return 15;                       // tag
  if (t.minScore > 0) return 31;                 // star
  if (t.sort == SortMode::Updated) return 19;    // branch
  if (t.category == "Party") return 13;          // users: a party row
  if (!t.category.empty()) return 22;            // package
  if (t.filter == Filter::Korean) return 17;     // globe
  if (t.minPlayers > 0) return 13;               // users
  if (t.minSize > 0) return 14;                  // hard drive
  if (t.sort == SortMode::Popular) return 28;    // trophy
  if (t.sort == SortMode::Release) return 29;    // sparkle
  return 30;                                     // clock
}
void Application::home() {
  if (groups_.empty()) {
    if (!session_) {
      const bool none = std::none_of(config_.servers.begin(), config_.servers.end(),
                                     [](const auto &s) { return s.enabled; });
      notice(config_.servers.empty() ? tr("등록된 서버가 없습니다.")
             : none                  ? tr("켜 둔 서버가 없습니다.")
                                     : tr("설정에서 연결할 서버를 고릅니다."),
             tr("설정의 + 서버 추가에서 서버 주소와 계정을 입력합니다."), 21);
      return;
    }
    if (error_) {
      notice(pairingPending_ ? tr("기기 승인을 기다립니다.")
             : error_.message == net::offlineMessage()
                 ? tr("인터넷에 연결되어 있지 않습니다.")
                 : tr("서버에 연결하지 못했습니다."),
             error_.message + "\n" + tr("설정에서 서버를 확인하고 - 버튼으로 새로고침합니다."),
             error_.message == net::offlineMessage() ? 20 : 25);
      return;
    }
    if (connected_) {
      notice(tr("표시할 게임이 없습니다."), tr("서버에 게임이 들어오면 여기에 보입니다."), 22);
      return;
    }
    // Connecting: the shape of the home screen, pulsing.
    paint_.skeleton(260, 24, 992, 248, 20);
    paint_.skeleton(260, 300, 160, 28, 8);
    paint_.skeletonCards(260, 346, 6);
    return;
  }
  constexpr int RowTop = 300, RowHeight = 283, Columns = 6;
  constexpr uint32_t HeroMs = 6000, HeroWaitMs = 6000;
  // What a slide draws: its backdrop (the admin banner or the blurred cover)
  // and the cover itself.
  auto slideImages = [this](int slide) {
    const auto &icon = groups_[heroes_[slide].index].representative.icon;
    const bool banner = !heroBanner_.empty() && heroFeatured_ && slide == 0;
    return std::array<std::string, 2>{banner ? thumbnailUrl(heroBanner_, "xl") : thumbnailUrl(icon),
                                       thumbnailUrl(icon, "md")};
  };
  auto ready = [&](int slide) {
    for (const auto &url : slideImages(slide))
      if (!url.empty() && !images_->loaded(url) && !images_->failed(url))
        return false;
    return true;
  };
  if (heroes_.size() > 1) {
    const int count = int(heroes_.size());
    const int next = (heroSlide_ + 1) % count, previous = (heroSlide_ + count - 1) % count;
    // The neighbours load in the background, so a turn shows them at once.
    for (const int slide : {next, previous})
      for (const auto &url : slideImages(slide))
        if (!url.empty())
          images_->request(url);
    // The banner turns on its own unless the cursor is on it, and waits
    // (a little while at most) for the next slide's pictures to arrive.
    const uint32_t shown = SDL_GetTicks() - heroAt_;
    if ((row_ != 0 || sidebar_) && shown > HeroMs && (ready(next) || shown > HeroMs + HeroWaitMs))
      showHero(next);
  }
  const auto &g = groups_[hero_];
  // Keep the focused row where the first row sits; the hero scrolls away.
  // A finger moves the page freely (glide); otherwise it follows the cursor.
  const int scroll = std::lround(glide("home", 232, 0, 1048, 660, !sidebar_ && row_ >= 2 ? (row_ - 1) * RowHeight : 0,
                                       RowTop + float(themes_.size()) * RowHeight + 16 - 660, 1));
  hit(260, 24 - scroll, 992, 248, [this] { openDetail(hero_); });
  // Sideways the banner follows the finger a little, then turns (heroFlick).
  const float hx = -glide("hero", 260, 24 - scroll, 992, 248, 0, 0, 1, true);
  if (row_ == 0 && !sidebar_)
    paint_.ring(260, 24 - scroll, 992, 248, 20);
  paint_.rect(260, 24 - scroll, 992, 248, {10, 10, 14, 255}, 20);
  clipTo(260, 24 - scroll, 992, 248);
  // The backdrop breathes in dark shades only while its picture is on its way.
  if (const auto backdrop = slideImages(heroSlide_)[0];
      !backdrop.empty() && !images_->loaded(backdrop) && !images_->failed(backdrop))
    paint_.skeleton(260 + hx, 24 - scroll, 992, 248, 20, true);
  // An admin banner shows as is under a dark wash; otherwise the cover blurs.
  if (!heroBanner_.empty() && heroFeatured_ && heroSlide_ == 0) {
    images_->draw(thumbnailUrl(heroBanner_, "xl"), 260 + hx, 24 - scroll, 992, 248, 20);
    paint_.rect(260 + hx, 24 - scroll, 992, 248, {10, 10, 14, 150}, 20);
  } else {
    images_->draw(thumbnailUrl(g.representative.icon), 260 + hx, 24 - scroll, 992, 248, 20, true);
  }
  cover(thumbnailUrl(g.representative.icon, "md"), 296 + hx, 56 - scroll, 184, false, g.representative.name, false, true);
  paint_.fonts.text(heroes_.empty() ? tr("추천 게임") : heroes_[heroSlide_].label, 512 + hx,
                    61 - scroll, 18, Butter, true);
  paint_.fonts.text(g.representative.name, 512 + hx, 98 - scroll, 40, Surface, true,
                    700);
  paint_.fonts.text(g.representative.publisher, 512 + hx, 155 - scroll, 19,
                    {228, 228, 234, 255}, false, 680);
  float x = 512 + hx;
  const auto &languages = g.representative.languages;
  if (std::find(languages.begin(), languages.end(), "ko") != languages.end())
    x += paint_.pill(tr("한국어"), x, 199 - scroll, true, 17) + 10;
  if (g.update)
    x += paint_.pill(tr("업데이트 포함"), x, 199 - scroll, true, 17) + 10;
  if (!g.dlc.empty())
    x += paint_.pill("DLC " + std::to_string(g.dlc.size()), x, 199 - scroll, true, 17) + 10;
  if (g.base && !g.base->priceDiscount.empty())
    paint_.pill(trf("eShop 할인가 {0}", {priceText(g.base->priceDiscount)}), x, 199 - scroll, true, 17);
  else if (g.base && !g.base->priceRegular.empty())
    paint_.pill("eShop " + priceText(g.base->priceRegular), x, 199 - scroll, true, 17);
  unclip();
  // Slide dots, bottom right: the current one is a wide butter pill.
  if (heroes_.size() > 1) {
    constexpr float Dot = 8, Wide = 24, Gap = 8;
    const float width = Wide + (heroes_.size() - 1) * (Dot + Gap);
    float dx = 1252 - 32 - width;
    const float dy = 24 + 248 - 30 - scroll;
    for (int n = 0; n < int(heroes_.size()); ++n) {
      const bool on = n == heroSlide_;
      const float w = on ? Wide : Dot;
      hit(dx - 4, dy - 12, w + 8, Dot + 24, [this, n] { showHero(n); });
      paint_.rect(dx, dy, w, Dot, on ? Butter : SDL_Color{255, 255, 255, 110}, 4);
      dx += w + Gap;
    }
  }
  for (size_t n = 0; n < themes_.size(); ++n) {
    const int y = RowTop + int(n) * RowHeight - scroll;
    if (y > 660)
      break;
    if (y + RowHeight < 0)
      continue;
    const auto &row = themes_[n];
    const bool focused = !sidebar_ && row_ == int(n) + 1;
    // Icon and title share the row header's center line with "R 모두 보기".
    constexpr float HeaderMid = 17;
    paint_.icon(themeIcon(row.theme), 260, y + HeaderMid - 13, Ink, 26);
    paint_.fonts.middle(row.theme.title, 296, y + HeaderMid, 26, Ink, true);
    if (focused)
      paint_.keyChip("R", tr("모두 보기"), 1252, y + HeaderMid);
    hit(1100, y, 160, 34, [this, n] { openTheme(n); });
    const int count = int(row.items.size());
    // Sideways the shelf follows the cursor, or the finger while it glides;
    // its last card (모두 보기) counts as one more.
    const float left = glide("shelf" + std::to_string(n), 232, y + 40, 1048, RowHeight - 40,
                             focused ? float(std::max(0, selected_ - (Columns - 1))) : 0.f,
                             float(count + 1 - Columns), 170, true, true);
    const int first = int(left);
    clipTo(232, y + 40, 1048, RowHeight - 40);
    for (int col = 0; col <= Columns && first + col <= count; ++col) {
      const float cx = 260 + 170 * (first + col - left), cy = y + 46;
      if (first + col < count) {
        card(groups_[row.items[first + col]], cx, cy, focused && selected_ == first + col, false);
        continue;
      }
      // The shelf's last card: the whole list, which scrolls on and on.
      hit(cx, cy, 150, 220, [this, n] { openTheme(n); });
      if (focused && selected_ == count) paint_.ring(cx, cy, 150, 150);
      paint_.rect(cx, cy, 150, 150, Surface, 14);
      paint_.icon(9, cx + 75 - 18, cy + 46, Ink, 36);
      const std::string all = tr("모두 보기");
      paint_.fonts.middle(all, cx + (150 - paint_.fonts.measure(all, 18, true)) / 2, cy + 112, 18, Ink, true);
    }
    unclip();
  }
  // Whole page: the hero plus one entry per row.
  if (!themes_.empty())
    paint_.scrollbar(1266, 24, 620, float(std::max(0, row_ - 1)), 1.8f,
                     float(themes_.size()));
}
} // namespace foil::ui
