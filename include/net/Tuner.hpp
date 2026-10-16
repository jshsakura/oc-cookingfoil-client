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
#include <string>
namespace foil::net {
class ConnectionTuner {
public:
  static constexpr double SpanSeconds = 7, Gain = 1.10, Shift = 0.30;
  static constexpr int TrySpans = 2, RetryAfter = 6, MostWait = 24;
  explicit ConnectionTuner(int ceiling = 0);
  int connections() const { return now_; }
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
// The one running download's tuner. startTuning() begins one under the
// server's `ceiling` for a download from `place` (its address' origin):
// queued downloads from the same place one after another keep what the
// last one found instead of measuring from 2 again; another place starts
// over. tunedConnections() is 0 while none runs (a caller keeps its own
// count then), else the count now, closing a span when one is due by the
// bytes the downloads counted (countReceived).
void startTuning(int ceiling, const std::string &place);
void stopTuning();
int tunedConnections();
void tuneTrouble();
std::string tuningSummary();
// X-CookingFoil-Active-Downloads: consoles downloading from the server now,
// this one included; 0 when the server does not say.
void heardActiveDownloads(int count);
int activeDownloads();
} // namespace foil::net
