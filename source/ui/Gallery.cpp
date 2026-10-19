#include "ui/Application.hpp"
namespace foil::ui {
// The page's screenshots (detail_.screenshots) under its description: the
// first three, each opening the gallery at itself; y below them.
int Application::screenshotStrip(int y) {
  const auto &shots = detail_.screenshots;
  if (shots.empty()) return y;
  paint_.fonts.text(tr("Screenshots"), 40, y, 20, Ink, true);
  paint_.keyLabel("ZR", tr("Enlarge · or tap a picture"), 150, y + 14, 17, Secondary);
  y += 40;
  for (size_t i = 0; i < shots.size() && i < 3; ++i) {
    const int x = 40 + int(i) * 214;
    hit(x, y, 200, 112, [this, i] {
      galleryOpen_ = true;
      screenshot_ = i;
    });
    const auto thumb = thumbnailUrl(shots[i]);
    // A picture that will not load says so instead of shimmering forever.
    if (images_->failed(thumb)) {
      paint_.rect(x, y, 200, 112, Line, 10);
      const std::string none = tr("No image");
      paint_.fonts.middle(none, x + (200 - paint_.fonts.measure(none, 16)) / 2, y + 56, 16, Secondary);
      continue;
    }
    if (!images_->loaded(thumb)) paint_.skeleton(x, y, 200, 112, 10);
    images_->draw(thumb, x, y, 200, 112, 10);
  }
  return y + 134;
}
// A shop path ("/api/…") made full against the server; a full address as it is.
std::string Application::shopAddress(const std::string &url) const {
  return !url.empty() && url[0] == '/' && session_ ? session_->server().url + url : url;
}
// Whose screenshots the gallery shows: the game, the RetroArch game or the app.
std::string Application::galleryName() const {
  if (screen_ == Screen::RetroDetail) return retroPick_.name;
  if (screen_ == Screen::Patches) {
    const auto rows = patchRows();
    const auto *row = pickedPatch(rows);
    return row ? row->patch.name : std::string();
  }
  if (screen_ == Screen::Homebrew) {
    const auto *app = homebrewPicked();
    return app ? app->name : std::string();
  }
  return detailGroup_ < groups_.size() ? groups_[detailGroup_].representative.name : std::string();
}
void Application::gallery() {
  if (detail_.screenshots.empty()) return;
  screenshot_ %= detail_.screenshots.size();
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
  paint_.rect(0, 0, 1280, 660, {10, 10, 14, 255});
  hit(0, 0, 640, 660, [this] { action(Action::Previous); });
  hit(640, 0, 640, 660, [this] { action(Action::Next); });
  const auto name = galleryName();
  // B 닫기 at the top right, a small white chip; a tap on it closes too.
  const float close = paint_.keyChip("B", tr("Close"), 1240, 40);
  const float after = paint_.fonts.middle(tr("Screenshots"), 40, 40, 26, Surface, true);
  paint_.fonts.middle(name, after + 16, 40, 19, {170, 170, 178, 255}, false, 1240 - close - 24 - after - 16);
  // The full-screen photo at the screen's width, not the original.
  const auto url = thumbnailUrl(detail_.screenshots[screenshot_], "xl");
  if (images_->failed(url)) {
    paint_.fonts.text(tr("Could not load the photo. Try again shortly."), 40, 330, 22, Surface);
  } else if (!images_->loaded(url)) {
    // Until the large photo arrives, the small one the detail page already
    // holds stands in, so a turn never flashes a light loading box over
    // the dark backdrop.
    const auto small = thumbnailUrl(detail_.screenshots[screenshot_]);
    if (images_->loaded(small))
      images_->draw(small, 40, 78, 1200, 558, 10, false, true);
    else
      paint_.skeleton(40, 78, 1200, 558, 10, true);
  }
  images_->draw(url, 40, 78, 1200, 558, 10, false, true);
  // Page dots under the photo: the current one long and butter.
  if (detail_.screenshots.size() > 1) {
    constexpr float Dot = 8, Gap = 10, Active = 26;
    const size_t count = detail_.screenshots.size();
    const float width = Active + (count - 1) * (Dot + Gap);
    float x = 640 - width / 2;
    paint_.rect(x - 14, 600, width + 28, 26, {10, 10, 14, 170}, 13);
    for (size_t n = 0; n < count; ++n) {
      const bool current = n == screenshot_;
      paint_.rect(x, 609, current ? Active : Dot, Dot, current ? Butter : SDL_Color{150, 150, 158, 255}, 4);
      x += (current ? Active : Dot) + Gap;
    }
  }
  // Round arrow buttons over the photo edges; taps go through the halves too.
  if (detail_.screenshots.size() > 1) {
    constexpr float Size = 60, Middle = 78 + 558 / 2.f;
    const struct { float x; int icon; Action move; } arrows[] = {
        {64, 7, Action::Previous}, {1216 - Size, 9, Action::Next}};
    for (const auto &arrow : arrows) {
      hit(arrow.x, Middle - Size / 2, Size, Size, [this, move = arrow.move] { action(move); });
      // Light buttons with a dark arrow: they stand out on the black
      // backdrop and on dark photos alike.
      paint_.rect(arrow.x - 2, Middle - Size / 2 - 2, Size + 4, Size + 4, {0, 0, 0, 90}, int(Size / 2) + 2);
      paint_.rect(arrow.x, Middle - Size / 2, Size, Size, {Surface.r, Surface.g, Surface.b, 235}, int(Size / 2));
      paint_.icon(arrow.icon, arrow.x + (Size - 30) / 2, Middle - 15, Ink, 30);
    }
  }
  // Over the halves' turns, so the chip closes rather than turns.
  hit(1240 - close - 8, 18, close + 16, 44, [this] { action(Action::Back); });
}
} // namespace foil::ui
