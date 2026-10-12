#pragma once
#include <SDL2/SDL.h>
#include <string>
namespace foil::ui {
class Runtime {
public:
  Runtime() = default;
  ~Runtime();
  bool open(int width, int height, bool offscreen);
  void render();
  bool screenshot(const std::string &path);
  SDL_Renderer *renderer() const { return renderer_; }

private:
  SDL_Window *window_ = nullptr;
  SDL_Surface *surface_ = nullptr;
  SDL_Renderer *renderer_ = nullptr;
};
} // namespace foil::ui
