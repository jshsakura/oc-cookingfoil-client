// An app's page in 홈브류, in the shape of a game's: the header, what the
// server says of it on the left, and on the right its rows (patches, saves,
// the HOME Menu shortcut, delete) over the button that gets, updates or gets
// it again. The rows and their popups are the ones the app's card had.
#include "ui/Application.hpp"
#include "core/I18n.hpp"
#include "platform/Shortcut.hpp"
#include "core/HomebrewCard.hpp"
#include "install/Installer.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
#include <cmath>
namespace foil::ui {
namespace {
constexpr int Patches = 1, Saves = 2, Remove = 3, Shortcut = 4;
// "2026-10-14T…" -> "2026.10.14".
std::string dayLabel(const std::string &at) {
  if (at.size() < 10 || at[4] != '-' || at[7] != '-') return at;
  return at.substr(0, 4) + "." + at.substr(5, 2) + "." + at.substr(8, 2);
}
} // namespace
// The version on the card: the ledger's, else the one the card scan read.
std::string Application::homebrewCardVersion(const homebrew::App &app) const {
  if (app.title) {
    const auto *game = homebrewInstalledTitle(app);
    return game ? game->displayVersion : "";
  }
  if (const auto found = homebrewLedger_.find(app.id); found != homebrewLedger_.end() && !found->second.version.empty())
    return found->second.version;
  if (const auto card = homebrewCardOf_.find(app.id); card != homebrewCardOf_.end())
    return homebrewCard_[card->second].version;
  return app.cardOnly ? app.version : "";
}
// 2 while it downloads (percent filled in), 1 while it waits its turn, else 0.
int Application::homebrewOnTheWay(const homebrew::App &app, double &percent) const {
  percent = 0;
  if (app.title && installer_.running()) {
    const auto p = installer_.progress();
    if (std::any_of(p.parts.begin(), p.parts.end(), [&](const Item &f) { return f.url == app.downloadUrl; })) {
      percent = p.percent;
      return 2;
    }
  }
  if (retroBusy_ == "homebrew:" + app.id) {
    if (retroBytes_) percent = std::min(100.0, 100.0 * double(retroReceived_.load()) / double(retroBytes_));
    return 2;
  }
  return homebrewQueuedAt(app) >= 0 ? 1 : 0;
}
int Application::homebrewQueuedAt(const homebrew::App &app) const {
  for (size_t i = 0; i < installQueue_.size(); ++i)
    if (installQueue_[i].kind == (app.title ? "" : "homebrew") && !app.downloadUrl.empty() &&
        installQueue_[i].file.url == app.downloadUrl)
      return int(i);
  return -1;
}
// The button: get it, update it, get it again; on its way, asks to cancel.
void Application::homebrewButton(const homebrew::App &app) {
  if (app.cardOnly) {
    message(tr("This app isn't on the server's list, so it can't be downloaded or updated here."));
    return;
  }
  double percent = 0;
  if (const int onTheWay = homebrewOnTheWay(app, percent)) {
    dialogFocus_ = 0;
    if (onTheWay == 2) {
      cancelRetro_ = !app.title; // a title installs as a game does
      confirmCancel_ = true;
      confirmExit_ = false;
    } else {
      unqueueAsk_ = homebrewQueuedAt(app);
      unqueuePick_ = false;
    }
    return;
  }
  getHomebrew(app);
}
std::string Application::homebrewButtonLabel(const homebrew::App &app) const {
  const auto status = homebrewStatus(app);
  if (status == homebrew::Status::Update) return trf("Update to v{0}", {app.version});
  if (status == homebrew::Status::Installed) return tr("Get again");
  return tr("Get");
}
void Application::homebrewDetail() {
  const auto *picked = homebrewPicked();
  if (!picked) {
    homebrewAsk_ = false;
    return;
  }
  const auto &app = *picked;
  const auto status = homebrewStatus(app);
  const auto icon = homebrewIcon(app);
  // Its screenshots go where a game's do, for the strip and the gallery.
  if (detail_.screenshots != app.screenshots) {
    detail_.screenshots = app.screenshots;
    galleryOpen_ = false;
    screenshot_ = 0;
  }
  // The server's wide picture behind the header, else the icon blurred.
  const auto backdrop = app.bannerUrl.empty() ? icon : app.bannerUrl;
  paint_.rect(0, 0, 1280, 212, {10, 10, 14, 255});
  if (!backdrop.empty() && !images_->loaded(backdrop) && !images_->failed(backdrop))
    paint_.skeleton(0, 0, 1280, 212, 0, true);
  images_->draw(backdrop, 0, 0, 1280, 212, 0, true);
  paint_.icon(7, 32, 14, Surface, 20);
  paint_.fonts.middle(homebrewReturn_ == Screen::Downloads ? tr("Downloads") : tr("Homebrew"), 56, 24, 17, Surface);
  hit(24, 4, 200, 40, [this] { action(Action::Back); });
  constexpr int Top = 58;
  cover(icon, 40, Top, 140, false, app.name, true, true);
  paint_.fonts.text(app.name, 208, Top + 4, 38, Surface, true, 1010);
  std::string byline = app.author;
  const auto onCard = homebrewCardVersion(app);
  if (!app.version.empty()) byline += (byline.empty() ? "" : " · ") + ("v" + app.version);
  paint_.fonts.text(byline, 208, Top + 62, 19, {228, 228, 234, 255}, false, 960);
  float px = 208;
  auto pill = [&](const std::string &text) {
    if (px + paint_.fonts.measure(text, 17) + 28 > 806) return;
    px += paint_.pill(text, px, Top + 100, true, 17) + 10;
  };
  if (status == homebrew::Status::Update)
    pill(onCard.empty() ? std::string(tr("Update|item")) : "v" + onCard + " → v" + app.version);
  else if (status == homebrew::Status::Installed)
    pill(tr("Installed"));
  pill(app.kind == homebrew::Kind::Port ? tr("Ports") : tr("Tools"));
  if (app.title) pill(tr("Installed as a title"));
  if (app.size) pill(formatBytes(app.size));
  // The left column scrolls in the band, as a game's description does.
  constexpr int BandTop = 212, BandBottom = 660, Pad = 14;
  SDL_Rect leftClip{0, BandTop, 806, BandBottom - BandTop};
  SDL_RenderSetClipRect(paint_.renderer, &leftClip);
  const int offset = int(std::lround(glide("description", 0, BandTop, 806, BandBottom - BandTop,
                                           float(descriptionScroll_ * ScrollStep), float(descriptionHeight_), 1)));
  int y = BandTop + Pad - offset;
  const std::string description =
      app.cardOnly ? tr("This app isn't on the server's list, so it can't be downloaded or updated here.") : app.description;
  if (!description.empty()) {
    y += int(paint_.fonts.wrapped(description, 40, y, 20, app.cardOnly ? Secondary : Ink, 750, 20, false, 1.6f)) + 24;
  } else {
    constexpr float Height = 92, Badge = 52;
    paint_.rect(40, y, 750, Height, Surface, 16);
    paint_.rect(64, y + (Height - Badge) / 2, Badge, Badge, {255, 241, 204, 255}, int(Badge / 2));
    paint_.icon(29, 64 + (Badge - 26) / 2, y + (Height - 26) / 2, Ink, 26);
    paint_.fonts.middle(tr("No description yet"), 136, y + 34, 20, Ink, true, 620);
    paint_.fonts.middle(tr("Check the details below."), 136, y + 62, 16, Secondary, false, 630);
    y += int(Height) + 16;
  }
  // Facts as a labeled grid, as a game's.
  struct Fact {
    int icon;
    std::string label, value;
    SDL_Color color = Ink;
  };
  std::vector<Fact> facts;
  if (!app.cardOnly && !app.version.empty()) facts.push_back({19, tr("Latest version"), "v" + app.version});
  if (!onCard.empty()) facts.push_back({24, tr("Installed version"), "v" + onCard});
  if (!app.author.empty()) facts.push_back({12, tr("Developer"), app.author});
  if (app.size) facts.push_back({14, tr("Size|info"), formatBytes(app.size)});
  if (!app.releasedAt.empty()) facts.push_back({11, tr("Released"), dayLabel(app.releasedAt)});
  facts.push_back({16, tr("Type|homebrew"), app.kind == homebrew::Kind::Port ? tr("Ports") : tr("Tools")});
  if (app.title) facts.push_back({26, tr("Title ID"), app.titleId});
  if (!app.requiredFirmware.empty()) {
    // A title the console's firmware is too old for says so, as a game does.
    Item need;
    need.requiredFirmware = app.requiredFirmware;
    const auto console = platform::firmwareVersion();
    if (!install::missingFirmware({need}, console).empty())
      facts.push_back({27, tr("Firmware"), trf("{0}+ · console {1}", {app.requiredFirmware, console}), Bad});
    else
      facts.push_back({27, tr("Firmware"), app.requiredFirmware});
  }
  if (app.stars >= 0) facts.push_back({31, tr("GitHub stars"), std::to_string(app.stars)});
  if (!app.license.empty()) facts.push_back({16, tr("License"), app.license});
  if (!app.updatedAt.empty()) facts.push_back({11, tr("Last updated"), dayLabel(app.updatedAt)});
  constexpr float Left = 40, Width = 750, Cell = Width / 3, RowHeight = 66;
  const int rows = int((facts.size() + 2) / 3);
  const auto file = homebrewFile(app);
  const float pathRow = file.empty() ? 0 : 58;
  paint_.rect(Left, float(y), Width, rows * RowHeight + 24 + pathRow, Surface, 16);
  for (size_t n = 0; n < facts.size(); ++n) {
    const float x = Left + 20 + (n % 3) * Cell, top = y + 16 + (n / 3) * RowHeight;
    paint_.icon(facts[n].icon, x, top + 1, Secondary, 16);
    paint_.fonts.middle(facts[n].label, x + 22, top + 9, 15, Secondary);
    paint_.fonts.text(facts[n].value, x, top + 24, 18, facts[n].color, true, Cell - 30);
  }
  if (pathRow > 0) {
    const float top = y + 16 + rows * RowHeight;
    paint_.rect(Left + 20, top - 6, Width - 40, 1, Line);
    paint_.icon(26, Left + 20, top + 9, Secondary, 16);
    paint_.fonts.middle(tr("Install location"), Left + 42, top + 17, 15, Secondary);
    const float labelW = paint_.fonts.measure(tr("Install location"), 15) + 18;
    paint_.fonts.middle(file, Left + 42 + labelW, top + 17, 16, Ink, false, Width - 82 - labelW);
  }
  y += int(rows * RowHeight + 24 + pathRow + 22);
  std::vector<std::string> languages;
  for (const auto &code : app.languages)
    if (const auto name = languageLabel(code); std::find(languages.begin(), languages.end(), name) == languages.end())
      languages.push_back(name);
  y = int(chipRow(tr("Languages"), languages, float(y)));
  y = screenshotStrip(y);
  // The release's notes, as plain text (made once per app).
  if (homebrewNotesOf_ != app.id) {
    homebrewNotesOf_ = app.id;
    homebrewNotes_ = homebrew::plainNotes(app.releaseNotes);
  }
  if (!homebrewNotes_.empty()) {
    paint_.fonts.text(tr("Release notes"), 40, y, 20, Ink, true);
    y += 40;
    y += int(paint_.fonts.wrapped(homebrewNotes_, 40, y, 17, Secondary, 750, 40, false, 1.5f)) + 24;
  }
  descriptionHeight_ = std::max(0, y + offset - (BandBottom - Pad));
  {
    SDL_Rect full{0, 0, 1280, 660};
    SDL_RenderSetClipRect(paint_.renderer, &full);
    constexpr float View = BandBottom - BandTop;
    paint_.scrollbar(798, BandTop + Pad, View - 2 * Pad, float(offset), View, View + float(descriptionHeight_),
                     descriptionFocus_);
  }
  homebrewDetailPanel(app);
  if (galleryOpen_) gallery();
}
// The right panel: the app's rows, then the button.
void Application::homebrewDetailPanel(const homebrew::App &app) {
  const auto extras = homebrewExtraKinds(app);
  const bool hasShortcut = homebrewHasShortcut(app);
  const bool panelFocus = !descriptionFocus_ && !homebrewExtras_;
  constexpr float X = 840, W = 386, Row = 56, Gap = 8, RowTop = 226;
  paint_.rect(818, 172, 430, 484, Surface, 20);
  paint_.fonts.middle(tr("Manage|app"), X, 194, 22, Ink, true);
  paint_.rect(818, 216, 430, 1, Line);
  if (extras.empty())
    paint_.fonts.wrapped(app.title ? tr("Opens from the HOME Menu once installed.")
                                   : tr("Once it is on the card, its patches, saves and HOME Menu shortcut are managed here."), X,
                         RowTop + 6, 17, Secondary, W, 3);
  for (int i = 0; i < int(extras.size()) && i < 6; ++i) {
    const float rowY = RowTop + i * (Row + Gap);
    hit(X, rowY, W, Row, [this, i] {
      descriptionFocus_ = false;
      homebrewAskRow_ = i;
      action(Action::Accept);
    });
    if (i == homebrewAskRow_ && panelFocus) paint_.focusRow(X, rowY, W, Row, Ground, 12);
    else paint_.rect(X, rowY, W, Row, Ground, 12);
    const float mid = rowY + Row / 2;
    if (extras[i] == Shortcut) {
      const bool blocked = !hasShortcut && !homebrewShortcutPossible(app);
      const auto ink = blocked ? Secondary : Ink;
      paint_.icon(0, X + 16, mid - 12, ink, 24);
      paint_.fonts.middle(tr("HOME Menu shortcut"), X + 52, mid, 18, ink, true, W - 160);
      const bool local = platform::canMakeShortcuts() && homebrew::cardNroPath(homebrewFile(app));
      const bool unsigned_ = !hasShortcut && !blocked && local && !signaturePatches_;
      const bool queued = std::any_of(installQueue_.begin(), installQueue_.end(), [&](const QueuedInstall &q) {
        return (q.kind == "shortcut" && q.name == trf("{0} shortcut", {app.name})) ||
               (!app.forwarderUrl.empty() && q.file.url == app.forwarderUrl);
      });
      const bool stale = !queued && homebrewShortcutStale(app);
      const std::string state = queued ? tr("Queued") : stale ? tr("Update|item") : hasShortcut ? tr("Installed")
                                : blocked ? tr("Unavailable") : unsigned_ ? tr("Needs sig patches") : tr("Not installed");
      paint_.fonts.middle(state, X + W - 16 - paint_.fonts.measure(state, 16, true), mid, 16,
                          stale || unsigned_ ? Pending : hasShortcut ? Good : Secondary, true);
      continue;
    }
    if (extras[i] == Remove) {
      paint_.icon(37, X + 16, mid - 12, Bad, 24);
      paint_.fonts.middle(tr("Delete"), X + 52, mid, 18, Bad, true);
      continue;
    }
    const bool patches = extras[i] == Patches;
    paint_.icon(patches ? 32 : 36, X + 16, mid - 12, Ink, 24);
    const float labelEnd = paint_.fonts.middle(patches ? tr("Patches") : tr("Saves"), X + 52, mid, 18, Ink, true);
    int count = 0, on = 0;
    if (patches)
      for (const auto &row : homebrewPatchRows(app)) ++count, on += row.record && row.record->on;
    else
      count = int(homebrewSaveRows(app).size());
    paint_.pill(std::to_string(count), labelEnd + 10, mid - 13);
    if (on) {
      const auto state = trf("{0} on", {std::to_string(on)});
      paint_.fonts.middle(state, X + W - 16 - paint_.fonts.measure(state, 16, true), mid, 16, Good, true);
    }
  }
  paint_.rect(X, 494, W, 1, Line);
  paint_.fonts.middle(tr("Size|info"), X + 12, 530, 18, Secondary);
  const auto size = app.size ? formatBytes(app.size) : std::string("-");
  paint_.fonts.middle(size, X + W - 12 - paint_.fonts.measure(size, 24, true), 530, 24, Ink, true);
  buttonHit({X, 568, W, 60}, 14, [this, app] {
    descriptionFocus_ = false;
    homebrewAskRow_ = -1;
    homebrewButton(app);
  });
  if (homebrewAskRow_ < 0 && panelFocus) paint_.ring(X, 568, W, 60, 14);
  double percent = 0;
  if (const int onTheWay = homebrewOnTheWay(app, percent)) {
    const bool mine = retroBusy_ == "homebrew:" + app.id && retroBytes_;
    onTheWayButton(X, W, onTheWay == 2, percent, mine ? 100.0 * double(retroKept_.load()) / double(retroBytes_) : 0);
  } else if (app.cardOnly) {
    paint_.rect(X, 568, W, 60, {228, 225, 218, 255}, 14);
    const std::string label = tr("Not on the server");
    paint_.fonts.middle(label, X + (W - paint_.fonts.measure(label, 21, true)) / 2, 598, 21, Secondary, true);
  } else {
    paint_.rect(X, 568, W, 60, Butter, 14);
    const auto label = homebrewButtonLabel(app);
    paint_.keyLabel("A", label, X + (W - paint_.keyLabelWidth("A", label, 22, true)) / 2, 598, 22, Ink, true);
  }
}
} // namespace foil::ui
