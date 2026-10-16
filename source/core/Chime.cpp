#include "core/Chime.hpp"
#include <algorithm>
#include <cmath>
namespace foil {
namespace {
enum class Wave { Marimba, Pulse, Triangle, Sine };
struct Note {
  float at;   // seconds from the start
  int pitch;  // MIDI note number (69 is A4)
  float dur, decay;
  Wave wave;
  float volume = 1.f;
};
struct Score {
  float length, delay, feedback, gain;
  std::vector<Note> notes;
};
constexpr float Pi = 3.14159265f;
constexpr int C4 = 60, E4 = 64, E5 = 76, G5 = 79, C6 = 84, D6 = 86, E6 = 88, G6 = 91, B6 = 95, C7 = 96;
float hertz(int pitch) { return 440.f * std::pow(2.f, (pitch - 69) / 12.f); }
float sample(Wave wave, float f, float t) {
  switch (wave) {
  case Wave::Marimba: // a struck bar: the note and a fast-dying fourth harmonic
    return std::sin(2 * Pi * f * t) + 0.35f * std::exp(-t / 0.03f) * std::sin(2 * Pi * f * 4 * t);
  case Wave::Pulse: { // a quarter-duty pulse, the 8-bit sound
    const float phase = f * t - std::floor(f * t);
    return phase < 0.25f ? 1.f : -1.f;
  }
  case Wave::Sine:
    return std::sin(2 * Pi * f * t);
  case Wave::Triangle: {
    const float phase = f * t - std::floor(f * t);
    return 4 * std::fabs(phase - 0.5f) - 1;
  }
  }
  return 0;
}
Score score(Cue cue) {
  switch (cue) {
  case Cue::Start: { // 8비트 출발: a short fanfare
    Score s{1.4f, 0, 0, 0.45f, {}};
    const int run[] = {E5, G5, E6, C6, D6, G6};
    for (int i = 0; i < 6; ++i) s.notes.push_back({i * 0.085f, run[i], 0.08f, i < 5 ? 0.08f : 0.3f, Wave::Pulse, 0.5f});
    return s;
  }
  case Cue::Move: // 틱: a high, very short sine per step
    return {0.06f, 0, 0, 0.18f, {{0, C7, 0.01f, 0.012f, Wave::Sine}}};
  case Cue::Add: // two notes up
    return {0.35f, 0, 0, 0.35f, {{0, E6, 0.04f, 0.05f, Wave::Marimba}, {0.06f, B6, 0.05f, 0.12f, Wave::Marimba}}};
  case Cue::Done: { // 도·미·솔 on the marimba, a short echo after
    Score s{0.9f, 0.12f, 0.2f, 0.4f, {}};
    const int run[] = {C6, E6, G6};
    for (int i = 0; i < 3; ++i) s.notes.push_back({i * 0.08f, run[i], 0.05f, i < 2 ? 0.12f : 0.35f, Wave::Marimba});
    return s;
  }
  case Cue::Error: // two low notes down
    return {0.45f, 0, 0, 0.4f, {{0, E4, 0.08f, 0.08f, Wave::Triangle}, {0.11f, C4, 0.12f, 0.14f, Wave::Triangle}}};
  }
  return {};
}
} // namespace
std::vector<int16_t> chime(Cue cue, int rate) {
  const auto s = score(cue);
  std::vector<float> out(size_t(s.length * rate), 0.f);
  for (const auto &n : s.notes) {
    const size_t start = size_t(n.at * rate);
    const float f = hertz(n.pitch);
    constexpr float Attack = 0.004f;
    const size_t span = size_t((n.dur + n.decay * 3) * rate);
    for (size_t i = 0; i < span && start + i < out.size(); ++i) {
      const float t = float(i) / rate;
      out[start + i] += sample(n.wave, f, t) * std::min(1.f, t / Attack) * std::exp(-t / n.decay) * n.volume;
    }
  }
  if (s.delay > 0)
    for (size_t i = size_t(s.delay * rate); i < out.size(); ++i) out[i] += out[i - size_t(s.delay * rate)] * s.feedback;
  float peak = 1e-9f;
  for (const float v : out) peak = std::max(peak, std::fabs(v));
  // The last 80 ms (a third of a short one) fade, so nothing ends on a click.
  const size_t fade = std::min(size_t(0.08f * rate), out.size() / 3);
  for (size_t i = 0; i < fade; ++i) out[out.size() - 1 - i] *= float(i) / fade;
  std::vector<int16_t> samples(out.size());
  for (size_t i = 0; i < out.size(); ++i)
    samples[i] = int16_t(std::clamp(out[i] / peak * s.gain, -1.f, 1.f) * 32767);
  return samples;
}
} // namespace foil
