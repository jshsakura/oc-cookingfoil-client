#include "ui/Application.hpp"
#include <algorithm>
#include <cmath>
namespace foil::ui {
namespace {
// A flick loses this share of its speed each second, as a phone's list does.
constexpr float KeptPerSecond = 0.135f;
// Slower than this many pixels a second, it stops.
constexpr float StopSpeed = 20;
// A finger held still this long before lifting means no flick.
constexpr uint32_t FlickWindowMs = 80;
// Past either end the list stretches a little and springs back: a finger
// moves it at this share, and it stretches at most this far.
constexpr float Stretch = 0.35f, MaxStretch = 90;
// How fast it springs back (per second), and how fast a coast that runs
// into an end dies down.
constexpr float SpringRate = 14, EdgeBrake = 30;
} // namespace
float Application::glide(const std::string &id, float x, float y, float w, float h, float focus, float max,
                         float unit) {
  max = std::max(0.f, max);
  float shown = std::clamp(focus, 0.f, max);
  if (glide_.active && glide_.id == id) {
    glide_.max = max;
    glide_.unit = unit;
    shown = glide_.pos; // may be past an end while it stretches

  }
  glideAreas_.push_back({id, {x, y, w, h}, shown, max, unit});
  return shown;
}
void Application::fingerDown() {
  swallowTap_ = glide_.active && !glide_.held && glide_.velocity != 0;
  glide_.velocity = 0;
}
void Application::dragBegin(float x, float y) {
  // The topmost area under the finger, as drawn in the last frame.
  for (auto it = glideAreas_.rbegin(); it != glideAreas_.rend(); ++it) {
    const auto &a = it->area;
    if (x < a.x || x >= a.x + a.w || y < a.y || y >= a.y + a.h)
      continue;
    glide_ = {it->id, it->shown, 0, it->max, it->unit, true, true, SDL_GetTicks()};
    return;
  }
  glide_.held = false;
}
void Application::dragEnd(float fingerSpeed) {
  if (!glide_.held)
    return;
  glide_.held = false;
  if (!std::isnan(fingerSpeed))
    glide_.velocity = -fingerSpeed / glide_.unit;
  else if (SDL_GetTicks() - glide_.at > FlickWindowMs)
    glide_.velocity = 0;
  glide_.at = SDL_GetTicks();
}
float Application::glideFollow(float steps) const {
  // Inside the ends the list follows the finger; past one it gives less,
  // and never more than MaxStretch pixels.
  const float next = glide_.pos + steps;
  const float over = next < 0 ? next : next > glide_.max ? next - glide_.max : 0;
  if (over == 0)
    return next;
  const float edge = over < 0 ? 0 : glide_.max;
  const float before = glide_.pos < 0 ? glide_.pos : glide_.pos > glide_.max ? glide_.pos - glide_.max : 0;
  const float stretched = before + (over - before) * Stretch;
  const float limit = MaxStretch / glide_.unit;
  return edge + std::clamp(stretched, -limit, limit);
}
void Application::glideStep() {
  if (!glide_.active || glide_.held)
    return;
  const uint32_t now = SDL_GetTicks();
  const float dt = std::min(0.05f, (now - glide_.at) / 1000.f);
  glide_.at = now;
  const bool past = glide_.pos < 0 || glide_.pos > glide_.max;
  if (glide_.velocity != 0) {
    glide_.pos = glideFollow(glide_.velocity * dt);
    // Running into an end: the stretch eats the speed quickly.
    glide_.velocity *= std::pow(KeptPerSecond, dt) * (past ? std::exp(-EdgeBrake * dt) : 1.f);
    if (std::abs(glide_.velocity) * glide_.unit < StopSpeed)
      glide_.velocity = 0;
    if (!past)
      return;
  }
  if (!past)
    return;
  // Spring back to the end it went past.
  const float edge = glide_.pos < 0 ? 0 : glide_.max;
  glide_.pos = edge + (glide_.pos - edge) * std::exp(-SpringRate * dt);
  if (std::abs(glide_.pos - edge) * glide_.unit < 0.5f) {
    glide_.pos = edge;
    glide_.velocity = 0;
  }
}
void Application::clipTo(float x, float y, float w, float h) {
  SDL_Rect previous{};
  SDL_RenderGetClipRect(paint_.renderer, &previous);
  clips_.push_back(previous);
  SDL_Rect area{int(std::floor(x)), int(std::floor(y)), int(std::ceil(w)), int(std::ceil(h))};
  // Inside whatever clip is already set (the content column).
  if (!SDL_RectEmpty(&previous))
    SDL_IntersectRect(&previous, &area, &area);
  SDL_RenderSetClipRect(paint_.renderer, &area);
}
void Application::unclip() {
  if (clips_.empty())
    return;
  const auto previous = clips_.back();
  clips_.pop_back();
  SDL_RenderSetClipRect(paint_.renderer, SDL_RectEmpty(&previous) ? nullptr : &previous);
}
} // namespace foil::ui
