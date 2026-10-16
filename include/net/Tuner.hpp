#pragma once
// How many connections a download uses, found as it goes instead of fixed:
// it starts at 2 (or the ceiling, when lower), and every span it compares
// the bytes that came with the span before. A step up that brings more
// than a tenth more goes on up to the ceiling; one that brings nothing
// stays; a drop, an error, a retry or a timeout steps down (never below 1).
// The ceiling is the server's (maxConnections), shared out among the
// consoles downloading from it now (X-CookingFoil-Active-Downloads).
#include <cstdint>
#include <string>
namespace foil::net {
class ConnectionTuner {
public:
  static constexpr double SpanSeconds = 7, Gain = 1.10;
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

private:
  int ceiling_ = 1, now_ = 1;
  bool probing_ = false, trouble_ = false;
  double before_ = 0; // the last span's bytes a second, at the count before the step
  std::string reason_ = "start";
};
// The one running download's tuner. startTuning() begins one under the
// server's `ceiling`; tunedConnections() is 0 while none runs (a caller
// keeps its own count then), else the count now, closing a span when one
// is due by the bytes the downloads counted (countReceived).
void startTuning(int ceiling);
void stopTuning();
int tunedConnections();
void tuneTrouble();
std::string tuningSummary();
// X-CookingFoil-Active-Downloads: consoles downloading from the server now,
// this one included; 0 when the server does not say.
void heardActiveDownloads(int count);
int activeDownloads();
} // namespace foil::net
