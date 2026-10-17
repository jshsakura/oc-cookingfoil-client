#pragma once
// The app's sounds, made in code rather than kept as files: a few notes
// each, on simple waves, mixed to mono 16-bit samples (picked by ear from
// candidates, 2026-10-16).
#include <cstdint>
#include <vector>
namespace foil {
enum class Cue { Start, Move, Add, Done, Error, Shortcut };
constexpr int CueCount = 6;
// The cue's samples at `rate` per second, ready to play.
std::vector<int16_t> chime(Cue cue, int rate);
} // namespace foil
