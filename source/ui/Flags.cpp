// Flags for the language popup, in the order of foil::Language. Plain SVG
// (no <use>, which nanosvg lacks). South Korea follows the Wikimedia
// construction; English uses the US flag, Portuguese Brazil's. Traditional
// Chinese has no flag (null) and gets a chip with its script's name.
#include "ui/Painter.hpp"
#include <SDL2/SDL_image.h>
#include <cctype>
#include <cmath>
#include <map>
#include <utility>
#include <algorithm>
#include <string>
namespace foil::ui {
namespace {
const char *const BrazilSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 30 20"><rect width="30" height="20" fill="#009C3B"/><path fill="#FFDF00" d="M15 2L28 10L15 18L2 10Z"/><circle cx="15" cy="10" r="4.6" fill="#002776"/></svg>)svg";
const char *const FlagSvg[] = {
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="-36 -24 72 48"><rect x="-36" y="-24" width="72" height="48" fill="#fff"/><g transform="rotate(-56.3099325)"><path d="M-6-25H6M-6-22H6M-6-19H6M-6 19H6M-6 22H6M-6 25H6" stroke="#000" stroke-width="2"/><path stroke="#fff" stroke-width="1" d="M0,17v10"/><circle fill="#cd2e3a" r="12"/><path fill="#0047a0" d="M0-12A6,6 0 0 0 0,0A6,6 0 0 1 0,12A12,12 0 0,1 0-12Z"/></g><g transform="rotate(-123.6900675)"><path d="M-6-25H6M-6-22H6M-6-19H6M-6 19H6M-6 22H6M-6 25H6" stroke="#000" stroke-width="2"/><path stroke="#fff" stroke-width="1" d="M0-23.5v3M0,17v3.5M0,23.5v3"/></g></svg>)svg",
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 190 100" preserveAspectRatio="none"><rect width="190" height="100" fill="#fff"/><rect y="0.000" width="190" height="7.692" fill="#B22234"/><rect y="15.385" width="190" height="7.692" fill="#B22234"/><rect y="30.769" width="190" height="7.692" fill="#B22234"/><rect y="46.154" width="190" height="7.692" fill="#B22234"/><rect y="61.538" width="190" height="7.692" fill="#B22234"/><rect y="76.923" width="190" height="7.692" fill="#B22234"/><rect y="92.308" width="190" height="7.692" fill="#B22234"/><rect width="76" height="53.846" fill="#3C3B6E"/><path fill="#fff" d="M6.33,2.78 L6.92,4.58 L8.81,4.58 L7.28,5.69 L7.86,7.49 L6.33,6.38 L4.81,7.49 L5.39,5.69 L3.86,4.58 L5.75,4.58Z M19.00,2.78 L19.58,4.58 L21.47,4.58 L19.94,5.69 L20.53,7.49 L19.00,6.38 L17.47,7.49 L18.06,5.69 L16.53,4.58 L18.42,4.58Z M31.67,2.78 L32.25,4.58 L34.14,4.58 L32.61,5.69 L33.19,7.49 L31.67,6.38 L30.14,7.49 L30.72,5.69 L29.19,4.58 L31.08,4.58Z M44.33,2.78 L44.92,4.58 L46.81,4.58 L45.28,5.69 L45.86,7.49 L44.33,6.38 L42.81,7.49 L43.39,5.69 L41.86,4.58 L43.75,4.58Z M57.00,2.78 L57.58,4.58 L59.47,4.58 L57.94,5.69 L58.53,7.49 L57.00,6.38 L55.47,7.49 L56.06,5.69 L54.53,4.58 L56.42,4.58Z M69.67,2.78 L70.25,4.58 L72.14,4.58 L70.61,5.69 L71.19,7.49 L69.67,6.38 L68.14,7.49 L68.72,5.69 L67.19,4.58 L69.08,4.58Z M12.67,8.17 L13.25,9.97 L15.14,9.97 L13.61,11.08 L14.19,12.87 L12.67,11.76 L11.14,12.87 L11.72,11.08 L10.19,9.97 L12.08,9.97Z M25.33,8.17 L25.92,9.97 L27.81,9.97 L26.28,11.08 L26.86,12.87 L25.33,11.76 L23.81,12.87 L24.39,11.08 L22.86,9.97 L24.75,9.97Z M38.00,8.17 L38.58,9.97 L40.47,9.97 L38.94,11.08 L39.53,12.87 L38.00,11.76 L36.47,12.87 L37.06,11.08 L35.53,9.97 L37.42,9.97Z M50.67,8.17 L51.25,9.97 L53.14,9.97 L51.61,11.08 L52.19,12.87 L50.67,11.76 L49.14,12.87 L49.72,11.08 L48.19,9.97 L50.08,9.97Z M63.33,8.17 L63.92,9.97 L65.81,9.97 L64.28,11.08 L64.86,12.87 L63.33,11.76 L61.81,12.87 L62.39,11.08 L60.86,9.97 L62.75,9.97Z M6.33,13.55 L6.92,15.35 L8.81,15.35 L7.28,16.46 L7.86,18.26 L6.33,17.15 L4.81,18.26 L5.39,16.46 L3.86,15.35 L5.75,15.35Z M19.00,13.55 L19.58,15.35 L21.47,15.35 L19.94,16.46 L20.53,18.26 L19.00,17.15 L17.47,18.26 L18.06,16.46 L16.53,15.35 L18.42,15.35Z M31.67,13.55 L32.25,15.35 L34.14,15.35 L32.61,16.46 L33.19,18.26 L31.67,17.15 L30.14,18.26 L30.72,16.46 L29.19,15.35 L31.08,15.35Z M44.33,13.55 L44.92,15.35 L46.81,15.35 L45.28,16.46 L45.86,18.26 L44.33,17.15 L42.81,18.26 L43.39,16.46 L41.86,15.35 L43.75,15.35Z M57.00,13.55 L57.58,15.35 L59.47,15.35 L57.94,16.46 L58.53,18.26 L57.00,17.15 L55.47,18.26 L56.06,16.46 L54.53,15.35 L56.42,15.35Z M69.67,13.55 L70.25,15.35 L72.14,15.35 L70.61,16.46 L71.19,18.26 L69.67,17.15 L68.14,18.26 L68.72,16.46 L67.19,15.35 L69.08,15.35Z M12.67,18.94 L13.25,20.73 L15.14,20.74 L13.61,21.85 L14.19,23.64 L12.67,22.53 L11.14,23.64 L11.72,21.85 L10.19,20.74 L12.08,20.73Z M25.33,18.94 L25.92,20.73 L27.81,20.74 L26.28,21.85 L26.86,23.64 L25.33,22.53 L23.81,23.64 L24.39,21.85 L22.86,20.74 L24.75,20.73Z M38.00,18.94 L38.58,20.73 L40.47,20.74 L38.94,21.85 L39.53,23.64 L38.00,22.53 L36.47,23.64 L37.06,21.85 L35.53,20.74 L37.42,20.73Z M50.67,18.94 L51.25,20.73 L53.14,20.74 L51.61,21.85 L52.19,23.64 L50.67,22.53 L49.14,23.64 L49.72,21.85 L48.19,20.74 L50.08,20.73Z M63.33,18.94 L63.92,20.73 L65.81,20.74 L64.28,21.85 L64.86,23.64 L63.33,22.53 L61.81,23.64 L62.39,21.85 L60.86,20.74 L62.75,20.73Z M6.33,24.32 L6.92,26.12 L8.81,26.12 L7.28,27.23 L7.86,29.03 L6.33,27.92 L4.81,29.03 L5.39,27.23 L3.86,26.12 L5.75,26.12Z M19.00,24.32 L19.58,26.12 L21.47,26.12 L19.94,27.23 L20.53,29.03 L19.00,27.92 L17.47,29.03 L18.06,27.23 L16.53,26.12 L18.42,26.12Z M31.67,24.32 L32.25,26.12 L34.14,26.12 L32.61,27.23 L33.19,29.03 L31.67,27.92 L30.14,29.03 L30.72,27.23 L29.19,26.12 L31.08,26.12Z M44.33,24.32 L44.92,26.12 L46.81,26.12 L45.28,27.23 L45.86,29.03 L44.33,27.92 L42.81,29.03 L43.39,27.23 L41.86,26.12 L43.75,26.12Z M57.00,24.32 L57.58,26.12 L59.47,26.12 L57.94,27.23 L58.53,29.03 L57.00,27.92 L55.47,29.03 L56.06,27.23 L54.53,26.12 L56.42,26.12Z M69.67,24.32 L70.25,26.12 L72.14,26.12 L70.61,27.23 L71.19,29.03 L69.67,27.92 L68.14,29.03 L68.72,27.23 L67.19,26.12 L69.08,26.12Z M12.67,29.71 L13.25,31.50 L15.14,31.50 L13.61,32.61 L14.19,34.41 L12.67,33.30 L11.14,34.41 L11.72,32.61 L10.19,31.50 L12.08,31.50Z M25.33,29.71 L25.92,31.50 L27.81,31.50 L26.28,32.61 L26.86,34.41 L25.33,33.30 L23.81,34.41 L24.39,32.61 L22.86,31.50 L24.75,31.50Z M38.00,29.71 L38.58,31.50 L40.47,31.50 L38.94,32.61 L39.53,34.41 L38.00,33.30 L36.47,34.41 L37.06,32.61 L35.53,31.50 L37.42,31.50Z M50.67,29.71 L51.25,31.50 L53.14,31.50 L51.61,32.61 L52.19,34.41 L50.67,33.30 L49.14,34.41 L49.72,32.61 L48.19,31.50 L50.08,31.50Z M63.33,29.71 L63.92,31.50 L65.81,31.50 L64.28,32.61 L64.86,34.41 L63.33,33.30 L61.81,34.41 L62.39,32.61 L60.86,31.50 L62.75,31.50Z M6.33,35.09 L6.92,36.89 L8.81,36.89 L7.28,38.00 L7.86,39.80 L6.33,38.69 L4.81,39.80 L5.39,38.00 L3.86,36.89 L5.75,36.89Z M19.00,35.09 L19.58,36.89 L21.47,36.89 L19.94,38.00 L20.53,39.80 L19.00,38.69 L17.47,39.80 L18.06,38.00 L16.53,36.89 L18.42,36.89Z M31.67,35.09 L32.25,36.89 L34.14,36.89 L32.61,38.00 L33.19,39.80 L31.67,38.69 L30.14,39.80 L30.72,38.00 L29.19,36.89 L31.08,36.89Z M44.33,35.09 L44.92,36.89 L46.81,36.89 L45.28,38.00 L45.86,39.80 L44.33,38.69 L42.81,39.80 L43.39,38.00 L41.86,36.89 L43.75,36.89Z M57.00,35.09 L57.58,36.89 L59.47,36.89 L57.94,38.00 L58.53,39.80 L57.00,38.69 L55.47,39.80 L56.06,38.00 L54.53,36.89 L56.42,36.89Z M69.67,35.09 L70.25,36.89 L72.14,36.89 L70.61,38.00 L71.19,39.80 L69.67,38.69 L68.14,39.80 L68.72,38.00 L67.19,36.89 L69.08,36.89Z M12.67,40.48 L13.25,42.27 L15.14,42.27 L13.61,43.38 L14.19,45.18 L12.67,44.07 L11.14,45.18 L11.72,43.38 L10.19,42.27 L12.08,42.27Z M25.33,40.48 L25.92,42.27 L27.81,42.27 L26.28,43.38 L26.86,45.18 L25.33,44.07 L23.81,45.18 L24.39,43.38 L22.86,42.27 L24.75,42.27Z M38.00,40.48 L38.58,42.27 L40.47,42.27 L38.94,43.38 L39.53,45.18 L38.00,44.07 L36.47,45.18 L37.06,43.38 L35.53,42.27 L37.42,42.27Z M50.67,40.48 L51.25,42.27 L53.14,42.27 L51.61,43.38 L52.19,45.18 L50.67,44.07 L49.14,45.18 L49.72,43.38 L48.19,42.27 L50.08,42.27Z M63.33,40.48 L63.92,42.27 L65.81,42.27 L64.28,43.38 L64.86,45.18 L63.33,44.07 L61.81,45.18 L62.39,43.38 L60.86,42.27 L62.75,42.27Z M6.33,45.86 L6.92,47.66 L8.81,47.66 L7.28,48.77 L7.86,50.56 L6.33,49.45 L4.81,50.56 L5.39,48.77 L3.86,47.66 L5.75,47.66Z M19.00,45.86 L19.58,47.66 L21.47,47.66 L19.94,48.77 L20.53,50.56 L19.00,49.45 L17.47,50.56 L18.06,48.77 L16.53,47.66 L18.42,47.66Z M31.67,45.86 L32.25,47.66 L34.14,47.66 L32.61,48.77 L33.19,50.56 L31.67,49.45 L30.14,50.56 L30.72,48.77 L29.19,47.66 L31.08,47.66Z M44.33,45.86 L44.92,47.66 L46.81,47.66 L45.28,48.77 L45.86,50.56 L44.33,49.45 L42.81,50.56 L43.39,48.77 L41.86,47.66 L43.75,47.66Z M57.00,45.86 L57.58,47.66 L59.47,47.66 L57.94,48.77 L58.53,50.56 L57.00,49.45 L55.47,50.56 L56.06,48.77 L54.53,47.66 L56.42,47.66Z M69.67,45.86 L70.25,47.66 L72.14,47.66 L70.61,48.77 L71.19,50.56 L69.67,49.45 L68.14,50.56 L68.72,48.77 L67.19,47.66 L69.08,47.66Z"/></svg>)svg",
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 900 600"><rect width="900" height="600" fill="#fff"/><circle cx="450" cy="300" r="180" fill="#BC002D"/></svg>)svg",
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 30 20"><rect width="30" height="20" fill="#EE1C25"/><path fill="#FFFF00" d="M5.00,2.00 L5.67,4.07 L7.85,4.07 L6.09,5.35 L6.76,7.43 L5.00,6.15 L3.24,7.43 L3.91,5.35 L2.15,4.07 L4.33,4.07Z M9.14,2.51 L9.62,1.97 L9.25,1.34 L9.91,1.63 L10.39,1.08 L10.33,1.80 L11.00,2.09 L10.29,2.25 L10.22,2.97 L9.85,2.35Z M11.01,4.14 L11.66,3.82 L11.56,3.10 L12.07,3.62 L12.72,3.30 L12.38,3.95 L12.88,4.47 L12.17,4.34 L11.83,4.99 L11.73,4.27Z M11.04,6.73 L11.76,6.70 L11.96,6.00 L12.21,6.68 L12.94,6.66 L12.37,7.10 L12.62,7.79 L12.01,7.38 L11.44,7.83 L11.64,7.13Z M9.22,8.38 L9.90,8.63 L10.35,8.06 L10.32,8.79 L11.00,9.05 L10.30,9.24 L10.26,9.96 L9.87,9.36 L9.16,9.55 L9.62,8.98Z"/></svg>)svg",
    nullptr, // traditional Chinese
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 3 2" preserveAspectRatio="none"><rect width="3" height="2" fill="#AA151B"/><rect y="0.5" width="3" height="1" fill="#F1BF00"/></svg>)svg", // Spanish
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 3 2" preserveAspectRatio="none"><rect x="0" width="1" height="2" fill="#002654"/><rect x="1" width="1" height="2" fill="#FFFFFF"/><rect x="2" width="1" height="2" fill="#ED2939"/></svg>)svg", // French
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 5 3" preserveAspectRatio="none"><rect y="0" width="5" height="1" fill="#000000"/><rect y="1" width="5" height="1" fill="#DD0000"/><rect y="2" width="5" height="1" fill="#FFCE00"/></svg>)svg", // German
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 3 2" preserveAspectRatio="none"><rect x="0" width="1" height="2" fill="#009246"/><rect x="1" width="1" height="2" fill="#FFFFFF"/><rect x="2" width="1" height="2" fill="#CE2B37"/></svg>)svg", // Italian
    BrazilSvg, // Portuguese
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 5 3" preserveAspectRatio="none"><rect y="0" width="5" height="1" fill="#AE1C28"/><rect y="1" width="5" height="1" fill="#FFFFFF"/><rect y="2" width="5" height="1" fill="#21468B"/></svg>)svg", // Dutch
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 5 3" preserveAspectRatio="none"><rect y="0" width="5" height="1" fill="#FFFFFF"/><rect y="1" width="5" height="1" fill="#0039A6"/><rect y="2" width="5" height="1" fill="#D52B1E"/></svg>)svg", // Russian
};
static_assert(sizeof FlagSvg / sizeof *FlagSvg == LanguageCount, "a flag per language");
// Regions beyond the languages' flags: Europe's ring of stars and Brazil.
const char *const RegionSvg[] = {
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 30 20"><rect width="30" height="20" fill="#003399"/><g fill="#FFCC00"><circle cx="15" cy="4" r="1"/><circle cx="18" cy="4.8" r="1"/><circle cx="20.2" cy="7" r="1"/><circle cx="21" cy="10" r="1"/><circle cx="20.2" cy="13" r="1"/><circle cx="18" cy="15.2" r="1"/><circle cx="15" cy="16" r="1"/><circle cx="12" cy="15.2" r="1"/><circle cx="9.8" cy="13" r="1"/><circle cx="9" cy="10" r="1"/><circle cx="9.8" cy="7" r="1"/><circle cx="12" cy="4.8" r="1"/></g></svg>)svg",
    BrazilSvg,
};
// One SVG rasterized at the drawn size, kept under `key`.
SDL_Texture *rasterize(SDL_Renderer *renderer, std::map<std::string, SDL_Texture *> &cache, const std::string &key,
                       std::string svg, float w, float h) {
  float sx = 1, sy = 1;
  SDL_RenderGetScale(renderer, &sx, &sy);
  const int pw = int(std::lround(w * sx)), ph = int(std::lround(h * sy));
  const auto sized = key + ":" + std::to_string(pw) + "x" + std::to_string(ph);
  auto found = cache.find(sized);
  if (found != cache.end()) return found->second;
  svg.insert(svg.find("<svg") + 4, " width=\"" + std::to_string(pw) + "\" height=\"" + std::to_string(ph) + "\"");
  SDL_Texture *texture = nullptr;
  if (auto *surface = IMG_LoadSVG_RW(SDL_RWFromConstMem(svg.data(), int(svg.size())))) {
    texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_FreeSurface(surface);
  }
  return cache.emplace(sized, texture).first->second;
}
} // namespace
void Painter::flag(int language, float x, float y, float w, float h) {
  if (language < 0 || language >= int(sizeof FlagSvg / sizeof *FlagSvg)) return;
  if (!FlagSvg[language]) {
    const SDL_Color chip{22, 22, 26, 210}, white{255, 255, 255, 255};
    rect(x - 1, y - 1, w + 2, h + 2, chip, 4);
    const int size = std::max(9, int(h * 0.6f));
    const char *script = "繁"; // i18n: as is (the script's own name)
    fonts.middle(script, x + (w - fonts.measure(script, size, true)) / 2, y + h / 2, size, white, true);
    return;
  }
  float sx = 1, sy = 1;
  SDL_RenderGetScale(renderer, &sx, &sy);
  const int pw = int(std::lround(w * sx)), ph = int(std::lround(h * sy));
  const auto key = std::to_string(language) + ":" + std::to_string(pw) + "x" + std::to_string(ph);
  auto found = flags_.find(key);
  if (found == flags_.end()) {
    std::string svg = FlagSvg[language];
    // Sized here so nanosvg rasterizes at the drawn size, not the viewBox's.
    svg.insert(svg.find("<svg") + 4, " width=\"" + std::to_string(pw) + "\" height=\"" + std::to_string(ph) + "\"");
    SDL_Texture *texture = nullptr;
    if (auto *surface = IMG_LoadSVG_RW(SDL_RWFromConstMem(svg.data(), int(svg.size())))) {
      texture = SDL_CreateTextureFromSurface(renderer, surface);
      SDL_FreeSurface(surface);
    }
    found = flags_.emplace(key, texture).first;
  }
  if (!found->second) return;
  rect(x - 1, y - 1, w + 2, h + 2, Line, 4); // a hairline so white flags keep their edge
  const SDL_FRect dest{x, y, w, h};
  SDL_RenderCopyF(renderer, found->second, nullptr, &dest);
}
} // namespace foil::ui
namespace foil::ui {
void Painter::languageChip(const std::string &code, float x, float y, float w, float h) {
  const SDL_Color chip{22, 22, 26, 210}, white{255, 255, 255, 255};
  rect(x - 1, y - 1, w + 2, h + 2, chip, 4);
  std::string label = code.substr(0, code.find('-'));
  for (auto &c : label) c = char(std::toupper(static_cast<unsigned char>(c)));
  const int size = std::max(9, int(h * 0.5f));
  fonts.middle(label, x + std::max(2.f, (w - fonts.measure(label, size, true)) / 2), y + h / 2, size, white, true, w - 4);
}
void Painter::region(const std::string &code, float x, float y, float w, float h) {
  static const std::pair<const char *, int> Languages[] = {{"kr", 0}, {"us", 1}, {"jp", 2}};
  for (const auto &[name, language] : Languages)
    if (code == name) return flag(language, x, y, w, h);
  if (code == "eu" || code == "br") {
    auto *texture = rasterize(renderer, flags_, "region-" + code, RegionSvg[code == "eu" ? 0 : 1], w, h);
    if (!texture) return;
    rect(x - 1, y - 1, w + 2, h + 2, Line, 4);
    const SDL_FRect dest{x, y, w, h};
    SDL_RenderCopyF(renderer, texture, nullptr, &dest);
    return;
  }
  // No one flag: a dark chip the flag's size, a globe for the world and
  // the name for Asia.
  if (code != "world" && code != "asia") return;
  const SDL_Color chip{22, 22, 26, 210}, white{255, 255, 255, 255};
  rect(x - 1, y - 1, w + 2, h + 2, chip, 4);
  if (code == "world") {
    const int size = int(h) - 2;
    icon(17, x + (w - size) / 2, y + 1, white, size);
  } else {
    const int size = std::max(9, int(h * 0.5f));
    fonts.middle("ASIA", x + (w - fonts.measure("ASIA", size, true)) / 2, y + h / 2, size, white, true);
  }
}
} // namespace foil::ui
