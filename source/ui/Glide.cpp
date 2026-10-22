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
// A shelf settling onto a card: a spring stiff enough to land quickly and
// damped just under critical, so it overshoots a touch and clicks back.
constexpr float SnapStiffness = 150, SnapDamping = 19;
} // namespace
float Application::glide(const std::string &id, float x, float y, float w, float h, float focus, float max,
                         float unit, bool horizontal, bool snap) {
  max = std::max(0.f, max);
  float shown = std::clamp(focus, 0.f, max);
  if (const auto rest = glideRest_.find(id); rest != glideRest_.end() && !(glide_.active && glide_.id == id))
    shown = std::clamp(rest->second, 0.f, max);
  if (glide_.active && glide_.id == id) {
    glide_.max = max;
    glide_.unit = unit;
    shown = glide_.pos; // may be past an end while it stretches

  }
  glideAreas_.push_back({id, {x, y, w, h}, shown, max, unit, horizontal, snap});
  // On whole screen pixels: text is drawn on them (Fonts), so a list that
  // stopped between two would move its cards and its words by different
  // amounts and the words would ripple as it scrolls.
  if (const float pixels = unit * paint_.fonts.scale(); pixels > 0) shown = std::round(shown * pixels) / pixels;
  return shown;
}
void Application::fingerDown() {
  swallowTap_ = glide_.active && !glide_.held && (glide_.velocity != 0 || glide_.settling);
  glide_.velocity = 0;
  glide_.settling = false;
}
void Application::fingerDown(float x, float y) {
  const auto inside = [&](const SDL_FRect &a) { return x >= a.x && x < a.x + a.w && y >= a.y && y < a.y + a.h; };
  // A list still coasting on another screen (or elsewhere on this one) does
  // not eat the touch: only one on the moving list stops it.
  const bool onIt = std::any_of(glideAreas_.begin(), glideAreas_.end(),
                                [&](const GlideArea &a) { return a.id == glide_.id && inside(a.area); });
  fingerDown();
  swallowTap_ = swallowTap_ && onIt;
  pressing_ = false;
  if (swallowTap_) return;
  for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
    if (inside(it->area)) {
      pressed_ = it->area;
      pressedLook_ = it->look;
      pressedRadius_ = it->radius;
      pressing_ = true;
      pressedUntil_ = 0;
      pressedScreen_ = int(screen_);
      pressedPopup_ = popupOpen();
      return;
    }
}
void Application::dragBegin(float x, float y, float dx, float dy) {
  pressing_ = false; // a drag presses nothing
  // The topmost area under the finger that moves the way the finger set
  // off, as drawn in the last frame.
  const bool sideways = std::abs(dx) > std::abs(dy);
  // The area let go of stays where it was, or where it was settling to.
  if (glide_.active && glide_.id != "hero")
    glideRest_[glide_.id] = glide_.settling ? glide_.target : std::clamp(glide_.pos, 0.f, glide_.max);
  for (auto it = glideAreas_.rbegin(); it != glideAreas_.rend(); ++it) {
    const auto &a = it->area;
    if (it->horizontal != sideways || x < a.x || x >= a.x + a.w || y < a.y || y >= a.y + a.h)
      continue;
    glide_ = {it->id, it->shown, 0, it->max, it->unit, true, true, sideways, SDL_GetTicks(), 0, it->snap, false, 0};
    return;
  }
  glide_.held = false;
}
void Application::dragEnd(float speedX, float speedY) {
  if (!glide_.held)
    return;
  glide_.held = false;
  const float fingerSpeed = glide_.horizontal ? speedX : speedY;
  if (glide_.id == "hero") {
    heroFlick(std::isnan(fingerSpeed) ? -glide_.velocity : fingerSpeed);
    return;
  }
  if (!std::isnan(fingerSpeed))
    glide_.velocity = -fingerSpeed / glide_.unit;
  if (glide_.snap) {
    // Where the flick would coast to, rounded to a whole card, so the shelf
    // always comes to rest with its cards whole.
    const float coast = glide_.velocity / std::log(1 / KeptPerSecond);
    glide_.target = std::clamp(std::round(glide_.pos + coast), 0.f, std::max(0.f, std::round(glide_.max)));
    glide_.settling = true;
    glide_.at = SDL_GetTicks();
    return;
  }
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
// The banner flips by how far the finger went, or how fast it flicked;
// it springs back into place either way.
void Application::heroFlick(float speed) {
  constexpr float FlipDistance = 60, FlipSpeed = 500;
  glide_.velocity = 0;
  if (glide_.travel <= -FlipDistance || speed <= -FlipSpeed)
    showHero(heroSlide_ + 1);
  else if (glide_.travel >= FlipDistance || speed >= FlipSpeed)
    showHero(heroSlide_ - 1);
}
void Application::glideStep() {
  if (!glide_.active || glide_.held)
    return;
  const uint32_t now = SDL_GetTicks();
  const float dt = std::min(0.05f, (now - glide_.at) / 1000.f);
  glide_.at = now;
  if (glide_.settling) {
    // In small steps, so a slow frame does not make the spring jump.
    for (float left = dt; left > 0; left -= 0.008f) {
      const float h = std::min(left, 0.008f);
      const float pull = SnapStiffness * (glide_.target - glide_.pos) - SnapDamping * glide_.velocity;
      glide_.velocity += pull * h;
      glide_.pos += glide_.velocity * h;
    }
    if (std::abs(glide_.target - glide_.pos) * glide_.unit < 0.5f && std::abs(glide_.velocity) * glide_.unit < 8) {
      glide_.pos = glide_.target;
      glide_.velocity = 0;
      glide_.settling = false;
    }
    return;
  }
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
void Application::stick(float amount, float seconds) {
  constexpr float FullSpeed = 1800; // px/s at full tilt; gentle near the middle
  // The largest list that scrolls; on a page that fits, the page itself,
  // which only stretches and springs back, as under a finger.
  const GlideArea *target = nullptr;
  for (const bool scrolls : {true, false}) {
    for (const auto &a : glideAreas_)
      if (!a.horizontal && (a.max > 0) == scrolls &&
          (!target || a.area.w * a.area.h > target->area.w * target->area.h))
        target = &a;
    if (target) break;
  }
  if (!target)
    return;
  if (!(glide_.active && glide_.id == target->id)) {
    if (glide_.active && glide_.id != "hero")
      glideRest_[glide_.id] = glide_.settling ? glide_.target : std::clamp(glide_.pos, 0.f, glide_.max);
    glide_ = {target->id, target->shown, 0, target->max, target->unit, true, false, false, SDL_GetTicks(), 0, false, false, 0};
  }
  const float speed = amount * std::abs(amount) * FullSpeed / glide_.unit;
  // Past an end it stretches, less the further it goes; let go, it springs back (glideStep).
  glide_.pos = glideFollow(speed * seconds);
  glide_.velocity = 0;
  glide_.at = SDL_GetTicks();
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
