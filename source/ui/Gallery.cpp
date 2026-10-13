#include "ui/Application.hpp"
namespace foil::ui {
void Application::gallery() {
  if (detail_.screenshots.empty()) return;
  screenshot_ %= detail_.screenshots.size();
  SDL_RenderSetClipRect(paint_.renderer, nullptr);
  paint_.rect(0, 0, 1280, 660, {10, 10, 14, 255});
  hit(0, 0, 640, 660, [this] { action(Action::Previous); });
  hit(640, 0, 640, 660, [this] { action(Action::Next); });
  const auto name = detailGroup_ < groups_.size() ? groups_[detailGroup_].representative.name : std::string();
  const float after = paint_.fonts.middle(tr("스크린샷"), 40, 40, 26, Surface, true);
  paint_.fonts.middle(name, after + 16, 40, 19, {170, 170, 178, 255}, false, 1180 - after);
  // The full-screen photo at the screen's width, not the original.
  const auto url = thumbnailUrl(detail_.screenshots[screenshot_], "xl");
  if (images_->failed(url)) {
    paint_.fonts.text(tr("사진을 불러오지 못했습니다. 잠시 뒤 다시 시도합니다."), 40, 330, 22, Surface);
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
}
} // namespace foil::ui
