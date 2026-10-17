#include "ui/ShortcutIcon.hpp"
#include <SDL.h>
#include <SDL_image.h>
#include <algorithm>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>
namespace foil::ui {
namespace {
// The control data holds an icon of at most this many bytes.
constexpr size_t MaxIcon = 0x20000;
// libjpeg straight into memory, its errors caught here: SDL_image's JPEG
// writer (IMG_SaveJPG_RW) crashed the app on the console (0.3.133, 0.3.134).
struct Escape {
  jpeg_error_mgr base;
  std::jmp_buf back;
};
void escape(j_common_ptr info) { std::longjmp(reinterpret_cast<Escape *>(info->err)->back, 1); }
std::string encodeJpeg(const uint8_t *rgb, int width, int height, int pitch, int quality) {
  jpeg_compress_struct info{};
  Escape errors{};
  info.err = jpeg_std_error(&errors.base);
  errors.base.error_exit = escape;
  errors.base.output_message = [](j_common_ptr) {}; // nothing to stderr
  unsigned char *buffer = nullptr;
  unsigned long size = 0;
  if (setjmp(errors.back)) {
    jpeg_destroy_compress(&info);
    std::free(buffer);
    return {};
  }
  jpeg_create_compress(&info);
  jpeg_mem_dest(&info, &buffer, &size);
  info.image_width = JDIMENSION(width);
  info.image_height = JDIMENSION(height);
  info.input_components = 3;
  info.in_color_space = JCS_RGB;
  jpeg_set_defaults(&info);
  jpeg_set_quality(&info, quality, TRUE);
  jpeg_start_compress(&info, TRUE);
  while (info.next_scanline < info.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(rgb + size_t(info.next_scanline) * size_t(pitch));
    jpeg_write_scanlines(&info, &row, 1);
  }
  jpeg_finish_compress(&info);
  std::string out(reinterpret_cast<const char *>(buffer), size_t(size));
  jpeg_destroy_compress(&info);
  std::free(buffer);
  return out;
}
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
  for (int quality = 92; quality >= 50; quality -= 14) {
    auto jpeg = encodeJpeg(static_cast<const uint8_t *>(out.surface->pixels), side, side, out.surface->pitch, quality);
    if (jpeg.size() > 4 && jpeg.size() <= MaxIcon) return jpeg;
    if (jpeg.empty()) break; // it failed, not too large
  }
  return {};
}
} // namespace foil::ui
