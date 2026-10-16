#include "ui/ShortcutIcon.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <algorithm>
#include <vector>
namespace foil::ui {
namespace {
// The control data holds an icon of at most this many bytes.
constexpr size_t MaxIcon = 0x20000;
struct Freed {
  SDL_Surface *surface;
  ~Freed() { SDL_FreeSurface(surface); }
};
} // namespace
std::string shortcutIconJpeg(const std::string &image, int side) {
  if (image.empty() || side <= 0) return {};
  SDL_Surface *loaded = IMG_Load_RW(SDL_RWFromConstMem(image.data(), int(image.size())), 1);
  if (!loaded) return {};
  const Freed source{SDL_ConvertSurfaceFormat(loaded, SDL_PIXELFORMAT_RGB24, 0)};
  SDL_FreeSurface(loaded);
  if (!source.surface || source.surface->w <= 0 || source.surface->h <= 0) return {};
  const Freed out{SDL_CreateRGBSurfaceWithFormat(0, side, side, 24, SDL_PIXELFORMAT_RGB24)};
  if (!out.surface) return {};
  // The middle square, each output pixel the average of the source pixels
  // it covers (a box filter: no shimmer on a large picture made small).
  const int edge = std::min(source.surface->w, source.surface->h);
  const int left = (source.surface->w - edge) / 2, top = (source.surface->h - edge) / 2;
  const auto *in = static_cast<const uint8_t *>(source.surface->pixels);
  auto *to = static_cast<uint8_t *>(out.surface->pixels);
  for (int y = 0; y < side; ++y) {
    const int y0 = top + y * edge / side, y1 = std::max(y0 + 1, top + (y + 1) * edge / side);
    for (int x = 0; x < side; ++x) {
      const int x0 = left + x * edge / side, x1 = std::max(x0 + 1, left + (x + 1) * edge / side);
      uint32_t sum[3]{}, count = 0;
      for (int sy = y0; sy < y1; ++sy)
        for (int sx = x0; sx < x1; ++sx, ++count) {
          const auto *p = in + sy * source.surface->pitch + sx * 3;
          sum[0] += p[0], sum[1] += p[1], sum[2] += p[2];
        }
      auto *q = to + y * out.surface->pitch + x * 3;
      for (int c = 0; c < 3; ++c) q[c] = uint8_t(sum[c] / count);
    }
  }
  // As good as fits: a busy picture at the top quality can run over.
  std::vector<char> buffer(MaxIcon);
  for (int quality = 92; quality >= 50; quality -= 14) {
    SDL_RWops *rw = SDL_RWFromMem(buffer.data(), int(buffer.size()));
    if (!rw) return {};
    const bool saved = IMG_SaveJPG_RW(out.surface, rw, 0, quality) == 0;
    const auto size = SDL_RWtell(rw);
    SDL_RWclose(rw);
    if (saved && size > 4 && size_t(size) <= buffer.size()) return std::string(buffer.data(), size_t(size));
  }
  return {};
}
} // namespace foil::ui
