#include "ui/Application.hpp"
#include "core/I18n.hpp"
#include "net/RetroApi.hpp"
#include <algorithm>
#include <cmath>
namespace foil::ui {
namespace {
std::string regionName(const std::string &code) {
  if (code == "kr") return tr("Korea|region");
  if (code == "jp") return tr("Japan|region");
  if (code == "us") return tr("North America|region");
  if (code == "eu") return tr("Europe|region");
  if (code == "asia") return tr("Asia|region");
  if (code == "br") return tr("Brazil|region");
  if (code == "world") return tr("World|region");
  return code;
}
// The cursor on the button, however many versions come.
constexpr int ButtonPart = 1 << 20;
std::string countText(int count) {
  auto text = std::to_string(count);
  for (int at = int(text.size()) - 3; at > 0; at -= 3) text.insert(size_t(at), ",");
  return text;
}
} // namespace
// A on a retro game, wherever it shows (the shelves, a system's list, a row
// in 다운로드): its page, as a shop game has one.
void Application::openRetroDetail(const retro::Game &game) {
  if (screen_ != Screen::RetroDetail) previous_ = screen_;
  notice_.clear();
  headerFocus_.clear();
  unglide();
  screen_ = Screen::RetroDetail;
  retroPick_ = game;
  retroVariants_.clear();
  retroVariant_ = 0;
  retroMeta_ = {};
  retroChecked_ = {game.id}; // the one opened, until its versions say which is recommended
  retroLoaded_games_.clear();
  retroLoaded_games_[game.id] = game;
  detail_ = {};
  descriptionScroll_ = 0;
  descriptionFocus_ = false;
  galleryOpen_ = raListOpen_ = false;
  screenshot_ = 0;
  retroPart_ = ButtonPart; // on the button, as a shop game's page opens
  retroDetailLoading_ = true;
  requestRetroDetail(game.id);
}
// 2 while it downloads (percent filled in), 1 while it waits its turn, else 0.
int Application::retroOnTheWay(const retro::Game &game, double &percent) const {
  percent = 0;
  if (!game.id.empty() && retroBusy_ == game.id) {
    if (retroBytes_) percent = std::min(100.0, 100.0 * double(retroReceived_.load()) / double(retroBytes_));
    return 2;
  }
  const bool waiting = std::any_of(installQueue_.begin(), installQueue_.end(), [&](const QueuedInstall &q) {
    return q.kind == "retro" && !game.bundleUrl.empty() && q.file.url == game.bundleUrl;
  });
  return waiting ? 1 : 0;
}
// The versions on the right: the server's list, or the game itself when it
// has only the one.
std::vector<retro::Variant> Application::retroVersionList() const {
  if (!retroVariants_.empty()) return retroVariants_;
  retro::Variant only;
  only.id = retroPick_.id;
  only.name = retroPick_.name;
  only.variant = retroPick_.variant;
  only.region = retroPick_.region;
  only.edition = retroPick_.edition;
  only.size = retroPick_.size;
  return {only};
}
int Application::retroPageOnTheWay(double &percent, int &queueAt) const {
  percent = 0;
  queueAt = -1;
  const auto mine = [&](const std::string &id) { return !id.empty() && (id == retroPick_.id || retroChecked_.count(id)); };
  if (mine(retroBusy_)) {
    if (retroBytes_) percent = std::min(100.0, 100.0 * double(retroReceived_.load()) / double(retroBytes_));
    return 2;
  }
  for (size_t i = 0; i < installQueue_.size(); ++i) {
    const auto &q = installQueue_[i];
    if (q.kind != "retro") continue;
    if ((!retroPick_.bundleUrl.empty() && q.file.url == retroPick_.bundleUrl) || mine(retro::parseGameJson(q.payload).id)) {
      queueAt = int(i);
      return 1;
    }
  }
  return 0;
}
void Application::getRetroPick() {
  double percent = 0;
  int queueAt = -1;
  const int onTheWay = retroPageOnTheWay(percent, queueAt);
  if (onTheWay) {
    // Already on its way: whether to cancel it, as 다운로드 asks.
    dialogFocus_ = 0;
    if (onTheWay == 2) {
      cancelRetro_ = true;
      confirmCancel_ = true;
      confirmExit_ = false;
    } else {
      unqueueAsk_ = queueAt;
      unqueuePick_ = false;
    }
    return;
  }
  getRetroChecked();
}
// A on a version row: in or out of what the button gets; one that would
// not run never goes in.
void Application::toggleRetroVersion(int row) {
  const auto versions = retroVersionList();
  if (row < 0 || row >= int(versions.size())) return;
  const auto &v = versions[size_t(row)];
  if (v.missing) {
    message(tr("This version cannot run: a file it needs is not on the server."));
    return;
  }
  if (!retroChecked_.erase(v.id)) retroChecked_.insert(v.id);
}
uint64_t Application::retroCheckedBytes() const {
  uint64_t bytes = 0;
  for (const auto &v : retroVersionList())
    if (retroChecked_.count(v.id)) bytes += v.size;
  return bytes;
}
// Every checked version into the queue; those not loaded in full yet are
// fetched first, off the drawing thread.
void Application::getRetroChecked() {
  if (retroChecked_.empty()) {
    message(tr("Pick at least one version."));
    return;
  }
  std::vector<std::string> missing;
  for (const auto &id : retroChecked_)
    if (!retroLoaded_games_.count(id)) missing.push_back(id);
  const auto queueAll = [this] {
    int queued = 0;
    for (const auto &id : retroChecked_)
      if (const auto found = retroLoaded_games_.find(id); found != retroLoaded_games_.end())
        queued += getRetroGame(found->second, true);
    if (queued > 1) message(trf("Added {0} versions to the download queue.", {std::to_string(queued)}));
  };
  if (missing.empty() || !session_) {
    queueAll();
    return;
  }
  const auto server = session_->server();
  const auto state = state_;
  if (!runRetroJob("list", [this, server, state, missing, queueAll]() -> std::function<void()> {
        std::vector<retro::Game> games;
        for (const auto &id : missing) {
          net::RetroReply reply;
          auto detail = net::retroDetail(server, state, id, reply, &stop_).value;
          if (!reply.error && !detail.game.id.empty()) games.push_back(std::move(detail.game));
        }
        return [this, games = std::move(games), queueAll]() mutable {
          for (auto &g : games) retroLoaded_games_[g.id] = std::move(g);
          queueAll();
        };
      }))
    message(tr("Try again in a moment."));
}
void Application::retroDetailAction(Action a) {
  if (raListOpen_) {
    raListAction(a);
    return;
  }
  // X: the game's achievements, when RetroAchievements has it.
  if (a == Action::Search) {
    openRaList();
    return;
  }
  const int rows = int(retroVersionList().size());
  retroPart_ = std::min(retroPart_, rows); // the button stays the button as versions arrive
  if (a == Action::Back) {
    screen_ = previous_ == Screen::RetroDetail ? Screen::Retro : previous_;
    unglide();
    return;
  }
  if (a == Action::Preview && !detail_.screenshots.empty()) {
    galleryOpen_ = true;
    screenshot_ = 0;
    return;
  }
  // On the description: up and down scroll it; right or A go back.
  if (descriptionFocus_) {
    const int last = (descriptionHeight_ + ScrollStep - 1) / ScrollStep;
    if (a == Action::Up) descriptionScroll_ = std::max(0, descriptionScroll_ - 1);
    if (a == Action::Down) descriptionScroll_ = std::min(last, descriptionScroll_ + 1);
    if (a == Action::Right || a == Action::Accept) descriptionFocus_ = false;
    return;
  }
  if (a == Action::Left) {
    descriptionFocus_ = true;
    return;
  }
  // Rows, then the button; the cursor on a row picks that version.
  if (a == Action::Up || a == Action::Down) {
    const int next = std::clamp(retroPart_ + (a == Action::Down ? 1 : -1), 0, rows);
    if (next == retroPart_) return;
    retroPart_ = next;
    if (next < rows && !retroVariants_.empty()) moveRetroVariant(next - retroVariant_);
    return;
  }
  if (a == Action::Accept) {
    if (retroPart_ < rows) toggleRetroVersion(retroPart_);
    else getRetroPick();
  }
}
// The note before the description: the core this game needs comes along.
int Application::retroCoreCard(int y) {
  if (!retroCoreMissing(retroPick_)) return y;
  constexpr float X = 40, W = 750, Height = 52;
  paint_.rect(X, y, W, Height, SDL_Color{255, 241, 204, 255}, 14);
  paint_.icon(33, X + 18, y + (Height - 24) / 2, Pending, 24);
  paint_.fonts.middle(trf("The {0} core is missing, so it comes down right after the game.", {retroPick_.core}), X + 54,
                      y + Height / 2, 18, Ink, true, W - 72);
  return int(y + Height + 16);
}
// Facts as a labeled grid, as a shop game's: what the server knows of the
// game, then where it goes on the card; genres as chips.
float Application::retroInfoCard(float y) {
  const auto &game = retroPick_;
  const auto &meta = retroMeta_;
  struct Fact {
    int icon;
    std::string label, value;
  };
  std::vector<Fact> facts;
  if (game.rating > 0)
    facts.push_back({31, game.ratingCount ? trf("Score · {0} votes", {countText(game.ratingCount)}) : std::string(tr("Score")),
                     std::to_string(game.rating)});
  if (meta.releaseDate) facts.push_back({11, tr("Released"), retro::releaseLabel(meta.releaseDate)});
  if (!meta.publisher.empty()) facts.push_back({12, tr("Publisher"), meta.publisher});
  if (!meta.developer.empty() && meta.developer != meta.publisher) facts.push_back({12, tr("Developer"), meta.developer});
  if (meta.players)
    facts.push_back({13, tr("Players"), meta.players == 1 ? std::string(tr("1 player")) : trf("Up to {0}", {std::to_string(meta.players)})});
  const auto system = std::find_if(retroSystems_.begin(), retroSystems_.end(), [&](const auto &s) { return s.id == game.system; });
  if (system != retroSystems_.end()) facts.push_back({27, tr("System|retro"), retro::systemName(*system, languageCode())});
  if (!game.core.empty())
    facts.push_back({22, tr("Core|retro"), system != retroSystems_.end() && !system->coreName.empty() && game.core == system->defaultCore
                                               ? system->coreName : game.core});
  facts.push_back({14, tr("Size|info"), formatBytes(game.size)});
  if (const auto dot = game.file.rfind('.'); dot != std::string::npos) {
    auto format = game.file.substr(dot + 1);
    std::transform(format.begin(), format.end(), format.begin(), ::toupper);
    if (format.size() <= 4) facts.push_back({16, tr("File type"), format});
  }
  if (!game.region.empty()) facts.push_back({17, tr("Region"), regionName(game.region)});
  facts.push_back({41, tr("Bezel"), game.bezel == retro::Game::Bezel::Game     ? std::string(tr("Game bezel"))
                                    : game.bezel == retro::Game::Bezel::System ? std::string(tr("Default bezel"))
                                                                               : std::string(tr("None|bezel"))});
  if (!game.forwarderUrl.empty()) facts.push_back({24, tr("HOME Menu shortcut"), tr("With the game|shortcut")});
  constexpr float Left = 40, Width = 750, Cell = Width / 3, RowHeight = 66;
  const int rows = int((facts.size() + 2) / 3);
  // Where it goes on the card, under the grid in a line of its own.
  const float pathRow = game.romPath.empty() ? 0 : 58;
  paint_.rect(Left, y, Width, rows * RowHeight + 24 + pathRow, Surface, 16);
  for (size_t n = 0; n < facts.size(); ++n) {
    const float x = Left + 20 + (n % 3) * Cell, top = y + 16 + (n / 3) * RowHeight;
    paint_.icon(facts[n].icon, x, top + 1, Secondary, 16);
    paint_.fonts.middle(facts[n].label, x + 22, top + 9, 15, Secondary);
    paint_.fonts.text(facts[n].value, x, top + 24, 18, Ink, true, Cell - 30);
  }
  if (pathRow > 0) {
    const float top = y + 16 + rows * RowHeight;
    paint_.rect(Left + 20, top - 6, Width - 40, 1, Line);
    paint_.icon(26, Left + 20, top + 9, Secondary, 16);
    paint_.fonts.middle(tr("Install location"), Left + 42, top + 17, 15, Secondary);
    const float labelW = paint_.fonts.measure(tr("Install location"), 15) + 18;
    paint_.fonts.middle(game.romPath, Left + 42 + labelW, top + 17, 16, Ink, false, Width - 82 - labelW);
  }
  y += rows * RowHeight + 24 + pathRow + 22;
  if (!meta.genres.empty()) {
    paint_.fonts.text(tr("Genres"), Left, y, 18, Ink, true);
    float x = Left + 110;
    for (const auto &label : meta.genres) {
      const float w = paint_.fonts.measure(label, 16, true) + 24;
      if (x + w > Left + Width) {
        x = Left + 110;
        y += 38;
      }
      paint_.rect(x, y - 3, w, 30, Surface, 15);
      paint_.fonts.middle(label, x + 12, y + 12, 16, Ink, true);
      x += w + 8;
    }
    y += 46;
  }
  return y;
}
void Application::retroDetail() {
  const auto &game = retroPick_;
  const auto &meta = retroMeta_;
  paint_.rect(0, 0, 1280, 212, {10, 10, 14, 255});
  // The banner: a screenshot, else the box art, else the tile, as a shop
  // game falls back to its icon.
  const auto banner = !meta.screenshots.empty() ? thumbnailUrl(meta.screenshots[0], "xl")
                      : !meta.boxartUrl.empty() ? meta.boxartUrl
                                                : game.iconUrl;
  if (!banner.empty() && !images_->loaded(banner) && !images_->failed(banner))
    paint_.skeleton(0, 0, 1280, 212, 0, true);
  images_->draw(banner, 0, 0, 1280, 212, 0, true);
  const auto back = screenLabel(previous_);
  paint_.icon(7, 32, 14, Surface, 20);
  paint_.fonts.middle(back, 56, 24, 17, Surface);
  hit(24, 4, 200, 40, [this] { action(Action::Back); });
  constexpr int Top = 58;
  cover(game.iconUrl, 40, Top, 140, false, game.name, true, true);
  paint_.region(game.region, 40 + 140 - 44, Top + 140 - 32, 36, 24);
  paint_.fonts.text(game.name, 208, Top + 4, 38, Surface, true, 1010);
  // Under the name: its original title, and the system it runs on.
  std::string subtitle = game.original != game.name ? game.original : "";
  const auto system = std::find_if(retroSystems_.begin(), retroSystems_.end(), [&](const auto &s) { return s.id == game.system; });
  if (system != retroSystems_.end())
    subtitle += (subtitle.empty() ? "" : " · ") + retro::systemName(*system, languageCode());
  paint_.fonts.text(subtitle, 208, Top + 62, 19, {228, 228, 234, 255}, false, 960);
  constexpr float PillsRight = 806;
  float x = 208;
  if (const float w = achievementLabel(game.achievements, x, Top + 100, 35, 17)) x += w + 10;
  for (const auto &label : retroLabels(game, true)) {
    if (x + paint_.fonts.measure(label, 17) + 28 > PillsRight) break;
    x += paint_.pill(label, x, Top + 100, true, 17) + 10;
  }
  // The description column, as a shop game's: it scrolls in the band
  // between the header and the footer.
  constexpr int BandTop = 212, BandBottom = 660, Pad = 14;
  SDL_Rect leftClip{0, BandTop, 806, BandBottom - BandTop};
  SDL_RenderSetClipRect(paint_.renderer, &leftClip);
  const int offset = int(std::lround(glide("description", 0, BandTop, 806, BandBottom - BandTop,
                                           float(descriptionScroll_ * ScrollStep), float(descriptionHeight_), 1)));
  int y = BandTop + Pad - offset;
  y = retroCoreCard(y);
  if (!meta.description.empty()) {
    y += paint_.fonts.wrapped(meta.description, 40, y, 20, Ink, 750, 20, false, 1.6f) + 24;
  } else if (retroDetailLoading_) {
    for (int line = 0; line < 5; ++line)
      paint_.skeleton(40, y + line * 34, line % 4 == 3 ? 420 : 720, 18, 6);
    y += 5 * 34 + 20;
  } else {
    constexpr float Height = 92, Badge = 52;
    paint_.rect(40, y, 750, Height, Surface, 16);
    paint_.rect(64, y + (Height - Badge) / 2, Badge, Badge, {255, 241, 204, 255}, int(Badge / 2));
    paint_.icon(29, 64 + (Badge - 26) / 2, y + (Height - 26) / 2, Ink, 26);
    paint_.fonts.middle(tr("No description yet"), 136, y + 34, 20, Ink, true, 620);
    paint_.fonts.middle(tr("Check the details below."), 136, y + 62, 16, Secondary, false, 630);
    y += Height + 16;
  }
  y = int(retroInfoCard(float(y))) + 22;
  y = raCard(y);
  y = screenshotStrip(y);
  descriptionHeight_ = std::max(0, y + offset - (BandBottom - Pad));
  {
    SDL_Rect full{0, 0, 1280, 660};
    SDL_RenderSetClipRect(paint_.renderer, &full);
    constexpr float View = BandBottom - BandTop;
    paint_.scrollbar(798, BandTop + Pad, View - 2 * Pad, float(offset), View, View + float(descriptionHeight_),
                     descriptionFocus_);
  }
  retroDetailPanel();
  if (galleryOpen_) gallery();
}
// The right panel, where a shop game lists what to install: the versions,
// the card it goes to and the button.
void Application::retroDetailPanel() {
  const auto versions = retroVersionList();
  const int rows = int(versions.size());
  const bool panelFocus = !descriptionFocus_;
  paint_.rect(818, 172, 430, 484, Surface, 20);
  const std::string head = tr("Versions");
  paint_.fonts.middle(head, 840, 194, 22, Ink, true);
  paint_.pill(std::to_string(rows), 840 + paint_.fonts.measure(head, 22, true) + 10, 194 - 13);
  paint_.rect(818, 216, 430, 1, Line);
  constexpr float RowTop = 226, RowPitch = 56, RowHeight = 52, W = 386;
  const int cursor = std::min(retroPart_, rows - 1);
  const float top = glide("retroVersions", 840, RowTop - 4, W, 4 * RowPitch, float(std::max(0, cursor - 3)),
                          float(rows - 4), RowPitch);
  const int first = int(top);
  clipTo(840, RowTop - 4, W, 4 * RowPitch);
  for (int i = 0; i < 5 && first + i < rows; ++i) {
    const int n = first + i;
    const auto &v = versions[size_t(n)];
    const float ry = RowTop + (n - top) * RowPitch, mid = ry + RowHeight / 2;
    const bool checked = retroChecked_.count(v.id) > 0;
    hit(840, ry, W, RowHeight, [this, n] {
      descriptionFocus_ = false;
      if (retroPart_ == n) toggleRetroVersion(n);
      retroPart_ = n;
      if (!retroVariants_.empty()) moveRetroVariant(n - retroVariant_);
    });
    if (retroPart_ == n && panelFocus) paint_.focusRow(840, ry, W, RowHeight, Ground, 12);
    // A box for each version, as a game's install list: checked ones come down.
    const SDL_Color edge = checked ? Ink : SDL_Color{138, 138, 147, 255};
    paint_.rect(852, mid - 14, 28, 28, v.missing ? Line : edge, 8);
    paint_.rect(854, mid - 12, 24, 24, checked ? Butter : Surface, 6);
    if (checked) paint_.icon(8, 856, mid - 10, Ink, 20);
    float tx = 888;
    if (!v.region.empty()) {
      paint_.region(v.region, tx, mid - 10, 30, 20);
      tx += 30 + 10;
    }
    const auto size = formatBytes(v.size);
    const float sizeW = paint_.fonts.measure(size, 17, true);
    paint_.fonts.middle(size, 1212 - sizeW, mid, 17, Ink, true);
    // Its tag over what it is: Korean, the server's pick, or that it will not run.
    std::string sub;
    auto add = [&](const std::string &part) { sub += (sub.empty() ? "" : " · ") + part; };
    if (v.edition.korean) add(tr("Korean|script"));
    if (v.representative && rows > 1) add(tr("Recommended"));
    if (v.missing) add(tr("Will not run"));
    const auto label = !v.variant.empty() ? v.variant : v.name;
    const float textW = 1212 - sizeW - 12 - tx;
    // Tags run long and differ at the end: the row under the cursor shows
    // its whole tag as the lists do, sliding.
    const float labelMid = sub.empty() ? mid : mid - 10;
    if (retroPart_ == n && panelFocus && !v.missing) marquee(label, tx, labelMid - 13, 18, textW);
    else paint_.fonts.middle(label, tx, labelMid, 18, v.missing ? Secondary : Ink, true, textW);
    if (!sub.empty()) paint_.fonts.middle(sub, tx, mid + 12, 15, v.missing ? Pending : Secondary, false, textW);
  }
  unclip();
  if (rows > 4) {
    const auto at = std::to_string(std::min(rows, cursor + 1)) + " / " + std::to_string(rows);
    paint_.fonts.middle(at, 1226 - paint_.fonts.measure(at, 15), 194, 15, Secondary);
    paint_.scrollbar(1232, RowTop, 4 * RowPitch - 4, float(first), 4, float(rows));
  }
  paint_.rect(818, 454, 430, 1, Line);
  paint_.fonts.middle(tr("Total"), 852, 476, 18, Secondary);
  const auto total = formatBytes(retroCheckedBytes());
  paint_.fonts.middle(total, 1214 - paint_.fonts.measure(total, 24, true), 476, 24, Ink, true);
  // Retro games always go to the SD card: one card, already chosen.
  constexpr float CardTop = 494, CardHeight = 60, TextLeft = 52;
  paint_.rect(840, CardTop, W, CardHeight, Ink, 12);
  paint_.icon(26, 856, CardTop + (CardHeight - 24) / 2, Butter, 24);
  paint_.fonts.middle(tr("SD card"), 840 + TextLeft, CardTop + 21, 18, Surface, true, W - TextLeft - 12);
  const auto room = sdFree_         ? trf("{0} free", {formatBytes(*sdFree_)})
                    : storageChecked_ ? std::string(tr("Space unknown"))
                                      : std::string(tr("Checking"));
  paint_.fonts.middle(room, 840 + TextLeft, CardTop + 41, 15, {200, 200, 206, 255}, false, W - TextLeft - 12);
  buttonHit({840, 568, W, 60}, 14, [this] {
    descriptionFocus_ = false;
    retroPart_ = int(retroVersionList().size());
    getRetroPick();
  });
  if (retroPart_ >= rows && panelFocus) paint_.ring(840, 568, W, 60, 14);
  double percent = 0;
  int queueAt = -1;
  if (const int onTheWay = retroPageOnTheWay(percent, queueAt)) {
    const bool downloading = onTheWay == 2;
    onTheWayButton(840, W, downloading, percent,
                   downloading && retroBytes_ ? 100.0 * double(retroKept_.load()) / double(retroBytes_) : 0);
  } else {
    paint_.rect(840, 568, W, 60, Butter, 14);
    const auto get = retroGetLabel();
    paint_.keyLabel("A", get, 840 + (W - paint_.keyLabelWidth("A", get, 22, true)) / 2, 598, 22, Ink, true);
  }
}
// What the button does: get it, get it again, or add the bezel it lacks.
std::string Application::retroGetLabel() const {
  if (retroChecked_.size() > 1) return trf("Download {0} versions", {std::to_string(retroChecked_.size())});
  const bool bezel = retroBezelMissing(retroPick_), there = retroInstalled(retroPick_) && !bezel;
  return there ? tr("Get again") : bezel ? tr("Get bezel") : tr("Download");
}
} // namespace foil::ui
