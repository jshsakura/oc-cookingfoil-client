#include "Test.hpp"
#include "net/Tuner.hpp"
// The connections a download uses: from 2, a step up kept only when two
// spans show it more than a tenth faster, back to the better count
// otherwise and a longer wait before the next try; trouble steps down only
// when it lasts; the ceiling is shared among the consoles downloading.
int runTunerTests() {
  int failures = 0;
  using foil::net::ConnectionTuner;
  const uint64_t MB = 1 << 20;
  const auto run = [](ConnectionTuner &t, int spans, uint64_t bytes, int downloading = 0) {
    for (int i = 0; i < spans; ++i) t.span(bytes, 7, downloading);
    return t.connections();
  };
  {
    // Faster with each one: it climbs to the ceiling, a try at a time.
    ConnectionTuner tuner(4);
    CHECK(tuner.connections() == 2);
    CHECK(tuner.span(20 * MB, 7, 0) == 3); // after a span to know its speed, one more is tried
    CHECK(tuner.span(30 * MB, 7, 0) == 3); // judged over two spans
    CHECK(tuner.span(30 * MB, 7, 0) == 3); // +50%: kept
    CHECK(tuner.summary(0) == "connections 3 (tuned)");
    CHECK(tuner.span(40 * MB, 7, 0) == 4); // the next try soon
    CHECK(run(tuner, 2, 40 * MB) == 4);
    CHECK(run(tuner, 20, 40 * MB) == 4); // at the ceiling it stays
  }
  {
    // A try that brings nothing goes back, and the next try waits longer:
    // the count does not swing every few seconds.
    ConnectionTuner flat(4);
    CHECK(flat.span(20 * MB, 7, 0) == 3);
    CHECK(run(flat, 2, 21 * MB) == 2); // +5%: back to 2
    CHECK(run(flat, 5, 20 * MB) == 2); // waits 6 spans
    CHECK(flat.span(20 * MB, 7, 0) == 3); // then tries again
    CHECK(run(flat, 2, 20 * MB) == 2);
    CHECK(run(flat, 11, 20 * MB) == 2); // now 12 spans
    CHECK(flat.span(20 * MB, 7, 0) == 3);
    // Slower with one more: back as well.
    ConnectionTuner worse(4);
    CHECK(worse.span(20 * MB, 7, 0) == 3);
    CHECK(run(worse, 2, 15 * MB) == 2);
  }
  {
    // One blip changes nothing; trouble two spans in a row steps down.
    ConnectionTuner steady(2);
    CHECK(steady.span(20 * MB, 7, 0) == 2);
    steady.trouble();
    CHECK(steady.span(20 * MB, 7, 0) == 2);
    CHECK(steady.span(20 * MB, 7, 0) == 2); // a span without: the count stays
    steady.trouble();
    CHECK(steady.span(20 * MB, 7, 0) == 2);
    steady.trouble();
    CHECK(steady.span(20 * MB, 7, 0) == 1);
    CHECK(steady.summary(0) == "connections 1 (backed off)");
    // Never below 1.
    steady.trouble();
    steady.span(20 * MB, 7, 0);
    steady.trouble();
    CHECK(steady.span(20 * MB, 7, 0) == 1);
  }
  {
    // Paused or offline (no bytes): nothing is decided.
    ConnectionTuner idle(4);
    CHECK(idle.span(0, 7, 0) == 2 && idle.span(0, 7, 0) == 2);
    ConnectionTuner one(1);
    CHECK(one.connections() == 1 && run(one, 5, 10 * MB) == 1);
  }
  {
    // Two other consoles downloading: the ceiling of 4 is shared, 4 / 3 = 1,
    // at once.
    ConnectionTuner shared(4);
    CHECK(shared.ceiling(3) == 1 && shared.ceiling(0) == 4 && shared.ceiling(1) == 4);
    CHECK(shared.span(20 * MB, 7, 3) == 1);
    CHECK(shared.summary(3) == "connections 1 (shared), 2 others downloading");
    // One other: 4 / 2 = 2, and no try above it.
    ConnectionTuner half(4);
    CHECK(run(half, 10, 20 * MB, 2) == 2);
    CHECK(half.summary(2) == "connections 2 (start), 1 other downloading");
  }
  {
    // Found once a try went back or the ceiling is reached: before that the
    // time left is not shown (its speed is still climbing).
    ConnectionTuner fresh(4);
    CHECK(!fresh.found());
    ConnectionTuner flat(4);
    run(flat, 4, 20 * MB); // more never helps: the try goes back
    CHECK(flat.found() && flat.connections() == 2);
    ConnectionTuner one(1);
    run(one, 2, 20 * MB);
    CHECK(one.found());
  }
  {
    // The running tuner: 0 while none runs, the start count once begun.
    CHECK(foil::net::tunedConnections() == 0);
    int id = foil::net::startTuning(4, "https://first.example");
    CHECK(foil::net::tunedConnections() == 2);
    foil::net::stopTuning(id);
    CHECK(foil::net::tunedConnections() == 0);
    foil::net::stopTuning(id); // twice: nothing
    // Each place keeps its own: one elsewhere in between leaves it be.
    id = foil::net::startTuning(2, "https://second.example");
    CHECK(foil::net::tunedConnections() == 2);
    foil::net::stopTuning(id);
    id = foil::net::startTuning(1, "https://first.example");
    CHECK(foil::net::tunedConnections() == 1); // the first place's, under its new ceiling
    foil::net::stopTuning(id);
  }
  {
    // Two at once (an app update during an install): the first leads, the
    // second ending leaves it running, and the first ending hands over to
    // the one left with its own place.
    const int install = foil::net::startTuning(1, "https://install.example");
    const int update = foil::net::startTuning(8, "https://update.example");
    CHECK(foil::net::tunedConnections() == 1);
    foil::net::stopTuning(update);
    CHECK(foil::net::tunedConnections() == 1); // still the install's
    const int later = foil::net::startTuning(8, "https://update.example");
    foil::net::stopTuning(install);
    CHECK(foil::net::tunedConnections() == 2); // the update's own, from 2
    foil::net::stopTuning(later);
    CHECK(foil::net::tunedConnections() == 0);
  }
  {
    // What one download found, the next from the same place keeps: no
    // measuring from 2 again, and no try sooner than it was due.
    ConnectionTuner tuner(4);
    run(tuner, 1, 20 * MB);
    run(tuner, 2, 30 * MB); // 3 kept
    run(tuner, 1, 30 * MB); // trying 4
    run(tuner, 2, 30 * MB); // no better: back to 3, next try in 6 spans
    CHECK(tuner.connections() == 3);
    tuner.carryOn(4);
    CHECK(tuner.connections() == 3);
    CHECK(run(tuner, 5, 30 * MB) == 3); // still waiting out its try
    CHECK(tuner.summary(0) == "connections 3 (tuned)");
    // A lower ceiling for the next one applies at once.
    tuner.carryOn(2);
    CHECK(tuner.connections() == 2);
    // The line changes under a decided count (here: far slower for a while):
    // it tries again at once instead of waiting its turn.
    ConnectionTuner watch(4);
    run(watch, 1, 20 * MB);
    run(watch, 2, 21 * MB); // no better: back to 2, next try in 6 spans
    CHECK(watch.connections() == 2);
    CHECK(run(watch, 2, 20 * MB) == 2);
    watch.span(8 * MB, 7, 0);
    CHECK(watch.span(8 * MB, 7, 0) == 3); // more than 30% off: a try now
    // A fresh tuner says it kept nothing; one carried on from the start says "kept".
    ConnectionTuner fresh(4);
    fresh.carryOn(4);
    CHECK(fresh.summary(0) == "connections 2 (kept)");
  }
  return failures;
}
