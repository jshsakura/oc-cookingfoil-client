#pragma once
// Plays the app's cues (core/Chime.hpp) on the console's audio, several at
// once if they overlap. Off in 설정 (효과음): nothing plays. A console or a
// host without audio simply stays silent.
#include "core/Chime.hpp"
#include <SDL2/SDL.h>
#include <array>
#include <cstdint>
#include <vector>
namespace foil::ui {
class Sounds {
public:
  Sounds() = default;
  Sounds(const Sounds &) = delete;
  Sounds &operator=(const Sounds &) = delete;
  ~Sounds();
  bool open();
  void play(Cue cue);
  void setOn(bool on) { on_ = on; }
  bool on() const { return on_; }

private:
  struct Voice {
    const std::vector<int16_t> *samples;
    size_t at;
  };
  static void mix(void *self, Uint8 *stream, int bytes);
  SDL_AudioDeviceID device_ = 0;
  std::array<std::vector<int16_t>, CueCount> cues_;
  std::vector<Voice> voices_; // under the device's lock
  bool on_ = true;
  uint32_t lastMove_ = 0;
};
} // namespace foil::ui
