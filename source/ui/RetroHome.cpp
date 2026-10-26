#include "ui/Application.hpp"
#include <algorithm>
#include <map>
namespace foil::ui {
namespace {
// A shelf: the system line, the covers and one line of name (no second line
// as on 홈), then room before the next system.
constexpr float ShelfHeight = 274, BannerHeight = 64, CardStep = 170, Card = 150;
constexpr int Columns = 6;
// A picked row's icon, from the same set as 홈's rows, by the kind its id
// names first (arcade:decade:1990 is an arcade row). A kind the server adds
// later gets the genre rows' package until it has its own.
int pickedIcon(const std::string &id) {
  static const std::map<std::string, int> icons{
      {"recent", 29},                                         // sparkle, as 홈's new releases
      {"arcade", 35},                                         // joystick, as 레트로 게임's menu
      {"coop", 13},        {"versus", 13}, {"players", 13},   // users, as 홈's party row
      {"family", 13},
      {"decade", 11},                                         // calendar
      {"classic", 31},     {"good", 31},   {"hidden", 31},    // star, as 홈's rated rows
      {"popular", 28},     {"achievements", 28},              // trophy, as 홈's popular row
      {"maker", 12},                                          // building
      {"series", 42},                                         // layers
      {"theme", 15},                                          // tag
      {"korean", 17},                                         // globe, as 홈's Korean row
      {"light", 30},                                          // clock
  };
  const auto found = icons.find(id.substr(0, id.find(':')));
  return found == icons.end() ? 22 : found->second; // package, as 홈's genre rows
}
} // namespace
// The retro header, the same on the home and on 업적: the title with every
// system's games, and L R 시스템별, Y RetroArch and X 업적 on the right.
void Application::retroHeader() {
  // Every system's games together, beside the title.
  int roms = 0;
  for (const auto &shelf : retroShelves_) roms += shelf.system.romCount;
  if (retroView_ == RetroView::Achievements) {
    // On 업적 the title says where it is and the way back: ‹ 레트로 게임
    // goes to the shelves, as B does.
    constexpr float Mid = 48;
    paint_.icon(7, 258, Mid - 11, Secondary, 22);
    const float end = paint_.fonts.middle(tr("Retro games"), 284, Mid, 22, Secondary, true, 220);
    hit(252, Mid - 22, end - 252 + 8, 44, [this] { leaveRaPage(); }, {252, Mid - 22, end - 252 + 8, 44}, 12);
    paint_.fonts.middle(tr("Achievements"), end + 20, Mid, 30, Ink, true, 300);
  } else
    pageTitle(tr("Retro games"), std::to_string(roms), 0);
  // X 업적 at the right end (where search sits elsewhere), Y RetroArch left
  // of it. Both are there from the first frame, one width whatever they
  // say, so the page loading or a label changing moves nothing.
  const float raX = raHeaderButton(1252);
  // Y gets RetroArch, or checks it for new files once it is there.
  const std::string get = retroBusy_ == "pack"  ? tr("Cancel install")
                          : retroArchPresent() ? tr("Update RetroArch")
                                               : tr("Install RetroArch");
  float getW = 0;
  for (const char *label : {"Cancel install", "Update RetroArch", "Install RetroArch"})
    getW = std::max(getW, paint_.keyLabelWidth("Y", tr(label), 19, true) + 40);
  const float getX = raX - 16 - getW;
  headerItem("retroarch", getX, 24, getW, 48, [this] { action(Action::Queue); });
  paint_.rect(getX, 24, getW, 48, Surface, 12);
  paint_.keyLabel("Y", get, getX + 20, 48, 19, Ink, true);
  // L and R open the focused shelf's system, where they then step through
  // the systems; said here, since nothing else shows it.
  if (!retroShelves_.empty()) {
    const auto systems = tr("By system");
    const float sysW = paint_.keyWidth("L") + paint_.keyWidth("R") + paint_.fonts.measure(systems, 19, true) + 56,
                sysX = getX - 16 - sysW;
    headerItem("systems", sysX, 24, sysW, 48, [this] { openRetroList(retroSystemAt(retroRow_)); });
    paint_.rect(sysX, 24, sysW, 48, Surface, 12);
    paint_.key("L", sysX + 12, 48);
    const float textX = sysX + 12 + paint_.keyWidth("L") + 16;
    paint_.fonts.middle(systems, textX, 48, 19, Ink, true);
    paint_.key("R", sysX + sysW - 12 - paint_.keyWidth("R"), 48);
  }
}
// The retro home: RetroArch first while it is missing, then one shelf per
// system with its best rated games and a 모두 보기 card at the end.
void Application::retroHome() {
  readRaAccount(); // once a run, and again after 새로고침
  retroHeader();
  const int rows = int(retroShelves_.size());
  retroRow_ = std::clamp(retroRow_, 0, std::max(0, rows - 1));
  if (retroRow_ >= 0 && retroRow_ < rows)
    retroCol_ = std::clamp(retroCol_, 0, int(retroShelves_[retroRow_].games.size()) - !retroShelves_[retroRow_].theme.empty());
  if (retroShelves_.empty()) {
    retroBanner(90); // RetroArch's state, always on the home
    const float top = 178;
    if (!connected_)
      offlineNotice(tr("Retro games load once the server is connected."));
    else if (retroMissing_)
      notice(tr("This server has no retro games."), tr("They appear here once the server has a retro folder."), 35);
    else if (retroError_ && retroRetryAt_)
      notice(tr("The server is reading its retro files."), tr("It loads again by itself shortly."), 35);
    else if (retroError_)
      notice(tr("Could not load the retro games."), retroError_.message, 25);
    else if (retroListWorker_.joinable() || !retroLoaded_) {
      paint_.skeleton(260, top, 240, 28, 8);
      paint_.skeletonCards(260, top + 46, 6);
    } else
      notice(tr("No retro games yet."), tr("ROMs put in the server's retro folder appear here."), 35);
    return;
  }
  // The focused shelf rides at the top once past the first; the banner
  // scrolls away with the rest.
  const float base = 90 + BannerHeight + 24;
  const float scroll = glide("retroHome", 232, 84, 1048, 576,
                             inContent() && retroRow_ >= 1 ? base - 90 + retroRow_ * ShelfHeight : 0,
                             base + rows * ShelfHeight + 16 - 660, 1);
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
    const bool theme = !shelf.theme.empty();
    constexpr float HeaderMid = 17;
    float x = 260;
    if (theme) {
      // A picked row: its icon and name as on 홈's rows; no count, no 모두
      // 보기 (the row is what the server picked, not a list to go through).
      const int named = Painter::iconNamed(shelf.icon);
      paint_.icon(named >= 0 ? named : pickedIcon(shelf.themeId), x, y + HeaderMid - 13, Ink, 26);
      paint_.fonts.middle(shelf.theme, x + 36, y + HeaderMid, 26, Ink, true, 900);
    }
    // Header: the system's icon on a white tile, its name, its game count.
    else if (!shelf.system.iconUrl.empty()) {
      paint_.rect(x, y + HeaderMid - 17, 34, 34, {255, 255, 255, 255}, 9);
      images_->draw(shelf.system.iconUrl, x + 4, y + HeaderMid - 13, 26, 26, 4);
      x += 44;
    }
    if (!theme) {
      x = paint_.fonts.middle(retro::systemName(shelf.system, languageCode()), x, y + HeaderMid, 26, Ink, true, 600);
      paint_.pill(std::to_string(shelf.system.romCount), x + 12, y + HeaderMid - 13);
    }
    if (!theme) paint_.keyChip("R", tr("See all"), 1252, y + HeaderMid);
    if (!theme) {
      // A finger gets the chip's size and a little more; pressed, the chip darkens.
      const float chipW = paint_.keyChipWidth("R", tr("See all"));
      hit(1252 - chipW - 6, y, chipW + 12, 34, [this, n] { openRetroList(retroSystemAt(n)); }, {1252 - chipW, y + HeaderMid - 14, chipW, 28}, 14);
    }
    const int count = int(shelf.games.size()), cards = theme ? count : count + 1;
    // Sideways like 홈's shelves: the cursor's place, or the finger's while it
    // glides, resting on whole cards; the last card (모두 보기) counts as one.
    const float left = glide("retroShelf" + std::to_string(n), 232, y + 40, 1048, ShelfHeight - 40,
                             focused ? float(std::max(0, retroCol_ - (Columns - 1))) : 0.f,
                             float(cards - Columns), CardStep, true, true);
    const int first = int(left);
    // Near the end, by cursor or by finger: the next of the system's games.
    if (first + Columns + 4 >= count) loadShelfMore(n);
    // From under the header, so the focus ring (8 px out from a card at y + 46)
    // shows whole at the top.
    clipTo(232, y + 36, 1048, ShelfHeight - 36);
    for (int col = 0; col <= Columns && first + col < cards; ++col) {
      const int i = first + col;
      const float cx = 260 + CardStep * (i - left), cy = y + 46;
      const bool on = focused && retroCol_ == i;
      hit(cx, cy, Card, Card + 30, [this, n, i, on] {
        sidebar_ = false;
        retroRow_ = n;
        retroCol_ = i;
        if (on) action(Action::Accept);
      });
      if (i == count) {
        // The shelf's last card: the system's whole list, with how many it holds.
        if (on) paint_.ring(cx, cy, Card, Card);
        paint_.rect(cx, cy, Card, Card, Surface, 14);
        paint_.icon(9, cx + Card / 2 - 18, cy + 34, Ink, 36);
        const std::string all = tr("See all");
        paint_.fonts.middle(all, cx + (Card - paint_.fonts.measure(all, 18, true)) / 2, cy + 100, 18, Ink, true);
        const auto many = trf("{0} games", {std::to_string(shelf.system.romCount)});
        paint_.fonts.middle(many, cx + (Card - paint_.fonts.measure(many, 15)) / 2, cy + 126, 15, Secondary);
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
      // Bottom left: how many versions it has, when more than one.
      versionsBadge(game.variants, cx + 6, cy + Card - 25, true);
      // Bottom right: its region's flag, left of the check once it is on the card.
      const bool installed = retroInstalled(game) && !retroBezelMissing(game);
      paint_.region(game.region, cx + Card - (installed ? 64 : 34), cy + Card - 26, 27, 18);
      // On the card already: a check on the bottom right.
      if (installed) {
        paint_.rect(cx + Card - 30, cy + Card - 30, 24, 24, Good, 12);
        paint_.icon(8, cx + Card - 27, cy + Card - 27, Surface, 18);
      }
      if (retroBusy_ == game.id) {
        const float ratio = retroBytes_ ? std::min(1.f, float(retroReceived_.load()) / float(retroBytes_)) : 0.f;
        paint_.rect(cx + 10, cy + Card - 18, Card - 20, 8, {22, 22, 26, 190}, 4);
        paint_.rect(cx + 10, cy + Card - 18, std::max(8.f, (Card - 20) * ratio), 8, Butter, 4);
      }
      // One line under the cover, as on 홈's shelves; the focused name runs.
      if (on) marquee(game.name, cx, cy + 158, 18, Card, false);
      else paint_.fonts.text(game.name, cx, cy + 158, 18, Ink, false, Card);
      // Under the name what the server knows of it, so no card stands bare:
      // 업적, 한글, its year, how many play at once. One that would spill
      // past the card is skipped and the next tried.
      float at = cx;
      if (game.hasLabels || !retroLabelBook_.empty()) { // the server's words
        for (const auto &ref : game.labels)
          if (const float w = labelChip(ref, retroLabelBook_, at, cy + 186, cx + Card, 22, 13)) at += w + 4;
        continue;
      }
      if (const float w = achievementLabel(game.achievements, at, cy + 186, 22, 13)) at += w + 4;
      std::vector<std::string> facts;
      if (game.edition.korean) facts.push_back(tr("Korean|script"));
      if (game.year > 0) facts.push_back(std::to_string(game.year));
      if (game.players > 1) facts.push_back(trf("{0}P|players", {std::to_string(game.players)}));
      for (const auto &fact : facts) {
        const float w = paint_.fonts.measure(fact, 13, true) + 16;
        if (at + w > cx + Card) continue;
        paint_.rect(at, cy + 186, w, 22, Line, 7);
        paint_.fonts.middle(fact, at + 8, cy + 197, 13, Ink, true);
        at += w + 4;
      }
    }
    unclip();
  }
  SDL_RenderSetClipRect(paint_.renderer, SDL_RectEmpty(&page) ? nullptr : &page);
  paint_.scrollbar(1266, 90, 560, float(std::max(0, retroRow_)), 1.6f, float(rows));
}
void Application::retroHomeAction(Action a) {
  const int rows = int(retroShelves_.size());
  const auto games = [&] { return retroRow_ >= 0 && retroRow_ < rows ? int(retroShelves_[retroRow_].games.size()) : 0; };
  // A theme row ends at its last game; a system's has 모두 보기 after it.
  const auto last = [&] {
    const bool theme = retroRow_ >= 0 && retroRow_ < rows && !retroShelves_[retroRow_].theme.empty();
    return theme ? std::max(0, games() - 1) : games();
  };
  switch (a) {
  case Action::Up:
    retroRow_ = std::max(0, retroRow_ - 1);
    retroCol_ = std::min(retroCol_, last());
    break;
  case Action::Down:
    retroRow_ = std::min(rows - 1, retroRow_ + 1);
    retroCol_ = std::min(retroCol_, last());
    break;
  case Action::Left:
    if (retroCol_ > 0) retroCol_--;
    else sidebar_ = true;
    break;
  case Action::Right:
    retroCol_ = std::min(last(), retroCol_ + 1);
    break;
  case Action::Previous:
  case Action::Next:
    openRetroList(retroSystemAt(retroRow_));
    break;
  case Action::Accept:
    if (retroCol_ >= games()) openRetroList(retroSystemAt(retroRow_));
    else if (const auto *game = retroFocused()) openRetroDetail(*game);
    break;
  default:
    break;
  }
}
float Application::versionsBadge(int count, float x, float y, bool dark) {
  if (count < 2) return 0;
  const auto label = std::to_string(count);
  const int text = dark ? 13 : 15, mark = dark ? 13 : 15;
  const float pad = dark ? 6 : 9, h = dark ? 19 : 26;
  const float w = pad + mark + 4 + paint_.fonts.measure(label, text, true) + pad;
  paint_.rect(x, y, w, h, dark ? SDL_Color{22, 22, 26, 190} : Line, dark ? 5 : 7);
  paint_.icon(42, x + pad, y + (h - mark) / 2, dark ? Surface : Ink, mark);
  paint_.fonts.middle(label, x + pad + mark + 4, y + h / 2, text, dark ? Surface : Ink, true);
  return w;
}
} // namespace foil::ui
