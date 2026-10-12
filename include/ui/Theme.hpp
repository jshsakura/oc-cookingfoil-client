#pragma once
#include <SDL2/SDL.h>
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
// `status` is the Korean key, never the translated text: callers pass the
// key here and draw tr(key).
inline SDL_Color statusColor(std::string_view status) {
  return status == "연결됨"                               ? Good // i18n: keep
         : status == "연결 중" || status == "승인 대기"    ? Pending // i18n: keep
         : status == "연결 실패" || status == "인터넷 없음" ? Bad // i18n: keep
                                                            : Secondary;
}
} // namespace foil::ui
