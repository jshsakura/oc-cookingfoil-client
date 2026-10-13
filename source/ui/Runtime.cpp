#include "ui/Runtime.hpp"
#include "ui/Theme.hpp"
#include "core/Log.hpp"
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <cstdio>
namespace foil::ui {
Runtime::~Runtime() {
  if (renderer_)
    SDL_DestroyRenderer(renderer_);
  if (surface_)
    SDL_FreeSurface(surface_);
  if (window_)
    SDL_DestroyWindow(window_);
  TTF_Quit();
  IMG_Quit();
  SDL_Quit();
}
bool Runtime::open(int width, int height, bool offscreen) {
  // Startup memory by step: the graphics stack takes most of an album
  // launch's heap, and the log says which part.
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
    return false;
  logLine("runtime sdl");
  const int flags = IMG_INIT_PNG | IMG_INIT_JPG;
  if ((IMG_Init(flags) & flags) != flags || TTF_Init() != 0)
    return false;
  if (offscreen) {
    surface_ = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                              SDL_PIXELFORMAT_RGBA32);
    if (!surface_)
      return false;
    renderer_ = SDL_CreateSoftwareRenderer(surface_);
  } else {
#ifdef __SWITCH__
    window_ = SDL_CreateWindow("CookingFoil", 0, 0, 0, 0,
                               SDL_WINDOW_FULLSCREEN_DESKTOP);
#else
    window_ = SDL_CreateWindow("CookingFoil", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, width, height,
                               SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
#endif
    if (!window_)
      return false;
    logLine("runtime window");
    renderer_ = SDL_CreateRenderer(
        window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_)
      renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  }
  if (!renderer_)
    return false;
  logLine("runtime renderer");
  SDL_GetRendererOutputSize(renderer_, &width, &height);
  // Explicit scale avoids the SDL console port's logical-size letterboxing.
  SDL_RenderSetScale(renderer_, width / float(Width), height / float(Height));
  SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
  return true;
}
void Runtime::render() {
  SDL_SetRenderDrawColor(renderer_, Ground.r, Ground.g, Ground.b, 255);
  SDL_RenderClear(renderer_);
  SDL_RenderPresent(renderer_);
}
bool Runtime::screenshot(const std::string &path) {
  int w = 0, h = 0;
  SDL_GetRendererOutputSize(renderer_, &w, &h);
  auto *shot =
      SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
  if (!shot)
    return false;
  const bool ok = SDL_RenderReadPixels(renderer_, nullptr, shot->format->format,
                                       shot->pixels, shot->pitch) == 0 &&
                  IMG_SavePNG(shot, path.c_str()) == 0;
  SDL_FreeSurface(shot);
  return ok;
}
} // namespace foil::ui
