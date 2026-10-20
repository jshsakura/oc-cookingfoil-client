#pragma once
#include <SDL2/SDL.h>
#include <iterator>
#include <string_view>
namespace foil::ui {
constexpr int Width = 1280, Height = 720, Sidebar = 232, Footer = 60;
constexpr SDL_Color Ground{242, 241, 238, 255}, Surface{255, 255, 255, 255};
constexpr SDL_Color Ink{22, 22, 26, 255}, Secondary{85, 85, 94, 255};
constexpr SDL_Color Line{226, 224, 218, 255}, Butter{255, 194, 61, 255};
// Focus ring uses the brand butter; its white inner band separates it from
// butter fills.
constexpr SDL_Color Focus = Butter, Okay{20, 99, 63, 255};
// Connection states: connected, in progress, failed.
constexpr SDL_Color Good{30, 142, 90, 255}, Pending{196, 124, 0, 255}, Bad{204, 62, 52, 255};
// A server's dot, apart from the connection states' red, orange and grey:
// green, blue, purple, teal, pink, indigo, lime, navy.
constexpr SDL_Color ServerColors[] = {{30, 142, 90, 255},  {52, 120, 246, 255}, {142, 92, 230, 255}, {20, 163, 168, 255},
                                      {224, 82, 156, 255}, {90, 103, 216, 255}, {120, 170, 40, 255}, {35, 55, 120, 255}};
// The dot of the server at `place` in the list, chosen as `color` (-1: by place).
inline SDL_Color serverColor(int color, size_t place) {
  constexpr int Count = int(std::size(ServerColors));
  return ServerColors[color >= 0 && color < Count ? color : int(place % Count)];
}
// `status` is the Korean key, never the translated text: callers pass the
// key here and draw tr(key).
inline SDL_Color statusColor(std::string_view status) {
  return status == "Connected"                               ? Good // i18n: keep
         : status == "Connecting" || status == "Awaiting approval"    ? Pending // i18n: keep
         : status == "Failed|connection" || status == "No internet" ? Bad // i18n: keep
                                                            : Secondary;
}
} // namespace foil::ui
