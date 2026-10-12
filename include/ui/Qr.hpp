#pragma once
#include <SDL2/SDL.h>
#include <string>
#include <vector>
namespace foil::ui {
class Qr {
public:
  bool encode(const std::string &url);
  void draw(SDL_Renderer *renderer, int x, int y, int width) const;
private:
  std::string url_;
  int size_ = 0;
  std::vector<uint8_t> modules_;
};
} // namespace foil::ui
