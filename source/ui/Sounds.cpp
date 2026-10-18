#include "ui/Sounds.hpp"
#include "core/Log.hpp"
#include "platform/Cores.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
constexpr int Rate = 48000; // the console's own rate: nothing to convert
constexpr size_t MostVoices = 6;
// Mashed buttons do not pile one cue on itself: a held stick does not buzz
// and a burst of presses does not stack loud enough to crackle.
constexpr uint32_t MoveGapMs = 35;
constexpr uint32_t OtherGapMs = 90;
// Every cue a touch under how it was made (about -1.4 dB).
constexpr int Gain = 218;
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
    // Never a frame's time, nor the start's: the loading runs on the writer
    // core, and a thread there is not paused for another of its priority.
    const platform::OnCore off(platform::NetworkCore);
    for (int i = 0; i < CueCount; ++i) {
      auto samples = chime(Cue(i), Rate);
      SDL_LockAudioDevice(device_);
      cues_[i] = std::move(samples);
      const bool start = Cue(i) == Cue::Start && startWanted_;
      SDL_UnlockAudioDevice(device_);
      if (start) play(Cue::Start);
    }
  });
  return true;
}
void Sounds::playStart(uint32_t beatAt) {
  if (!device_) return;
  SDL_LockAudioDevice(device_);
  beatAt_ = beatAt;
  const bool ready = !cues_[int(Cue::Start)].empty();
  if (!ready) startWanted_ = true;
  SDL_UnlockAudioDevice(device_);
  if (ready) play(Cue::Start);
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
  // The start cue plays once a launch: once it has, its samples (the
  // longest, about 200 KB) go.
  if (cue != Cue::Start && startPlayed_ && !cues_[int(Cue::Start)].empty() &&
      std::none_of(voices_.begin(), voices_.end(), [&](const Voice &v) { return v.samples == &cues_[int(Cue::Start)]; }))
    std::vector<int16_t>().swap(cues_[int(Cue::Start)]);
  if (cue == Cue::Start) startPlayed_ = true;
  if (voices_.size() >= MostVoices) voices_.erase(voices_.begin()); // the oldest gives way
  // The start waits for the butter's next landing, less what the device
  // holds before it is heard (one buffer).
  size_t wait = 0;
  // Right behind a done, a shortcut's note comes on that arpeggio's next
  // step (its notes are 80 ms apart) rather than over it.
  constexpr uint32_t DoneStepMs = 240;
  if (const auto done = lastAt_[int(Cue::Done)]; cue == Cue::Shortcut && done && now - done < DoneStepMs)
    wait = size_t(DoneStepMs - (now - done)) * Rate / 1000;
  if (cue == Cue::Start && beatAt_) {
    constexpr uint32_t BeatMs = 1000, BufferMs = 1024 * 1000 / Rate;
    const uint32_t toBeat = (BeatMs - (now - beatAt_) % BeatMs) % BeatMs;
    const uint32_t waitMs = (toBeat + BeatMs - BufferMs % BeatMs) % BeatMs;
    wait = size_t(waitMs) * Rate / 1000;
    startHeardAt_ = now + waitMs + BufferMs;
  }
  voices_.push_back({&cues_[int(cue)], 0, wait});
  SDL_UnlockAudioDevice(device_);
}
// On SDL's audio thread, with the device locked.
void Sounds::mix(void *self, Uint8 *stream, int bytes) {
  auto &me = *static_cast<Sounds *>(self);
  auto *out = reinterpret_cast<int16_t *>(stream);
  const size_t count = size_t(bytes) / sizeof(int16_t);
  std::fill(out, out + count, int16_t(0));
  for (auto &v : me.voices_) {
    const size_t skip = std::min(count, v.wait);
    v.wait -= skip;
    const size_t n = std::min(count - skip, v.samples->size() - v.at);
    for (size_t i = 0; i < n; ++i)
      out[skip + i] = int16_t(std::clamp(int(out[skip + i]) + int((*v.samples)[v.at + i]) * Gain / 256, -32768, 32767));
    v.at += n;
  }
  me.voices_.erase(std::remove_if(me.voices_.begin(), me.voices_.end(),
                                  [](const Voice &v) { return v.at >= v.samples->size(); }),
                   me.voices_.end());
}
} // namespace foil::ui
