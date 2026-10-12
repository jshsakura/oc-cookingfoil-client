#include "ui/Qr.hpp"
#include "qrcodegen.hpp"
namespace foil::ui {
bool Qr::encode(const std::string &url) {
  if (url == url_)
    return size_ > 0;
  url_ = url;
  size_ = 0;
  modules_.clear();
  if (url.empty() || url.size() > 512)
    return false;
  try {
    const auto qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
    size_ = qr.getSize();
    modules_.resize(size_ * size_);
    for (int y = 0; y < size_; ++y)
      for (int x = 0; x < size_; ++x)
        modules_[y * size_ + x] = qr.getModule(x, y);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}
void Qr::draw(SDL_Renderer *renderer, int x, int y, int width) const {
  if (!size_)
    return;
  int scale = width / (size_ + 8);
  if (scale >= 4)
    scale -= scale % 2;
  if (scale < 2)
    return;
  const int margin = (width - (size_ + 8) * scale) / 2 + 4 * scale;
  SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
  SDL_Rect background{x, y, width, width};
  SDL_RenderFillRect(renderer, &background);
  SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
  for (int yy = 0; yy < size_; ++yy)
    for (int xx = 0; xx < size_; ++xx)
      if (modules_[yy * size_ + xx]) {
        SDL_Rect module{x + margin + xx * scale, y + margin + yy * scale, scale, scale};
        SDL_RenderFillRect(renderer, &module);
      }
}
} // namespace foil::ui
