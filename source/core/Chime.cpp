#include "core/Chime.hpp"
#include <algorithm>
#include <cmath>
namespace foil {
namespace {
enum class Wave { Marimba, Pulse, Triangle, Sine, Glock, Vibes, Pluck };
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
constexpr int F3 = 53, G3 = 55, A3 = 57, D4 = 62, F4 = 65, A4 = 69, C4 = 60, E4 = 64, G4 = 67, B4 = 71, C5 = 72, D5 = 74, E5 = 76, G5 = 79, A5 = 81, C6 = 84, D6 = 86, E6 = 88,
              G6 = 91, B6 = 95, C7 = 96;
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
  case Wave::Glock: // a small bell: the note and a quickly fading, slightly sharp sixth partial
    return std::sin(2 * Pi * f * t) + 0.25f * std::exp(-t / 0.05f) * std::sin(2 * Pi * f * 2.76f * t);
  case Wave::Vibes: // a metal bar with its tremolo
    return std::sin(2 * Pi * f * t) * (1 - 0.35f * (0.5f + 0.5f * std::sin(2 * Pi * 5.5f * t)));
  case Wave::Triangle: {
    const float phase = f * t - std::floor(f * t);
    return 4 * std::fabs(phase - 0.5f) - 1;
  }
  }
  return 0;
}
Score score(Cue cue) {
  switch (cue) {
  case Cue::Start: { // 우쿨렐레: strummed C, C, Am, F, G and C ringing on
    Score s{3.f, 0.16f, 0.22f, 0.3f, {}};
    struct Strum {
      float at, ring, volume, spread;
      int notes[4];
    };
    static constexpr Strum Strums[] = {{0, 0.6f, 0.45f, 0.025f, {C4, E4, G4, C5}},   {0.4f, 0.4f, 0.35f, 0.02f, {C4, E4, G4, C5}},
                                       {0.75f, 0.5f, 0.4f, 0.025f, {A3, C4, E4, A4}}, {1.15f, 0.5f, 0.4f, 0.025f, {F3, C4, F4, A4}},
                                       {1.55f, 0.4f, 0.4f, 0.025f, {G3, D4, G4, B4}}, {1.9f, 1.6f, 0.45f, 0.03f, {C4, E4, G4, C5}}};
    for (const auto &strum : Strums)
      for (int k = 0; k < 4; ++k)
        s.notes.push_back({strum.at + k * strum.spread, strum.notes[k], 0, strum.ring * 1.2f, Wave::Pluck, strum.volume});
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
    if (n.wave == Wave::Pluck) { // a plucked string (Karplus-Strong): a burst of noise, averaged as it circles
      std::vector<float> ring(std::max(2, int(rate / f)));
      uint32_t seed = uint32_t(f * 1000) | 1;
      for (auto &v : ring) {
        seed = seed * 1664525u + 1013904223u;
        v = float(seed >> 8) / float(1 << 24) * 2 - 1;
      }
      for (size_t i = 0; i < span && start + i < out.size(); ++i) {
        const float t = float(i) / rate;
        auto &here = ring[i % ring.size()];
        out[start + i] += here * std::min(1.f, t / 0.002f) * std::exp(-t / n.decay) * n.volume;
        here = 0.4987f * (here + ring[(i + 1) % ring.size()]); // a little lost each turn
      }
      continue;
    }
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
