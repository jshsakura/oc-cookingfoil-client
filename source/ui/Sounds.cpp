#include "ui/Sounds.hpp"
#include "core/Log.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
constexpr int Rate = 48000; // the console's own rate: nothing to convert
constexpr size_t MostVoices = 6;
// Mashed buttons do not pile one cue on itself: a held stick does not buzz
// and a burst of presses does not stack loud enough to crackle.
constexpr uint32_t MoveGapMs = 35;
constexpr uint32_t OtherGapMs = 90;
} // namespace
Sounds::~Sounds() {
  if (maker_.joinable()) maker_.join();
  if (device_) SDL_CloseAudioDevice(device_);
}
bool Sounds::open() {
  if (device_) return true;
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    logLine(std::string("sounds: no audio (") + SDL_GetError() + ")");
    return false;
  }
  SDL_AudioSpec want{}, got{};
  want.freq = Rate;
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 1024;
  want.callback = &Sounds::mix;
  want.userdata = this;
  device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &got, 0); // SDL converts to what the device takes
  if (!device_) {
    logLine(std::string("sounds: no audio device (") + SDL_GetError() + ")");
    return false;
  }
  voices_.reserve(MostVoices + 1);
  SDL_PauseAudioDevice(device_, 0);
  maker_ = std::thread([this] {
    for (int i = 0; i < CueCount; ++i) {
      auto samples = chime(Cue(i), Rate);
      SDL_LockAudioDevice(device_);
      cues_[i] = std::move(samples);
      SDL_UnlockAudioDevice(device_);
      if (Cue(i) == Cue::Start) play(Cue::Start);
    }
  });
  return true;
}
void Sounds::play(Cue cue) {
  if (!device_ || !on_) return;
  const auto now = SDL_GetTicks();
  auto &last = lastAt_[int(cue)];
  if (last && now - last < (cue == Cue::Move ? MoveGapMs : OtherGapMs)) return;
  SDL_LockAudioDevice(device_);
  if (cues_[int(cue)].empty()) { // still being made
    SDL_UnlockAudioDevice(device_);
    return;
  }
  last = now;
  if (voices_.size() >= MostVoices) voices_.erase(voices_.begin()); // the oldest gives way
  voices_.push_back({&cues_[int(cue)], 0});
  SDL_UnlockAudioDevice(device_);
}
// On SDL's audio thread, with the device locked.
void Sounds::mix(void *self, Uint8 *stream, int bytes) {
  auto &me = *static_cast<Sounds *>(self);
  auto *out = reinterpret_cast<int16_t *>(stream);
  const size_t count = size_t(bytes) / sizeof(int16_t);
  std::fill(out, out + count, int16_t(0));
  for (auto &v : me.voices_) {
    const size_t n = std::min(count, v.samples->size() - v.at);
    for (size_t i = 0; i < n; ++i)
      out[i] = int16_t(std::clamp(int(out[i]) + int((*v.samples)[v.at + i]), -32768, 32767));
    v.at += n;
  }
  me.voices_.erase(std::remove_if(me.voices_.begin(), me.voices_.end(),
                                  [](const Voice &v) { return v.at >= v.samples->size(); }),
                   me.voices_.end());
}
} // namespace foil::ui
