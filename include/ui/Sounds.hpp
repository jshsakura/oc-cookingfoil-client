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
  // The start cue, now or as soon as it is made, on the splash's next
  // landing: the butter lands every second from `beatAt` (SDL ticks).
  void playStart(uint32_t beatAt);
  // When the start cue's first coin is heard (SDL ticks); 0 until it is
  // set to play. Its fanfare's chord comes FanfareMs after that.
  uint32_t startHeardAt() const { return startHeardAt_; }
  static constexpr uint32_t FanfareMs = 2270;
  // Off also stops what is playing or waiting to (the start cue, set to
  // play before the setting is read off the card).
  void setOn(bool on);
  bool on() const { return on_; }

private:
  struct Voice {
    const std::vector<int16_t> *samples;
    size_t at;
    size_t wait = 0; // samples of silence before it starts
    int gain = 256;  // of 256
  };
  void takeRequests();
  static void mix(void *self, Uint8 *stream, int bytes);
  SDL_AudioDeviceID device_ = 0;
  std::array<std::vector<int16_t>, CueCount> cues_; // filled under the lock
  std::vector<Voice> voices_; // under the device's lock
  std::atomic<bool> on_{true};
  bool startWanted_ = false, startPlayed_ = false; // under the lock
  std::atomic<uint32_t> beatAt_{0};
  // A cue asked for: the screen only leaves this note, the audio thread takes
  // it at its next buffer. The screen never waits for the device's lock.
  struct Request {
    std::atomic<uint32_t> count{0};
    std::atomic<size_t> wait{0};
  };
  std::array<Request, CueCount> requests_;
  std::atomic<uint32_t> startHeardAt_{0};
  std::thread maker_;
  std::array<std::atomic<uint32_t>, CueCount> lastAt_{}; // when each cue last started
};
} // namespace foil::ui
