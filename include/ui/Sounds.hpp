#pragma once
// Plays the app's cues (core/Chime.hpp) on the console's audio, several at
// once if they overlap. Off in 설정 (효과음): nothing plays. A console or a
// host without audio simply stays silent.
#include "core/Chime.hpp"
#include <SDL2/SDL.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>
namespace foil::ui {
class Sounds {
public:
  Sounds() = default;
  Sounds(const Sounds &) = delete;
  Sounds &operator=(const Sounds &) = delete;
  ~Sounds();
  // Opens the audio and makes the cues on a thread of their own, so the
  // screen never waits; the start cue plays as soon as it is ready.
  bool open();
  void play(Cue cue);
  // The start cue, now or as soon as it is made.
  void playStart();
  void setOn(bool on) { on_.store(on); }
  bool on() const { return on_; }

private:
  struct Voice {
    const std::vector<int16_t> *samples;
    size_t at;
  };
  static void mix(void *self, Uint8 *stream, int bytes);
  SDL_AudioDeviceID device_ = 0;
  std::array<std::vector<int16_t>, CueCount> cues_; // filled under the lock
  std::vector<Voice> voices_; // under the device's lock
  std::atomic<bool> on_{true};
  bool startWanted_ = false, startPlayed_ = false; // under the lock
  std::thread maker_;
  std::array<uint32_t, CueCount> lastAt_{}; // when each cue last started
};
} // namespace foil::ui
