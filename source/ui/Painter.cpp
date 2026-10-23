#include "ui/Painter.hpp"
#include <SDL2/SDL_image.h>
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include <algorithm>
#include <cmath>
#include <iterator>
#include <map>
#include <string>
namespace foil::ui {
// A white top-left quarter circle of radius r whose edge pixels carry
// their coverage as alpha (4x4 samples), tinted per draw.
SDL_Texture *Painter::cornerTexture(int r) {
  if (const auto found = corners_.find(r); found != corners_.end())
    return found->second;
  SDL_Texture *texture = nullptr;
  if (auto *surface = SDL_CreateRGBSurfaceWithFormat(0, r, r, 32, SDL_PIXELFORMAT_RGBA32)) {
    auto *pixels = static_cast<Uint32 *>(surface->pixels);
    const int stride = surface->pitch / 4;
    for (int py = 0; py < r; ++py)
      for (int px = 0; px < r; ++px) {
        int inside = 0;
        for (int sy = 0; sy < 4; ++sy)
          for (int sx = 0; sx < 4; ++sx) {
            const float dx = r - (px + (sx + .5f) / 4), dy = r - (py + (sy + .5f) / 4);
            inside += dx * dx + dy * dy <= float(r) * r;
          }
        pixels[py * stride + px] =
            SDL_MapRGBA(surface->format, 255, 255, 255, Uint8(inside * 255 / 16));
      }
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (texture)
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surface);
  }
  corners_[r] = texture;
  return texture;
}
void Painter::rect(float x, float y, float w, float h, SDL_Color color,
                   int radius) {
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  if (!radius) {
    SDL_FRect r{x, y, w, h};
    SDL_RenderFillRectF(renderer, &r);
    return;
  }
  float sx, sy;
  SDL_RenderGetScale(renderer, &sx, &sy);
  SDL_Rect clip;
  const bool clipped = SDL_RenderIsClipEnabled(renderer);
  SDL_RenderGetClipRect(renderer, &clip);
  SDL_RenderSetScale(renderer, 1, 1);
  if (clipped) {
    SDL_Rect physical{int(clip.x * sx), int(clip.y * sy), int(clip.w * sx),
                      int(clip.h * sy)};
    SDL_RenderSetClipRect(renderer, &physical);
  }
  // Physical pixels: the straight parts as plain fills, each corner as a
  // smoothed quarter circle, so curves never show stair steps.
  const int left = int(std::lround(x * sx)), top = int(std::lround(y * sy));
  const int width = int(std::lround((x + w) * sx)) - left;
  const int height = int(std::lround((y + h) * sy)) - top;
  const int r = std::min({int(std::lround(radius * sx)), width / 2, height / 2});
  if (r <= 0) {
    SDL_Rect whole{left, top, width, height};
    SDL_RenderFillRect(renderer, &whole);
  } else {
    const SDL_Rect fills[] = {{left + r, top, width - 2 * r, r},
                              {left, top + r, width, height - 2 * r},
                              {left + r, top + height - r, width - 2 * r, r}};
    for (const auto &fill : fills)
      if (fill.w > 0 && fill.h > 0)
        SDL_RenderFillRect(renderer, &fill);
    if (auto *corner = cornerTexture(r)) {
      SDL_SetTextureColorMod(corner, color.r, color.g, color.b);
      SDL_SetTextureAlphaMod(corner, color.a);
      const struct { int x, y; SDL_RendererFlip flip; } corners[] = {
          {left, top, SDL_FLIP_NONE},
          {left + width - r, top, SDL_FLIP_HORIZONTAL},
          {left, top + height - r, SDL_FLIP_VERTICAL},
          {left + width - r, top + height - r, SDL_RendererFlip(SDL_FLIP_HORIZONTAL | SDL_FLIP_VERTICAL)}};
      for (const auto &c : corners) {
        SDL_Rect to{c.x, c.y, r, r};
        SDL_RenderCopyEx(renderer, corner, nullptr, &to, 0, nullptr, c.flip);
      }
    }
  }
  SDL_RenderSetScale(renderer, sx, sy);
  if (clipped)
    SDL_RenderSetClipRect(renderer, &clip);
}
void Painter::line(float x, float y, float xx, float yy, SDL_Color color,
                   int width) {
  float sx, sy;
  SDL_RenderGetScale(renderer, &sx, &sy);
  SDL_Rect clip;
  const bool clipped = SDL_RenderIsClipEnabled(renderer);
  SDL_RenderGetClipRect(renderer, &clip);
  SDL_RenderSetScale(renderer, 1, 1);
  if (clipped) {
    SDL_Rect physical{int(clip.x * sx), int(clip.y * sy), int(clip.w * sx),
                      int(clip.h * sy)};
    SDL_RenderSetClipRect(renderer, &physical);
  }
  SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
  const float dx = (xx - x) * sx, dy = (yy - y) * sy,
              length = std::max(1.f, std::sqrt(dx * dx + dy * dy));
  for (int i = 0; i < std::lround(width * sx); ++i) {
    const float offset = i - (width * sx - 1) / 2;
    SDL_RenderDrawLine(renderer, std::lround(x * sx - dy / length * offset),
                       std::lround(y * sy + dx / length * offset),
                       std::lround(xx * sx - dy / length * offset),
                       std::lround(yy * sy + dx / length * offset));
  }
  SDL_RenderSetScale(renderer, sx, sy);
  if (clipped)
    SDL_RenderSetClipRect(renderer, &clip);
}
void Painter::skeleton(float x, float y, float w, float h, int radius, bool dark, SDL_Color on) {
  // A slow, soft breath between two close shades of the backdrop: it reads
  // as loading without flashing. On a dark backdrop a light grey would.
  // Both shades come from the backdrop it sits on, so the menu's white and
  // the page's ground breathe alike (fixed shades faded into one and not
  // the other).
  constexpr float PulseMs = 2000;
  constexpr SDL_Color DarkBase{28, 28, 34, 255}, DarkLift{44, 44, 52, 255};
  const float phase = std::fmod(SDL_GetTicks(), PulseMs) / PulseMs;
  const float breath = 0.5f + 0.5f * std::sin(phase * 6.283185f);
  const auto below = [](Uint8 c, int by) { return Uint8(std::max(0, int(c) - by)); };
  const SDL_Color base = dark ? DarkBase : SDL_Color{below(on.r, 16), below(on.g, 18), below(on.b, 22), 255};
  const SDL_Color lift = dark ? DarkLift : on;
  rect(x, y, w, h, base, radius);
  rect(x, y, w, h, {lift.r, lift.g, lift.b, Uint8((dark ? 255 : 110) * breath)}, radius);
}
void Painter::skeletonCards(float x, float y, int count, int columns) {
  for (int n = 0; n < count; ++n) {
    const float cx = x + n % columns * 170, cy = y + n / columns * 240;
    skeleton(cx, cy, 150, 150, 14);
    skeleton(cx, cy + 162, 128, 16, 6);
    skeleton(cx, cy + 196, 72, 22, 7);
  }
}
constexpr SDL_Color ScrollHint{120, 117, 110, 110};
void Painter::scrollbar(float x, float y, float height, float offset,
                        float view, float content, bool active) {
  if (content <= view || height <= 0)
    return;
  // Only a position hint, not a draggable control: a thin faint pill, no
  // track, visible while the list moves and fading out after that.
  constexpr Uint32 HoldMs = 900, FadeMs = 300, LeftMs = 1500; // a gap this long: the screen was left
  const Uint32 now = SDL_GetTicks();
  auto [it, fresh] = scrolls_.try_emplace({int(x), int(y)}, ScrollSeen{offset, now, now});
  auto &seen = it->second;
  // Moved, or the screen was just (re)opened: show it again.
  if (!fresh && (seen.offset != offset || now - seen.drawnAt > LeftMs)) seen.movedAt = now;
  seen.offset = offset;
  seen.drawnAt = now;
  const Uint32 age = active ? 0 : now - seen.movedAt;
  if (age >= HoldMs + FadeMs)
    return;
  const float fade = age <= HoldMs ? 1.f : 1.f - float(age - HoldMs) / FadeMs;
  SDL_Color color = active ? Butter : ScrollHint;
  color.a = Uint8(color.a * fade);
  constexpr float Width = 4, MinThumb = 28;
  const float thumb = std::max(MinThumb, height * view / content);
  const float top = y + (height - thumb) * std::clamp(offset / (content - view), 0.f, 1.f);
  rect(x + 1, top, Width, thumb, color, 2);
}
void Painter::outline(float x, float y, float w, float h, int radius) {
  rect(x - 3, y - 3, w + 6, h + 6, Focus, radius + 3);
}
void Painter::focusRow(float x, float y, float w, float h, SDL_Color fill, int radius) {
  constexpr float Border = 3;
  rect(x, y, w, h, Focus, radius);
  rect(x + Border, y + Border, w - 2 * Border, h - 2 * Border, fill, std::max(0, radius - 3));
}
void Painter::ring(float x, float y, float w, float h, int radius) {
  rect(x - 8, y - 8, w + 16, h + 16, Focus, radius + 8);
  rect(x - 4, y - 4, w + 8, h + 8, Surface, radius + 4);
}
// Brand mark (brand/cookfoil-icon.svg) rasterized once, then the two-tone
// wordmark: "Cooking" in ink, "Foil" on a butter tag.
SDL_Texture *Painter::renderWordmark(int size) {
  float sx = 1, sy = 1;
  SDL_RenderGetScale(renderer, &sx, &sy);
  const int pixels = int(std::lround(size * sy));
#ifdef __SWITCH__
  const auto data = readFile("romfs:/fonts/Baloo2-ExtraBold.ttf");
  TTF_Font *font = data.empty() ? nullptr
                                : TTF_OpenFontRW(SDL_RWFromConstMem(data.data(), int(data.size())), 1, pixels);
#else
  TTF_Font *font = TTF_OpenFont("host/fonts/Baloo2-ExtraBold.ttf", pixels);
#endif
  if (!font)
    return nullptr;
  const std::string word = "cookingfoil.";
  const float tracking = -0.02f * pixels;
  // Lay the glyphs out first to size the strip.
  std::vector<std::pair<SDL_Surface *, int>> glyphs;
  float pen = 0;
  for (size_t i = 0; i < word.size(); ++i) {
    const bool dot = word[i] == '.';
    auto *glyph = TTF_RenderGlyph32_Blended(font, Uint32(word[i]), dot ? Butter : Ink);
    int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;
    TTF_GlyphMetrics32(font, Uint32(word[i]), &minx, &maxx, &miny, &maxy, &advance);
    if (glyph)
      glyphs.push_back({glyph, int(std::lround(pen))});
    pen += advance + (i + 1 < word.size() ? tracking : 0);
  }
  const int width = int(std::ceil(pen)) + 2, height = TTF_FontHeight(font);
  TTF_CloseFont(font);
  SDL_Texture *texture = nullptr;
  if (auto *strip = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32)) {
    // Glyphs may overlap by a pixel once tracked in; keep the stronger alpha
    // instead of blending, which would darken the edges.
    for (auto &[glyph, at] : glyphs) {
      auto *rgba = SDL_ConvertSurfaceFormat(glyph, SDL_PIXELFORMAT_RGBA32, 0);
      if (!rgba)
        continue;
      for (int row = 0; row < std::min(rgba->h, height); ++row) {
        auto *src = static_cast<Uint8 *>(rgba->pixels) + row * rgba->pitch;
        auto *dst = static_cast<Uint8 *>(strip->pixels) + row * strip->pitch;
        for (int col = 0; col < rgba->w && at + col < width; ++col) {
          if (at + col < 0)
            continue;
          Uint8 *d = dst + (at + col) * 4, *p = src + col * 4;
          if (p[3] > d[3])
            std::copy(p, p + 4, d);
        }
      }
      SDL_FreeSurface(rgba);
    }
    texture = SDL_CreateTextureFromSurface(renderer, strip);
    SDL_FreeSurface(strip);
  }
  for (auto &[glyph, at] : glyphs)
    SDL_FreeSurface(glyph);
  return texture;
}
SDL_Texture *Painter::renderMark(int size) {
  float sx = 1, sy = 1;
  SDL_RenderGetScale(renderer, &sx, &sy);
  const int pixels = int(std::lround(size * sx));
  const std::string svg =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(pixels) +
      "\" height=\"" + std::to_string(pixels) +
      "\" viewBox=\"0 0 40 40\"><rect x=\"4\" y=\"11\" width=\"32\" height=\"20\" rx=\"5\" "
      "fill=\"#FFC23D\"/><rect x=\"4\" y=\"11\" width=\"32\" height=\"7\" rx=\"3.5\" "
      "fill=\"#FFE08A\"/><rect x=\"4\" y=\"11\" width=\"32\" height=\"20\" rx=\"5\" "
      "fill=\"none\" stroke=\"#16161A\" stroke-width=\"2\"/></svg>";
  SDL_Texture *texture = nullptr;
  if (auto *surface = IMG_LoadSVG_RW(SDL_RWFromConstMem(svg.data(), int(svg.size())))) {
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
  }
  return texture;
}
void Painter::toggle(float x, float y, bool on) {
  constexpr float W = 52, H = 30, Knob = 24;
  rect(x, y, W, H, on ? Good : Line, int(H / 2));
  rect(on ? x + W - Knob - 3 : x + 3, y + 3, Knob, Knob, Surface, int(Knob / 2));
}
void Painter::logo(float x, float y) {
  constexpr int Mark = 40;
  if (!logo_)
    logo_ = renderMark(Mark);
  if (logo_) {
    const SDL_FRect dest{x, y, float(Mark), float(Mark)};
    SDL_RenderCopyF(renderer, logo_, nullptr, &dest);
  }
  // Wordmark: lowercase "cookingfoil" in Baloo 2 ExtraBold, tracked
  // -0.02em, then a butter period. Rendered once into a small texture and
  // the font closed again, so it costs no memory after the first frame.
  if (!wordmark_)
    wordmark_ = renderWordmark();
  if (wordmark_) {
    float sx = 1, sy = 1;
    SDL_RenderGetScale(renderer, &sx, &sy);
    int w = 0, h = 0;
    SDL_QueryTexture(wordmark_, nullptr, nullptr, &w, &h);
    const SDL_FRect dest{x + Mark + 10, y + Mark / 2.f - h / sy / 2, w / sx, h / sy};
    SDL_RenderCopyF(renderer, wordmark_, nullptr, &dest);
  }
}
namespace {
// The butter's hop, once a second: squashed on the ground, stretched as it
// leaves, up 34 at the top, a little squash as it lands. Rising slows,
// falling speeds up.
struct Hop {
  float rise, wide, tall;
};
Hop hop(float seconds) {
  struct Key {
    float at, rise, wide, tall;
  };
  static constexpr Key Keys[] = {{0, 0, 1.12f, .86f}, {.12f, 0, .94f, 1.08f}, {.5f, 34, 1, 1},
                                 {.88f, 0, 1.04f, .96f}, {1, 0, 1.12f, .86f}};
  const float t = seconds - std::floor(seconds);
  // The first hop leaves from standing: the screen opens on a take-off, not a
  // landing, so every landing it shows is one the start cue rings on.
  constexpr Key Standing{0, 0, 1, 1};
  for (int i = 0; i + 1 < int(std::size(Keys)); ++i) {
    const auto &a = i == 0 && seconds < 1 ? Standing : Keys[i], &b = Keys[i + 1];
    if (t > b.at) continue;
    float u = (t - a.at) / (b.at - a.at);
    u = i == 1 ? 1 - (1 - u) * (1 - u) : i == 2 ? u * u : u * u * (3 - 2 * u);
    return {a.rise + (b.rise - a.rise) * u, a.wide + (b.wide - a.wide) * u, a.tall + (b.tall - a.tall) * u};
  }
  return {0, Keys[0].wide, Keys[0].tall};
}
} // namespace
void Painter::splash(const std::string &caption, float progress, float seconds) {
  SDL_SetRenderDrawColor(renderer, Ground.r, Ground.g, Ground.b, 255);
  SDL_RenderClear(renderer);
  // Drawn larger than the header logo, from textures made for this screen
  // only: kept while it shows, freed by splashDone().
  constexpr int Mark = 120, Word = 44;
  float sx = 1, sy = 1;
  SDL_RenderGetScale(renderer, &sx, &sy);
  if (!splashMark_) splashMark_ = renderMark(Mark);
  if (!splashWord_) splashWord_ = renderWordmark(Word);
  if (splashMark_) {
    // The butter fills rows 11..31 of the mark's 40: it stands on y 307.
    constexpr float Floor = 307, Bottom = Mark * 31 / 40.f;
    const auto h = hop(seconds);
    const float lift = h.rise / 34, shadow = 1.1f - .5f * lift;
    rect(640 - 50 * shadow, Floor + 7 - 5 * shadow, 100 * shadow, 10 * shadow,
         {Ink.r, Ink.g, Ink.b, Uint8(36 - 22 * lift)}, int(5 * shadow));
    const float w = Mark * h.wide, tall = Mark * h.tall;
    const SDL_FRect dest{640 - w / 2, Floor - h.rise - Bottom * h.tall, w, tall};
    SDL_RenderCopyF(renderer, splashMark_, nullptr, &dest);
  }
  if (splashWord_) {
    int w = 0, h = 0;
    SDL_QueryTexture(splashWord_, nullptr, nullptr, &w, &h);
    const SDL_FRect dest{640 - w / sx / 2, 340, w / sx, h / sy};
    SDL_RenderCopyF(renderer, splashWord_, nullptr, &dest);
  }
  // A thin bar under the wordmark, filled by how far startup has come.
  constexpr float BarWidth = 280, BarTop = 420;
  rect(640 - BarWidth / 2, BarTop, BarWidth, 6, Line, 3);
  rect(640 - BarWidth / 2, BarTop, std::max(6.f, BarWidth * std::clamp(progress, 0.f, 1.f)), 6, Butter, 3);
  if (!caption.empty())
    fonts.text(caption, 640 - fonts.measure(caption, 18) / 2, 640, 18, Secondary); // what it is doing, at the bottom
  SDL_RenderPresent(renderer);
}
// Stroke icons (24 unit viewBox, 2.2 stroke), drawn for this app.
static const char *IconPaths[] = {
    R"(<path d="M3 11l9-7 9 7v9a1 1 0 0 1-1 1h-5v-6h-6v6H4a1 1 0 0 1-1-1z"/>)",
    R"(<circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/>)",
    R"(<rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/>)",
    R"(<path d="M21 12a9 9 0 1 1-2.6-6.4"/><path d="M21 4v5h-5"/>)",
    R"(<path d="M12 3v12M7 10l5 5 5-5M4 20h16"/>)",
    // Gear from Lucide (ISC); a ray-style gear read as a theme toggle.
    R"(<path d="M12.22 2h-.44a2 2 0 0 0-2 2v.18a2 2 0 0 1-1 1.73l-.43.25a2 2 0 0 1-2 0l-.15-.08a2 2 0 0 0-2.73.73l-.22.38a2 2 0 0 0 .73 2.73l.15.1a2 2 0 0 1 1 1.72v.51a2 2 0 0 1-1 1.74l-.15.09a2 2 0 0 0-.73 2.73l.22.38a2 2 0 0 0 2.73.73l.15-.08a2 2 0 0 1 2 0l.43.25a2 2 0 0 1 1 1.73V20a2 2 0 0 0 2 2h.44a2 2 0 0 0 2-2v-.18a2 2 0 0 1 1-1.73l.43-.25a2 2 0 0 1 2 0l.15.08a2 2 0 0 0 2.73-.73l.22-.39a2 2 0 0 0-.73-2.73l-.15-.08a2 2 0 0 1-1-1.74v-.5a2 2 0 0 1 1-1.74l.15-.09a2 2 0 0 0 .73-2.73l-.22-.38a2 2 0 0 0-2.73-.73l-.15.08a2 2 0 0 1-2 0l-.43-.25a2 2 0 0 1-1-1.73V4a2 2 0 0 0-2-2z"/><circle cx="12" cy="12" r="3"/>)",
    R"(<rect x="3" y="4" width="18" height="16" rx="2.5"/><path d="M8 12.5l3 3 5-6"/>)",
    R"(<path d="M15 18l-6-6 6-6"/>)",          // 7 chevron back
    R"(<path d="M5 12.5l4.5 4.5L19 7.5"/>)",   // 8 check mark
    R"(<path d="M9 18l6-6-6-6"/>)",            // 9 chevron forward
    // 10.. Lucide (ISC), same 24 viewBox and stroke as the menu icons.
    R"(<path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/>)",                // 10 sliders
    R"(<rect x="3" y="4" width="18" height="18" rx="2"/><path d="M16 2v4M8 2v4M3 10h18"/>)",            // 11 calendar
    R"(<path d="M3 21h18M5 21V7l7-4 7 4v14M9 10h1M14 10h1M9 14h1M14 14h1M9 18h1M14 18h1"/>)",           // 12 building
    R"(<path d="M16 21v-2a4 4 0 0 0-4-4H6a4 4 0 0 0-4 4v2"/><circle cx="9" cy="7" r="4"/><path d="M22 21v-2a4 4 0 0 0-3-3.87M16 3.13a4 4 0 0 1 0 7.75"/>)", // 13 users
    R"(<path d="M22 12H2M5.45 5.11L2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z"/><path d="M6 16h.01M10 16h.01"/>)", // 14 hard drive
    R"(<path d="M20.59 13.41l-7.17 7.17a2 2 0 0 1-2.83 0L2 12V2h10l8.59 8.59a2 2 0 0 1 0 2.82z"/><path d="M7 7h.01"/>)", // 15 tag
    R"(<path d="M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z"/><path d="M14 2v6h6"/>)",  // 16 file
    R"(<circle cx="12" cy="12" r="10"/><path d="M2 12h20M12 2a15.3 15.3 0 0 1 4 10 15.3 15.3 0 0 1-4 10 15.3 15.3 0 0 1-4-10 15.3 15.3 0 0 1 4-10z"/>)", // 17 globe
    R"(<path d="M4 9h16M4 15h16M10 3L8 21M16 3l-2 18"/>)",                                               // 18 hash
    R"(<path d="M6 3v12"/><circle cx="18" cy="6" r="3"/><circle cx="6" cy="18" r="3"/><path d="M18 9a9 9 0 0 1-9 9"/>)", // 19 branch (version)
    R"(<path d="M1 1l22 22M16.72 11.06A10.94 10.94 0 0 1 19 12.55M5 12.55a10.94 10.94 0 0 1 5.17-2.39M10.71 5.05A16 16 0 0 1 22.58 9M1.42 9a15.91 15.91 0 0 1 4.7-2.88M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01"/>)", // 20 wifi off
    R"(<rect x="2" y="2" width="20" height="8" rx="2"/><rect x="2" y="14" width="20" height="8" rx="2"/><path d="M6 6h.01M6 18h.01"/>)", // 21 server
    R"(<path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/><path d="M3.27 6.96L12 12.01l8.73-5.05M12 22.08V12"/>)", // 22 package
    R"(<circle cx="11" cy="11" r="8"/><path d="M21 21l-4.35-4.35M8.5 8.5l5 5M13.5 8.5l-5 5"/>)",        // 23 search x
    R"(<path d="M22 11.08V12a10 10 0 1 1-5.93-9.14"/><path d="M22 4L12 14.01l-3-3"/>)",                // 24 check circle
    R"(<circle cx="12" cy="12" r="10"/><path d="M12 8v4M12 16h.01"/>)",                                 // 25 alert
    R"(<path d="M6 22a2 2 0 0 1-2-2V6l4-4h10a2 2 0 0 1 2 2v16a2 2 0 0 1-2 2z"/><path d="M10 6v2M14 6v2M18 6v2"/>)", // 26 sd card
    R"(<rect x="2" y="6" width="20" height="12" rx="2"/><path d="M6 12h4M8 10v4M15 13h.01M18 11h.01"/>)", // 27 gamepad
    R"(<path d="M6 9H4.5a2.5 2.5 0 0 1 0-5H6M18 9h1.5a2.5 2.5 0 0 0 0-5H18M4 22h16M10 14.66V17c0 .55-.47.98-.97 1.21C7.85 18.75 7 20.24 7 22M14 14.66V17c0 .55.47.98.97 1.21C16.15 18.75 17 20.24 17 22M18 2H6v7a6 6 0 0 0 12 0V2z"/>)", // 28 trophy
    R"(<path d="M12 3l1.9 5.8L20 11l-6.1 2.2L12 19l-1.9-5.8L4 11l6.1-2.2z"/>)",                         // 29 sparkle
    R"(<circle cx="12" cy="12" r="10"/><path d="M12 6v6l4 2"/>)",                                       // 30 clock
    R"(<path d="M12 2l3.09 6.26L22 9.27l-5 4.87 1.18 6.88L12 17.77l-6.18 3.25L7 14.14 2 9.27l6.91-1.01z"/>)", // 31 star
    R"(<path d="M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.77-3.77a6 6 0 0 1-7.94 7.94l-6.91 6.91a2.12 2.12 0 0 1-3-3l6.91-6.91a6 6 0 0 1 7.94-7.94l-3.76 3.76z"/>)", // 32 wrench
    R"(<circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/>)", // 33 info
    R"(<path d="M20 13c0 5-3.5 7.5-7.66 8.95a1 1 0 0 1-.67-.01C7.5 20.5 4 18 4 13V6a1 1 0 0 1 1-1c2 0 4.5-1.2 6.24-2.72a1.17 1.17 0 0 1 1.52 0C14.51 3.81 17 5 19 5a1 1 0 0 1 1 1z"/><path d="M9 12l2 2 4-4"/>)", // 34 shield
    R"(<path d="M21 17a1 1 0 0 0-1-1H4a1 1 0 0 0-1 1v2a1 1 0 0 0 1 1h16a1 1 0 0 0 1-1Z"/><path d="M6 15v-2"/><path d="M12 15V9"/><circle cx="12" cy="6" r="3"/>)", // 35 joystick
    R"(<path d="M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z"/><path d="M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7"/><path d="M7 3v4a1 1 0 0 0 1 1h7"/>)", // 36 save
    R"(<path d="M3 6h18M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6M10 11v6M14 11v6"/>)", // 37 trash
    R"(<path d="M12 2v10"/><path d="M18.4 6.6a9 9 0 1 1-12.77.04"/>)",                                   // 38 power
    R"(<path d="M12 20h.01"/><path d="M2 8.82a15 15 0 0 1 20 0"/><path d="M5 12.86a10 10 0 0 1 14 0"/><path d="M8.5 16.43a5 5 0 0 1 7 0"/>)", // 39 wifi
    R"(<path d="M12 21V9M7 14l5-5 5 5M4 4h16"/>)", // 40 upload
    R"(<rect x="3" y="3" width="18" height="18" rx="2"/><circle cx="9" cy="9" r="2"/><path d="M21 15l-3.09-3.09a2 2 0 0 0-2.82 0L6 21"/>)", // 41 picture
    R"(<path d="M12 2L2 7l10 5 10-5-10-5z"/><path d="M2 17l10 5 10-5"/><path d="M2 12l10 5 10-5"/>)", // 42 layers (versions)
    R"(<path d="M19 21v-2a4 4 0 0 0-4-4H9a4 4 0 0 0-4 4v2"/><circle cx="12" cy="7" r="4"/>)", // 43 user (an account)
    R"(<path d="M11 5L6 9H2v6h4l5 4V5z"/><path d="M15.54 8.46a5 5 0 0 1 0 7.07M19.07 4.93a10 10 0 0 1 0 14.14"/>)", // 44 speaker
    R"(<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/><path d="M9 12l2 2 4-4"/>)", // 45 shield check (verified)
    R"(<path d="M3 12a9 9 0 0 1 9-9 9.75 9.75 0 0 1 6.74 2.74L21 8"/><path d="M21 3v5h-5"/><path d="M21 12a9 9 0 0 1-9 9 9.75 9.75 0 0 1-6.74-2.74L3 16"/><path d="M8 16H3v5"/>)", // 46 refresh
    R"(<path d="M7 10v12"/><path d="M15 5.88 14 10h5.83a2 2 0 0 1 1.92 2.56l-2.33 8A2 2 0 0 1 17.5 22H4a2 2 0 0 1-2-2v-8a2 2 0 0 1 2-2h2.76a2 2 0 0 0 1.79-1.11L12 2a3.13 3.13 0 0 1 3 3.88Z"/>)", // 47 thumbs up
    R"(<path d="M17 14V2"/><path d="M9 18.12 10 14H4.17a2 2 0 0 1-1.92-2.56l2.33-8A2 2 0 0 1 6.5 2H20a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-2.76a2 2 0 0 0-1.79 1.11L12 22a3.13 3.13 0 0 1-3-3.88Z"/>)", // 48 thumbs down
};
// Glyph-like marks read better a touch heavier than the menu icons.
// Marks read a touch heavier than outline icons.
int Painter::iconNamed(const std::string &name) {
  static const std::map<std::string, int> Names = {
      {"home", 0},     {"search", 1},    {"grid", 2},      {"refresh", 3},   {"download", 4},  {"gear", 5},
      {"installed", 6}, {"back", 7},     {"check", 8},     {"forward", 9},   {"filter", 10},   {"calendar", 11},
      {"building", 12}, {"users", 13},   {"inbox", 14},    {"tag", 15},      {"file", 16},     {"globe", 17},
      {"hash", 18},     {"branch", 19},  {"package", 22},  {"check-circle", 24}, {"alert", 25}, {"sd", 26},
      {"gamepad", 27},  {"trophy", 28},  {"sparkle", 29},  {"clock", 30},    {"star", 31},     {"wrench", 32},
      {"info", 33},     {"shield", 34},  {"joystick", 35}, {"save", 36},     {"upload", 40},   {"image", 41},
      {"layers", 42},   {"user", 43},    {"volume", 44},   {"shield-check", 45}, {"thumbs-up", 47}, {"thumbs-down", 48}};
  const auto found = Names.find(name);
  return found == Names.end() ? -1 : found->second;
}
const char *iconStroke(int kind) {
  return kind == 8 ? "3.2" : kind == 7 || kind == 9 ? "2.6" : "2.2";
}
void Painter::splashDone() {
  if (splashMark_) SDL_DestroyTexture(splashMark_);
  if (splashWord_) SDL_DestroyTexture(splashWord_);
  splashMark_ = splashWord_ = nullptr;
}
Painter::~Painter() {
  splashDone();
  if (logo_)
    SDL_DestroyTexture(logo_);
  if (wordmark_)
    SDL_DestroyTexture(wordmark_);
  for (const auto &pair : icons_)
    SDL_DestroyTexture(pair.second);
  for (const auto &pair : corners_)
    if (pair.second) SDL_DestroyTexture(pair.second);
  for (const auto &pair : flags_)
    if (pair.second) SDL_DestroyTexture(pair.second);
}
void Painter::icon(int kind, float x, float y, SDL_Color c, int size) {
  if (kind < 0 || kind >= int(std::size(IconPaths)))
    return;
  // One rasterized texture per kind and size, drawn 1:1 in physical pixels.
  const int key = kind * 1000 + size;
  auto found = icons_.find(key);
  if (found == icons_.end()) {
    float sx = 1, sy = 1;
    SDL_RenderGetScale(renderer, &sx, &sy);
    const int pixels = int(std::lround(size * sx));
    // Rasterized white at physical size, then tinted with color mod.
    const std::string svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(pixels) +
        "\" height=\"" + std::to_string(pixels) +
        "\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"#FFFFFF\" stroke-width=\"" +
        iconStroke(kind) + "\" stroke-linecap=\"round\" stroke-linejoin=\"round\">" +
        IconPaths[kind] + "</svg>";
    auto *surface = IMG_LoadSVG_RW(SDL_RWFromConstMem(svg.data(), int(svg.size())));
    SDL_Texture *texture = nullptr;
    if (surface) {
      texture = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_FreeSurface(surface);
    }
    if (texture)
      SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    found = icons_.emplace(key, texture).first;
  }
  if (!found->second)
    return;
  SDL_SetTextureColorMod(found->second, c.r, c.g, c.b);
  SDL_SetTextureAlphaMod(found->second, c.a);
  const SDL_FRect dest{x, y, float(size), float(size)};
  SDL_RenderCopyF(renderer, found->second, nullptr, &dest);
}
// "L R" is two buttons, drawn as two badges side by side.
float Painter::keyWidth(const std::string &name) {
  constexpr float Height = 32, Gap = 4;
  const auto space = name.find(' ');
  if (space != std::string::npos)
    return keyWidth(name.substr(0, space)) + Gap + keyWidth(name.substr(space + 1));
  return std::max(Height, fonts.measure(name, 18, true) + 20);
}
float Painter::key(const std::string &name, float x, float centerY, bool enabled) {
  constexpr float Height = 32, Gap = 4;
  constexpr SDL_Color DisabledKey{176, 174, 167, 255};
  if (const auto space = name.find(' '); space != std::string::npos) {
    const float left = key(name.substr(0, space), x, centerY, enabled);
    return left + Gap + key(name.substr(space + 1), x + left + Gap, centerY, enabled);
  }
  const float w = keyWidth(name);
  rect(x, centerY - Height / 2, w, Height, enabled ? Ink : DisabledKey, int(Height / 2));
  fonts.middle(name, x + (w - fonts.measure(name, 18, true)) / 2, centerY, 18,
               Surface, true);
  return w;
}
float Painter::keyLabel(const std::string &name, const std::string &label,
                        float x, float centerY, int size, SDL_Color color,
                        bool bold, bool enabled) {
  x += key(name, x, centerY, enabled) + 10;
  return fonts.middle(label, x, centerY, size, enabled ? color : SDL_Color{150, 148, 142, 255}, bold);
}
float Painter::keyLabelWidth(const std::string &name, const std::string &label,
                             int size, bool bold) {
  return keyWidth(name) + 10 + fonts.measure(label, size, bold);
}
namespace {
constexpr float ChipHeight = 28, ChipBadge = 20;
constexpr int ChipKeySize = 12, ChipLabelSize = 15;
} // namespace
float Painter::keyChipWidth(const std::string &name, const std::string &label) {
  const float badge = std::max(ChipBadge, fonts.measure(name, ChipKeySize, true) + 10);
  return 4 + badge + 7 + fonts.measure(label, ChipLabelSize, true) + 12;
}
float Painter::keyChip(const std::string &name, const std::string &label, float right,
                       float centerY) {
  constexpr float Height = ChipHeight, Badge = ChipBadge;
  constexpr int KeySize = ChipKeySize, LabelSize = ChipLabelSize;
  const float badge = std::max(Badge, fonts.measure(name, KeySize, true) + 10);
  const float w = keyChipWidth(name, label);
  const float x = right - w;
  rect(x, centerY - Height / 2, w, Height, Surface, int(Height / 2));
  rect(x + 4, centerY - Badge / 2, badge, Badge, Ink, int(Badge / 2));
  fonts.middle(name, x + 4 + (badge - fonts.measure(name, KeySize, true)) / 2, centerY, KeySize, Surface, true);
  fonts.middle(label, x + 4 + badge + 7, centerY, LabelSize, Secondary, true);
  return w;
}
float Painter::verifiedPill(float x, float y, bool dark, int size) {
  // The shop's green: #3fbf83 on dark, #1e8e5a on light, filled at 14% and
  // edged at 45% of it. On the page the two are mixed into its ground; on
  // the detail's picture band only the fill, see-through like its pills.
  const SDL_Color green = dark ? SDL_Color{63, 191, 131, 255} : Good;
  const auto over = [&](float t) {
    const auto mix = [&](Uint8 c, Uint8 g) { return Uint8(std::lround(g + (c - g) * t)); };
    return SDL_Color{mix(green.r, Ground.r), mix(green.g, Ground.g), mix(green.b, Ground.b), 255};
  };
  const std::string text = tr("Verified|badge");
  const float h = dark ? 35 : 26, mark = dark ? 18 : 15, pad = dark ? 12 : 7;
  const int radius = dark ? 17 : 7;
  const float w = pad + mark + 4 + fonts.measure(text, size, true) + pad;
  if (dark)
    rect(x, y, w, h, {green.r, green.g, green.b, 64}, radius);
  else {
    rect(x, y, w, h, over(0.45f), radius);
    rect(x + 1, y + 1, w - 2, h - 2, over(0.14f), radius - 1);
  }
  icon(45, x + pad, y + (h - mark) / 2, green, int(mark));
  fonts.middle(text, x + pad + mark + 4, y + h / 2.f, size, green, true);
  return w;
}
float Painter::pill(const std::string &s, float x, float y, bool dark,
                    int size) {
  const auto w = fonts.measure(s, size, dark ? false : true) + (dark ? 28 : 18);
  rect(x, y, w, dark ? 35 : 26, dark ? SDL_Color{255, 255, 255, 41} : Line,
       dark ? 17 : 7);
  fonts.middle(s, x + (dark ? 14 : 9), y + (dark ? 35 : 26) / 2.f, size,
               dark ? Surface : Ink, !dark);
  return w;
}
} // namespace foil::ui
