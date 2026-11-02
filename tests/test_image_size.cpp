#include "Test.hpp"
#include "core/ImageSize.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
// A picture's size from its header, for each format, matching what it
// decodes to; anything cut or of another kind is not guessed.
int runImageSizeTests() {
  int failures = 0;
  uint32_t w = 0, h = 0;
  // Real files written by SDL_image: a PNG and a JPEG of 300 x 170.
  auto *surface = SDL_CreateRGBSurfaceWithFormat(0, 300, 170, 32, SDL_PIXELFORMAT_RGBA32);
  CHECK(surface);
  if (!surface) return failures;
  for (const bool jpeg : {false, true}) {
    char path[] = "/tmp/cookingfoil-image-XXXXXX";
    const int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd < 0) continue;
    close(fd);
    CHECK((jpeg ? IMG_SaveJPG(surface, path, 80) : IMG_SavePNG(surface, path)) == 0);
    std::string bytes;
    if (auto *f = std::fopen(path, "rb")) {
      char chunk[4096];
      for (size_t n; (n = std::fread(chunk, 1, sizeof chunk, f)) > 0;) bytes.append(chunk, n);
      std::fclose(f);
    }
    std::remove(path);
    CHECK(foil::imageSize(bytes, w, h) && w == 300 && h == 170);
    // Every cut of the header is refused or read whole, never past the end.
    for (size_t cut = 0; cut < std::min<size_t>(bytes.size(), 700); ++cut)
      if (foil::imageSize(bytes.substr(0, cut), w, h)) CHECK(w == 300 && h == 170);
  }
  SDL_FreeSurface(surface);
  // GIF and the three WebP kinds, from their headers.
  std::string gif = "GIF89a";
  gif += std::string("\x00\x10\x00\x08", 4);
  CHECK(foil::imageSize(gif, w, h) && w == 4096 && h == 2048);
  std::string vp8x = std::string("RIFF\0\0\0\0WEBPVP8X", 16) + std::string(14, '\0');
  vp8x[24] = char(0xFF), vp8x[25] = char(0x0F); // 4095 + 1
  vp8x[27] = char(0xFF), vp8x[28] = char(0x0F);
  CHECK(foil::imageSize(vp8x, w, h) && w == 4096 && h == 4096);
  std::string vp8 = std::string("RIFF\0\0\0\0WEBPVP8 ", 16) + std::string(14, '\0');
  vp8[26] = char(0x80), vp8[27] = char(0x07); // 1920
  vp8[28] = char(0x38), vp8[29] = char(0x04); // 1080
  CHECK(foil::imageSize(vp8, w, h) && w == 1920 && h == 1080);
  CHECK(!foil::imageSize("<html>not a picture</html>", w, h) && !foil::imageSize("", w, h));
  return failures;
}
