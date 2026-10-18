#include "ui/Application.hpp"
#include <algorithm>
#include <cmath>
namespace foil::ui {
void Application::hit(float x, float y, float w, float h, std::function<void()> run) {
  // Inside a gliding list only the part on screen answers.
  if (!clips_.empty() && SDL_RenderIsClipEnabled(paint_.renderer)) {
    SDL_Rect clip{};
    SDL_RenderGetClipRect(paint_.renderer, &clip);
    const float left = std::max(x, float(clip.x)), top = std::max(y, float(clip.y));
    const float right = std::min(x + w, float(clip.x + clip.w)), bottom = std::min(y + h, float(clip.y + clip.h));
    if (right <= left || bottom <= top)
      return;
    x = left, y = top, w = right - left, h = bottom - top;
  }
  hits_.push_back({{x, y, w, h}, std::move(run)});
}
// A touched header item takes the cursor, as a touched card or menu item
// does. 내 게임's tabs open sibling screens that draw the same tabs, so
// the cursor lands on the touched tab there too.
void Application::headerHit(const std::string &id, float x, float y, float w, float h,
                            const std::function<void()> &run) {
  hit(x, y, w, h, [this, id, run] {
    run();
    headerFocus_ = id;
    sidebar_ = autoSidebar_ = false;
  });
}
void Application::headerItem(const std::string &id, float x, float y, float w, float h, std::function<void()> run,
                             bool home) {
  headerHit(id, x, y, w, h, run);
  header_.push_back({id, {x, y, w, h}, std::move(run), false});
  if (home || headerHome_.empty()) headerHome_ = id;
  if (headerFocus_ == id) paint_.ring(x, y, w, h, 12);
}
bool Application::enterHeader() {
  if (header_.empty()) return false;
  headerFocus_ = headerHome_.empty() ? header_.front().id : headerHome_;
  sidebar_ = autoSidebar_ = false;
  return true;
}
bool Application::headerAction(Action a) {
  // Left to right as drawn, whatever order the screen drew them in.
  std::vector<const HeaderItem *> row;
  for (const auto &item : header_) row.push_back(&item);
  std::stable_sort(row.begin(), row.end(), [](const auto *l, const auto *r) { return l->area.x < r->area.x; });
  auto at = std::find_if(row.begin(), row.end(), [&](const auto *item) { return item->id == headerFocus_; });
  if (at == row.end()) {
    // What had the cursor is gone (a button that hides): back to the start.
    if (!enterHeader()) {
      headerFocus_.clear();
      return false;
    }
    at = std::find_if(row.begin(), row.end(), [&](const auto *item) { return item->id == headerFocus_; });
    if (at == row.end()) return false;
  }
  const auto land = [&](const HeaderItem &item) {
    headerFocus_ = item.id;
    if (item.live) item.run();
  };
  switch (a) {
  case Action::Left:
    if (at != row.begin()) land(**(at - 1));
    else {
      headerFocus_.clear();
      sidebar_ = true;
    }
    return true;
  case Action::Right:
    if (at + 1 != row.end()) land(**(at + 1));
    return true;
  case Action::Up:
    return true;
  case Action::Down:
    if (hasFocusable()) headerFocus_.clear();
    return true;
  case Action::Back:
    // As B from the content: a search clears first, a list goes back.
    headerFocus_.clear();
    return false;
  case Action::Accept: {
    // On the tabs A steps to the next one, round to the first after the
    // last, like R; the cursor stays on the strip.
    if ((*at)->live) {
      auto next = at + 1;
      if (next == row.end() || !(*next)->live)
        while (next != row.begin() && (*(next - 1))->live) --next;
      land(**next);
      return true;
    }
    const auto run = (*at)->run;
    headerFocus_.clear();
    run();
    return true;
  }
  default:
    // L R on the tabs move the cursor with them (headerTabs follows the
    // tab shown); other keys do what they always do, from the content.
    if ((a == Action::Previous || a == Action::Next) && headerFocus_.rfind("tab", 0) == 0) return false;
    headerFocus_.clear();
    return false;
  }
}
bool Application::contentAtTop() const {
  switch (screen_) {
  case Screen::Library:
  case Screen::Installed:
  case Screen::Homebrew:
    return selected_ < 6; // rows of six
  case Screen::Updates:
  case Screen::Patches:
    return selected_ <= 0;
  case Screen::Downloads:
    return downloadsRow_ <= 0;
  case Screen::Retro:
    return retroView_ == RetroView::Home ? retroRow_ <= 0 : selected_ <= 0;
  case Screen::Saves:
    return savesTab_ == 0 ? selected_ <= 0 : savesTab_ == 1 ? shopSaveRow_ <= 0 : (!dropboxLinked_ || dropboxOnAccount_); // the account row is the first
  default:
    return false;
  }
}
void Application::scrollToTop() {
  if (sidebar_ || !headerFocus_.empty() || popupOpen() || fullPage()) return;
  const bool list = screen_ == Screen::Home || screen_ == Screen::Library || screen_ == Screen::Installed ||
                    screen_ == Screen::Homebrew || screen_ == Screen::Updates || screen_ == Screen::Patches ||
                    screen_ == Screen::Downloads || screen_ == Screen::Retro || screen_ == Screen::Saves;
  if (!list) return;
  // Up row by row, as the pad would, so every screen's own rules hold;
  // it stops on the first row, short of the header.
  const auto top = [this] { return screen_ == Screen::Home ? row_ <= 0 : contentAtTop(); };
  for (int n = 0; n < 4000 && !top(); ++n) action(Action::Up);
}
bool Application::popupOpen() const {
  return !alert_.empty() || clockModal() || unqueueAsk_ >= 0 || bugAsk_ || appUpdateAsk_ || appUpdate_ == AppUpdate::Downloading || viewPopup_ || confirmInstall_ || confirmCancel_ || languagePopup_ ||
         tidyOpen_ || homebrewSortAsk_ || confirmPatchRemove_ || managingPatch_ || pickingPatch_ || serverForm_ || homebrewRemoveAsk_ || (homebrewAsk_ && homebrewExtras_) || shortcutRemoveAsk_ || selfShortcutAsk_ ||
         retroArchAsking_ || saveNeedsGame_ || savePicking_ || shopAppSaveAsk_ || applyAsking_ || managedGame_ ||
         removal_ || galleryOpen_ || (screen_ == Screen::Saves && (dropboxModal() || dropboxSession_));
}
void Application::touch(float x, float y) {
  // The pressed look stays a moment after the finger lifts, so a quick tap
  // still shows it took.
  if (pressing_ && !pressedUntil_) pressedUntil_ = SDL_GetTicks() + 120;
  if (swallowTap_) {
    swallowTap_ = false;
    return;
  }
  // A tap moves the cursor off the header; the tapped thing takes it.
  headerFocus_.clear();
  // Hits are recorded in draw order, so the last match is the one on top.
  // They are what the last frame showed: the tap goes there before anything
  // that finished meanwhile (a download, its shortcut) changes the list.
  for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
    const auto &a = it->area;
    if (x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h) {
      const auto run = it->run;
      const uint32_t from = SDL_GetTicks();
      run();
      notePress("tap", from);
      break;
    }
  }
  poll();
}
void Application::drag(float dx, float dy) {
  // In a gliding area: the view follows the finger (finger up reads further
  // down), and the speed of the last moves becomes the flick. Elsewhere a
  // drag does nothing; it never moves the cursor.
  if (!glide_.held)
    return;
  const uint32_t now = SDL_GetTicks();
  const float move = glide_.horizontal ? dx : dy;
  glide_.travel += move;
  const float steps = -move / glide_.unit;
  glide_.pos = glideFollow(steps);
  const float dt = std::max(0.008f, (now - glide_.at) / 1000.f);
  // Smoothed, so one jittery sample does not throw the list.
  glide_.velocity = 0.6f * (steps / dt) + 0.4f * glide_.velocity;
  glide_.at = now;
}
void Application::hintAction(const std::string &key) {
  if (key == "A") action(Action::Accept);
  else if (key == "B") action(Action::Back);
  else if (key == "X") {
    if (screen_ == Screen::Library) searchRequested_ = true;
    else action(Action::Search);
  } else if (key == "Y") action(screen_ == Screen::Settings ? Action::Reload : Action::Queue);
  else if (key == "L R") {
    action(Action::Next);
  }
  else if (key == "ZR") action(Action::Preview);
  else if (key == "+") exitRequested_ = requestQuit();
  else if (key == "-") refreshAll();
}
} // namespace foil::ui
