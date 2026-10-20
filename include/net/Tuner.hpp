#pragma once
// How many connections a download uses, found as it goes and held steady:
//
// - It starts at 2 (or the ceiling, when lower) and, after a span to know
//   its speed, tries one more. A try is judged over two spans: more than a
//   tenth faster keeps it and tries the next soon; anything less goes back
//   to the count before (the better one for fewer connections) and waits
//   longer before trying again (6 spans, doubling to 24), so a count that
//   is right is not shaken every few seconds.
// - Settled, it keeps watching: when the speed at its count moves by more
//   than Shift (the line got busier or freer), it tries again at once
//   instead of waiting, so a count that has gone stale is found anew.
// - Trouble (an error, a retry, a timeout) in two spans in a row steps down
//   one (never below 1); a single blip changes nothing.
// - A span with no bytes (paused, offline) decides nothing.
// - The ceiling is the server's (maxConnections), shared out among the
//   consoles downloading from it now (X-CookingFoil-Active-Downloads); a
//   lower share applies at once.
#include <cstdint>
#include <map>
#include <string>
namespace foil::net {
class ConnectionTuner {
public:
  static constexpr double SpanSeconds = 7, Gain = 1.10, Shift = 0.30;
  static constexpr int TrySpans = 2, RetryAfter = 6, MostWait = 24;
  // `remembered`: the count a run before found at this place; it starts
  // there as found (the next try waits its turn) instead of at 2.
  explicit ConnectionTuner(int ceiling = 0, int remembered = 0);
  int connections() const { return now_; }
  // The count is found: no try under way, and the last one went back (more
  // did not help), it is at the ceiling, or it backed off or was shared.
  // Before that the speed is still climbing and an estimate from it is high.
  bool found() const { return !trying_ && (now_ >= ceiling_ || (reason_ != "start" && wait_ > 1)); }
  // The ceiling now: the server's, divided among `downloading` consoles
  // (this one included; 0 or 1 when it is alone).
  int ceiling(int downloading) const;
  // Something failed and is being asked again: counted at the span's end.
  void trouble() { trouble_ = true; }
  // A span ended with `bytes` received over `seconds`; returns the count.
  int span(uint64_t bytes, double seconds, int downloading);
  // "connections 3 (tuned), 2 others downloading".
  std::string summary(int downloading) const;
  // The next download to the same place goes on with what was found: the
  // count (under the new `ceiling`) and the wait before the next try. A try
  // under way is finished over the new download's spans.
  void carryOn(int ceiling);

private:
  void settle(int wait, const char *reason);
  int ceiling_ = 1, now_ = 1;
  bool trouble_ = false, trying_ = false;
  int troubled_ = 0;   // spans in a row with trouble
  int here_ = 0;       // measured spans since the count last changed
  int wait_ = 1;       // spans at a count before the next try
  int tried_ = 0;      // spans measured in the try
  double speed_ = 0;   // bytes a second at this count, smoothed
  double before_ = 0;  // the speed the try is measured against
  double trySum_ = 0;
  double settled_ = 0; // the speed when the count was last decided
  std::string reason_ = "start";
};
// The running download's tuner. startTuning() begins one under the
// server's `ceiling` for a download from `place` (its address' origin) and
// returns its id for stopTuning(): a download from a place seen before in
// this run goes on with what was found there instead of measuring from 2
// again (each place keeps its own; one too small to measure changes
// nothing). Two at once (an app update during an install): the first
// started leads, and no span is judged while they share the line, since
// the bytes counted are both of theirs; once one is left it goes on with
// its own place. tunedConnections() is 0 while none runs (a caller keeps
// its own count then), else the count now, closing a span when one is due
// by the bytes the downloads counted (countReceived).
int startTuning(int ceiling, const std::string &place);
void stopTuning(int id);
int tunedConnections();
void tuneTrouble();
// Whether the running download's count is found (true when none is tuned).
bool tuningFound();
// The counts found, kept in `path` on the card across runs: the first
// download from a place after a launch starts at what was found there.
// Read on the first download, written when a download ends with a count
// found that differs from the one kept.
void rememberTuningIn(const std::string &path);
// "<place> <count>" a line, and back (lines that do not parse are skipped).
std::string serializeFound(const std::map<std::string, int> &found);
std::map<std::string, int> parseFound(const std::string &text);
} // namespace foil::net
