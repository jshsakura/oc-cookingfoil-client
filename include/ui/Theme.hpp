#pragma once
#include <SDL2/SDL.h>
#include <iterator>
#include <string_view>
namespace foil::ui {
constexpr int Width = 1280, Height = 720, Sidebar = 232, Footer = 60;
// A detail page's top line: the way back on the left, its chips on the right.
constexpr float BackLine = 30;
// The band's chips (votes, 업적, admin) sit a little lower, off the screen's
// corner where a finger hardly lands; their touch reaches up to the edge.
constexpr float ChipLine = 42;
// A game card: its labels just under the name, and the step from one row
// of cards to the next in a grid.
constexpr float CardLabelTop = 186, CardRowStep = 226;
constexpr float BandRight = 1248; // the band's chips end here
// The page's colors, light or dark. They change while the app runs, so the
// names below are references into the palette in use, not constants.
struct Palette {
  SDL_Color ground, surface, ink, secondary, line, okay, good, pending, bad;
  // Butter, red and green washed into the page (a chosen row, a warning
  // card, a finished part); greyed ink; a pressed button's shade.
  SDL_Color cream, redTint, greenTint, muted, press;
  bool dark;
  // The menu column's fill when it differs from the cards'; unset (alpha 0)
  // it is the surface.
  SDL_Color side{};
};
constexpr Palette LightPalette{{242, 241, 238, 255}, {255, 255, 255, 255}, {22, 22, 26, 255},
                               {85, 85, 94, 255},    {226, 224, 218, 255}, {20, 99, 63, 255},
                               {30, 142, 90, 255},   {196, 124, 0, 255},   {204, 62, 52, 255},
                               {255, 241, 204, 255}, {253, 226, 226, 255}, {227, 242, 234, 255},
                               {176, 174, 167, 255}, {0, 0, 0, 36},        false};
constexpr Palette DarkPalette{{18, 18, 22, 255},     {34, 34, 40, 255},    {236, 236, 240, 255},
                              {160, 160, 170, 255},  {56, 56, 64, 255},    {92, 204, 148, 255},
                              {63, 191, 131, 255},   {232, 160, 48, 255},  {240, 104, 92, 255},
                              {72, 60, 30, 255},     {80, 38, 38, 255},    {30, 62, 46, 255},
                              {104, 104, 112, 255},  {255, 255, 255, 30},  true};
// Navy takes Nord's colors and purple Catppuccin Mocha's, green Everforest's;
// pink is a light page of its own. Their reds are brightened where errors
// read too dim, and every text color keeps at least 4:1 against its page.
constexpr Palette NavyPalette{{46, 52, 64, 255}, {62, 70, 88, 255}, {236, 239, 244, 255}, {196, 204, 218, 255}, {76, 86, 106, 255}, {163, 190, 140, 255}, {163, 190, 140, 255}, {235, 203, 139, 255}, {226, 128, 136, 255}, {93, 90, 83, 255}, {82, 63, 74, 255}, {75, 86, 83, 255}, {118, 128, 148, 255}, {255, 255, 255, 30}, true};
constexpr Palette PurplePalette{{30, 30, 46, 255}, {49, 50, 68, 255}, {205, 214, 244, 255}, {166, 173, 200, 255}, {69, 71, 90, 255}, {166, 227, 161, 255}, {166, 227, 161, 255}, {250, 179, 135, 255}, {243, 139, 168, 255}, {85, 79, 78, 255}, {83, 57, 77, 255}, {64, 79, 75, 255}, {108, 112, 134, 255}, {255, 255, 255, 30}, true};
constexpr Palette GreenPalette{{45, 53, 59, 255}, {60, 72, 79, 255}, {211, 198, 170, 255}, {165, 175, 160, 255}, {76, 88, 94, 255}, {167, 192, 128, 255}, {167, 192, 128, 255}, {219, 188, 127, 255}, {236, 134, 136, 255}, {89, 87, 76, 255}, {91, 71, 76, 255}, {75, 88, 76, 255}, {122, 132, 120, 255}, {255, 255, 255, 30}, true};
constexpr Palette PinkPalette{{249, 231, 236, 255}, {255, 247, 249, 255}, {74, 52, 66, 255}, {116, 90, 104, 255}, {238, 210, 220, 255}, {20, 99, 63, 255}, {30, 142, 90, 255}, {176, 110, 0, 255}, {196, 48, 72, 255}, {255, 236, 204, 255}, {252, 214, 222, 255}, {222, 240, 230, 255}, {186, 160, 172, 255}, {60, 0, 30, 30}, false, {242, 210, 222, 255}};
// In ThemeNames' order (core/State.hpp).
constexpr const Palette *ThemePalettes[] = {&LightPalette, &DarkPalette,  &NavyPalette,
                                            &PurplePalette, &GreenPalette, &PinkPalette};
inline Palette palette = LightPalette;
inline void usePalette(const Palette &next) { palette = next; }
inline const SDL_Color &Ground = palette.ground, &Surface = palette.surface;
inline const SDL_Color &Ink = palette.ink, &Secondary = palette.secondary;
inline const SDL_Color &Line = palette.line;
inline const SDL_Color &Cream = palette.cream, &RedTint = palette.redTint, &GreenTint = palette.greenTint;
inline const SDL_Color &Muted = palette.muted, &Press = palette.press;
inline SDL_Color sideColor() { return palette.side.a ? palette.side : palette.surface; }
// Colors that stay put in either palette: white and near-black over a
// picture or a fixed dark band, and the ink drawn on butter.
constexpr SDL_Color White{255, 255, 255, 255}, Night{22, 22, 26, 255}, OnButter = Night;
constexpr SDL_Color Butter{255, 194, 61, 255};
// Butter's stripes on a button that warns before it acts.
constexpr SDL_Color ButterStripe{255, 181, 42, 255};
// Focus ring uses the brand butter; its inner band of the surface separates
// it from butter fills.
constexpr SDL_Color Focus = Butter;
inline const SDL_Color &Okay = palette.okay;
// Connection states: connected, in progress, failed.
inline const SDL_Color &Good = palette.good, &Pending = palette.pending, &Bad = palette.bad;
// A server's dot, apart from the connection states' red, orange and grey:
// green, blue, purple, teal, pink, indigo, lime, navy, sky, magenta, brown, mint.
constexpr SDL_Color ServerColors[] = {{30, 142, 90, 255},  {52, 120, 246, 255}, {142, 92, 230, 255}, {20, 163, 168, 255},
                                      {224, 82, 156, 255}, {90, 103, 216, 255}, {120, 170, 40, 255}, {35, 55, 120, 255},
                                      {40, 165, 225, 255}, {178, 60, 190, 255}, {132, 88, 58, 255},  {70, 196, 150, 255}};
// The dot of the server at `place` in the list, chosen as `color` (-1: by place).
inline SDL_Color serverColor(int color, size_t place) {
  constexpr int Count = int(std::size(ServerColors));
  return ServerColors[color >= 0 && color < Count ? color : int(place % Count)];
}
// `status` is the untranslated key, never the translated text: callers pass the
// key here and draw tr(key).
inline SDL_Color statusColor(std::string_view status) {
  return status == "Connected"                               ? Good // i18n: keep
         : status == "Connecting" || status == "Awaiting approval"    ? Pending // i18n: keep
         : status == "Failed|connection" || status == "No internet" ? Bad // i18n: keep
                                                            : Secondary;
}
} // namespace foil::ui
