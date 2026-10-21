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
std::vector<Application::HeroTag> Application::heroTags(const Group &g) const {
  const auto &item = g.base ? *g.base : g.representative;
  std::vector<HeroTag> tags;
  if (item.score > 0) {
    // A score with no label still says what it is: "평점 81", never a bare 81.
    const auto label = genreLabel(scoreLabels_, item.scoreLabel);
    tags.push_back({label.empty() ? trf("Rated {0}", {std::to_string(item.score)})
                                  : std::to_string(item.score) + " · " + label,
                    item.score});
  }
  // The price in the sale's place: the sale when there is one, else the
  // regular price (both from the server's eShop prices).
  if (!item.priceDiscount.empty()) tags.push_back({trf("eShop sale {0}", {priceText(item.priceDiscount)})});
  else if (!item.priceRegular.empty()) tags.push_back({"eShop " + priceText(item.priceRegular)});
  const auto &languages = g.representative.languages;
  if (std::find(languages.begin(), languages.end(), "ko") != languages.end()) tags.push_back({tr("Korean")});
  if (item.size > 0) tags.push_back({formatBytes(item.size)});
  int genres = 0;
  for (const auto &key : item.categories) {
    if (genres == 2) break;
    const auto name = genreLabel(genreNames_, key);
    if (name.empty()) continue;
    tags.push_back({name});
    ++genres;
  }
  if (item.players >= 2) tags.push_back({trf("Up to {0}", {std::to_string(item.players)})});
  if (item.releaseDate > 0) tags.push_back({trf("Released {0}|tag", {dateText(item.releaseDate)})});
  if (g.update) tags.push_back({tr("Update included")});
  if (!g.dlc.empty()) tags.push_back({"DLC " + std::to_string(g.dlc.size())});
  return tags;
}
void Application::warmHome() {
  if (!images_ || groups_.empty() || hero_ >= groups_.size()) return;
  const auto &icon = groups_[hero_].representative.icon;
  if (!heroBanner_.empty() && heroFeatured_) images_->request(thumbnailUrl(heroBanner_, "xl"));
  images_->request(thumbnailUrl(icon));
  images_->request(thumbnailUrl(icon, "md"));
  for (size_t n = 0; n < themes_.size() && n < 2; ++n)
    for (size_t col = 0; col < themes_[n].items.size() && col < 6; ++col)
      if (themes_[n].items[col] < groups_.size())
        images_->request(thumbnailUrl(groups_[themes_[n].items[col]].representative.icon));
}
void Application::home() {
  if (groups_.empty()) {
    // A write still holds the records (a slow card): the home's shape, as
    // while it connects; past a few seconds, what may be holding them.
    if (stateWaiting_ && SDL_GetTicks() - stateWaitAt_ > 8000) {
      notice(tr("Reading the app's records on the SD card."), tr("If another CookingFoil is open, close it."), 25);
      return;
    }
    const auto shape = [this] {
      paint_.skeleton(260, 24, 992, 248, 20);
      paint_.skeleton(260, 300, 160, 28, 8);
      paint_.skeletonCards(260, 346, 6);
    };
    if (stateWaiting_) {
      shape();
      return;
    }
    if (!session_) {
      const bool none = std::none_of(config_.servers.begin(), config_.servers.end(),
                                     [](const auto &s) { return s.enabled; });
      notice(config_.servers.empty() ? tr("No servers added.")
             : none                  ? tr("No servers are on.")
                                     : tr("Choose a server in Settings."),
             tr("Enter a server's address and account with \"+ Add server\" in Settings."), 21,
             tr("Go to Settings"), [this] { screen(Screen::Settings); }, tr("Refresh"), "-", [this] { refreshAll(); });
      return;
    }
    if (error_) {
      // A dropped line or a server that did not answer is tried again here;
      // nothing points to 설정 for what is likely a moment's outage.
      const bool offline = error_.message == net::offlineMessage();
      if (pairingPending_)
        notice(tr("Waiting for device approval."), error_.message, 25);
      else
        notice(offline ? tr("No internet connection.") : tr("Couldn't connect to the server."),
               offline ? error_.message : error_.message + "\n" + tr("Check the server in Settings and press - to refresh."),
               offline ? 20 : 25, tr("Try again"), [this] { refreshAll(); }, "", "", {}, 46);
      return;
    }
    if (connected_) {
      notice(tr("No games to show."), tr("Games appear here once the server has some."), 22);
      return;
    }
    // Connecting: the shape of the home screen, pulsing.
    shape();
    return;
  }
  constexpr int RowTop = 300, RowHeight = 268, Columns = 6;
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
  paint_.fonts.text(heroes_.empty() ? tr("Featured") : heroes_[heroSlide_].label, 512 + hx,
                    61 - scroll, 18, Butter, true);
  paint_.fonts.text(g.representative.name, 512 + hx, 98 - scroll, 40, Surface, true,
                    700);
  paint_.fonts.text(g.representative.publisher, 512 + hx, 155 - scroll, 19,
                    {228, 228, 234, 255}, false, 680);
  // The tags in the website's order; what does not fit goes whole, from the
  // end (no tag is cut). The score's tag takes its card colour.
  // Each tag whole on one line, never wrapped or cut: as many as fit show,
  // the rest go from the end.
  float x = 512 + hx;
  const float right = 1252 - 32 + hx;
  for (const auto &tag : heroTags(g)) {
    const bool bold = tag.score > 0;
    const float w = paint_.fonts.measure(tag.text, 17, bold) + 28; // as pill(dark)
    if (w > right - x) break;
    paint_.rect(x, 199 - scroll, w, 35, bold ? scoreColor(tag.score) : SDL_Color{255, 255, 255, 41}, 17);
    paint_.fonts.middle(tag.text, x + 14, 199 - scroll + 17.5f, 17, Surface, bold);
    x += w + 10;
  }
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
    // Always shown: a finger may open any row's list, not only the focused one's.
    paint_.keyChip("R", tr("See all"), 1252, y + HeaderMid);
    {
      // A finger gets the chip's size and a little more; pressed, the chip darkens.
      const float chipW = paint_.keyChipWidth("R", tr("See all"));
      hit(1252 - chipW - 6, y, chipW + 12, 34, [this, n] { openTheme(n); }, {1252 - chipW, y + HeaderMid - 14, chipW, 28}, 14);
    }
    const int count = int(row.items.size());
    // Sideways the shelf follows the cursor, or the finger while it glides;
    // its last card (모두 보기) counts as one more.
    const float left = glide("shelf" + std::to_string(n), 232, y + 40, 1048, RowHeight - 40,
                             focused ? float(std::max(0, selected_ - (Columns - 1))) : 0.f,
                             float(count + 1 - Columns), 170, true, true);
    const int first = int(left);
    // From under the header, so the focus ring (8 px out from a card at y + 46)
    // shows whole at the top.
    clipTo(232, y + 36, 1048, RowHeight - 36);
    for (int col = 0; col <= Columns && first + col <= count; ++col) {
      const float cx = 260 + 170 * (first + col - left), cy = y + 46;
      if (first + col < count) {
        card(groups_[row.items[first + col]], cx, cy, focused && selected_ == first + col, true);
        continue;
      }
      // The shelf's last card: the whole list, which scrolls on and on.
      hit(cx, cy, 150, 220, [this, n] { openTheme(n); });
      if (focused && selected_ == count) paint_.ring(cx, cy, 150, 150);
      paint_.rect(cx, cy, 150, 150, Surface, 14);
      paint_.icon(9, cx + 75 - 18, cy + 46, Ink, 36);
      const std::string all = tr("See all");
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
