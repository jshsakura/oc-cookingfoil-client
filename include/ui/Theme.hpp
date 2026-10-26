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
// Black: true black for OLED screens. The rest follow well-known terminal
// color schemes, with their red brightened where errors read too dim.
constexpr Palette BlackPalette{{0, 0, 0, 255},        {22, 22, 26, 255},    {236, 236, 240, 255},
                               {150, 150, 160, 255},  {46, 46, 52, 255},    {92, 204, 148, 255},
                               {63, 191, 131, 255},   {232, 160, 48, 255},  {240, 104, 92, 255},
                               {64, 53, 24, 255},     {70, 30, 30, 255},    {24, 54, 40, 255},
                               {96, 96, 104, 255},    {255, 255, 255, 30},  true};
constexpr Palette CatppuccinLattePalette{{230, 233, 239, 255}, {250, 251, 253, 255}, {76, 79, 105, 255}, {92, 95, 119, 255}, {204, 208, 218, 255}, {40, 118, 26, 255}, {64, 160, 43, 255}, {200, 86, 6, 255}, {210, 15, 57, 255}, {248, 228, 196, 255}, {246, 214, 220, 255}, {220, 238, 214, 255}, {156, 160, 176, 255}, {0, 0, 0, 30}, false};
constexpr Palette CatppuccinMochaPalette{{30, 30, 46, 255}, {49, 50, 68, 255}, {205, 214, 244, 255}, {166, 173, 200, 255}, {69, 71, 90, 255}, {166, 227, 161, 255}, {166, 227, 161, 255}, {250, 179, 135, 255}, {243, 139, 168, 255}, {85, 79, 78, 255}, {83, 57, 77, 255}, {64, 79, 75, 255}, {108, 112, 134, 255}, {255, 255, 255, 30}, true};
constexpr Palette NordPalette{{46, 52, 64, 255}, {59, 66, 82, 255}, {236, 239, 244, 255}, {196, 204, 218, 255}, {76, 86, 106, 255}, {163, 190, 140, 255}, {163, 190, 140, 255}, {235, 203, 139, 255}, {226, 128, 136, 255}, {93, 90, 83, 255}, {82, 63, 74, 255}, {75, 86, 83, 255}, {118, 128, 148, 255}, {255, 255, 255, 30}, true};
constexpr Palette DraculaPalette{{40, 42, 54, 255}, {56, 59, 76, 255}, {248, 248, 242, 255}, {180, 188, 220, 255}, {80, 84, 106, 255}, {80, 250, 123, 255}, {80, 250, 123, 255}, {255, 184, 108, 255}, {255, 110, 110, 255}, {84, 74, 52, 255}, {94, 53, 62, 255}, {50, 94, 71, 255}, {122, 134, 180, 255}, {255, 255, 255, 30}, true};
constexpr Palette GruvboxPalette{{40, 40, 40, 255}, {60, 56, 54, 255}, {235, 219, 178, 255}, {189, 174, 147, 255}, {80, 73, 69, 255}, {184, 187, 38, 255}, {184, 187, 38, 255}, {250, 189, 47, 255}, {255, 104, 84, 255}, {92, 77, 42, 255}, {93, 48, 43, 255}, {76, 77, 40, 255}, {124, 111, 100, 255}, {255, 255, 255, 30}, true};
constexpr Palette TokyoNightPalette{{26, 27, 38, 255}, {36, 40, 59, 255}, {192, 202, 245, 255}, {154, 165, 206, 255}, {65, 72, 104, 255}, {158, 206, 106, 255}, {158, 206, 106, 255}, {224, 175, 104, 255}, {247, 118, 142, 255}, {76, 64, 55, 255}, {81, 50, 64, 255}, {59, 72, 55, 255}, {98, 108, 150, 255}, {255, 255, 255, 30}, true};
constexpr Palette SolarizedLightPalette{{238, 232, 213, 255}, {253, 246, 227, 255}, {7, 54, 66, 255}, {88, 110, 117, 255}, {220, 212, 188, 255}, {98, 116, 0, 255}, {133, 153, 0, 255}, {160, 118, 0, 255}, {220, 50, 47, 255}, {250, 230, 178, 255}, {247, 215, 205, 255}, {228, 234, 200, 255}, {147, 161, 161, 255}, {0, 0, 0, 30}, false};
constexpr Palette SolarizedDarkPalette{{0, 43, 54, 255}, {7, 54, 66, 255}, {238, 232, 213, 255}, {147, 161, 161, 255}, {16, 72, 86, 255}, {133, 153, 0, 255}, {133, 153, 0, 255}, {181, 137, 0, 255}, {242, 96, 90, 255}, {45, 66, 41, 255}, {55, 40, 52, 255}, {33, 71, 41, 255}, {88, 110, 117, 255}, {255, 255, 255, 30}, true};
// In ThemeNames' order (core/State.hpp).
constexpr const Palette *ThemePalettes[] = {
    &LightPalette,       &DarkPalette,           &BlackPalette,      &CatppuccinLattePalette,
    &CatppuccinMochaPalette, &NordPalette,       &DraculaPalette,    &GruvboxPalette,
    &TokyoNightPalette,  &SolarizedLightPalette, &SolarizedDarkPalette};
inline Palette palette = LightPalette;
inline void usePalette(const Palette &next) { palette = next; }
inline const SDL_Color &Ground = palette.ground, &Surface = palette.surface;
inline const SDL_Color &Ink = palette.ink, &Secondary = palette.secondary;
inline const SDL_Color &Line = palette.line;
inline const SDL_Color &Cream = palette.cream, &RedTint = palette.redTint, &GreenTint = palette.greenTint;
inline const SDL_Color &Muted = palette.muted, &Press = palette.press;
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
