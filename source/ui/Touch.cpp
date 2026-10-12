#include "ui/Application.hpp"
#include <algorithm>
#include <cmath>
namespace foil::ui {
void Application::hit(float x, float y, float w, float h, std::function<void()> run) {
  hits_.push_back({{x, y, w, h}, std::move(run)});
}
void Application::headerItem(const std::string &id, float x, float y, float w, float h, std::function<void()> run,
                             bool home) {
  hit(x, y, w, h, run);
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
    return savesTab_ == 0 ? selected_ <= 0 : savesTab_ == 1 ? shopSaveRow_ <= 0 : dropboxRow_ <= 0;
  default:
    return false;
  }
}
bool Application::popupOpen() const {
  return !alert_.empty() || appUpdateAsk_ || appUpdate_ == AppUpdate::Downloading || viewPopup_ || confirmInstall_ || confirmCancel_ || confirmResume_ || languagePopup_ ||
         confirmCleanup_ || cleanupBusy_ || confirmPatchRemove_ || managingPatch_ || pickingPatch_ || serverForm_ || homebrewRemoveAsk_ || homebrewAsk_ || shortcutRemoveAsk_ || selfShortcutAsk_ ||
         retroAsking_ || retroArchAsking_ || saveNeedsGame_ || savePicking_ || shopAppSaveAsk_ || applyAsking_ || managedGame_ ||
         removal_ || galleryOpen_ || (screen_ == Screen::Saves && (dropboxModal() || dropboxSession_));
}
void Application::touch(float x, float y) {
  poll();
  // A tap moves the cursor off the header; the tapped thing takes it.
  headerFocus_.clear();
  // Hits are recorded in draw order, so the last match is the one on top.
  for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
    const auto &a = it->area;
    if (x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h) {
      const auto run = it->run;
      run();
      return;
    }
  }
}
void Application::drag(float dy) {
  dragRest_ += dy;
  const bool detailText = screen_ == Screen::Detail && !galleryOpen_;
  const float step = detailText ? ScrollStep : 240;
  while (std::abs(dragRest_) >= step) {
    // Finger up moves the content up, which reads further down.
    const bool forward = dragRest_ < 0;
    dragRest_ += forward ? step : -step;
    if (detailText)
      action(forward ? Action::ScrollDown : Action::ScrollUp);
    else if (screen_ == Screen::Library || screen_ == Screen::Installed || screen_ == Screen::Homebrew)
      action(forward ? Action::Down : Action::Up);
  }
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
