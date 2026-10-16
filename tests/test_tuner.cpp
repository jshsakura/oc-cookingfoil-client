#include "Test.hpp"
#include "net/Tuner.hpp"
// The connections a download uses: from 2, a step up kept while it brings
// more than a tenth more, back on a drop or trouble, under a ceiling shared
// among the consoles downloading.
int runTunerTests() {
  int failures = 0;
  using foil::net::ConnectionTuner;
  const uint64_t MB = 1 << 20;
  {
    ConnectionTuner tuner(4);
    CHECK(tuner.connections() == 2);
    CHECK(tuner.span(20 * MB, 7, 0) == 3); // a step up is tried
    CHECK(tuner.span(30 * MB, 7, 0) == 4); // +50%: up again
    CHECK(tuner.span(31 * MB, 7, 0) == 4); // at the ceiling, little gain: stays
    CHECK(tuner.span(31 * MB, 7, 0) == 4);
    CHECK(tuner.summary(0) == "connections 4 (tuned)");
    tuner.trouble();
    CHECK(tuner.span(31 * MB, 7, 0) == 3); // trouble: one fewer
    CHECK(tuner.summary(0) == "connections 3 (backed off)");
  }
  {
    // A step up that brings nothing stays; one that makes it slower goes back.
    ConnectionTuner flat(4);
    CHECK(flat.span(20 * MB, 7, 0) == 3);
    CHECK(flat.span(21 * MB, 7, 0) == 3);
    CHECK(flat.span(25 * MB, 7, 0) == 3); // decided: no more probing
    ConnectionTuner worse(4);
    CHECK(worse.span(20 * MB, 7, 0) == 3);
    CHECK(worse.span(15 * MB, 7, 0) == 2);
  }
  {
    // Paused or offline (no bytes): nothing is decided.
    ConnectionTuner idle(4);
    CHECK(idle.span(0, 7, 0) == 2 && idle.span(0, 7, 0) == 2);
    // Never below 1, never above the ceiling; a ceiling of 1 starts there.
    ConnectionTuner one(1);
    CHECK(one.connections() == 1 && one.span(10 * MB, 7, 0) == 1);
    one.trouble();
    CHECK(one.span(10 * MB, 7, 0) == 1);
  }
  {
    // Two other consoles downloading: the ceiling of 4 is shared, 4 / 3 = 1.
    ConnectionTuner shared(4);
    CHECK(shared.ceiling(3) == 1 && shared.ceiling(0) == 4 && shared.ceiling(1) == 4);
    CHECK(shared.span(20 * MB, 7, 3) == 1);
    CHECK(shared.summary(3) == "connections 1 (shared), 2 others downloading");
    // Alone again: it tries its way back up.
    CHECK(shared.span(10 * MB, 7, 1) == 2);
    // One other: 4 / 2 = 2.
    ConnectionTuner half(4);
    CHECK(half.span(20 * MB, 7, 2) == 2 && half.span(30 * MB, 7, 2) == 2);
    CHECK(half.summary(2) == "connections 2 (start), 1 other downloading");
  }
  {
    // The running tuner: 0 while none runs, the start count once begun.
    foil::net::stopTuning();
    CHECK(foil::net::tunedConnections() == 0);
    foil::net::startTuning(4);
    CHECK(foil::net::tunedConnections() == 2);
    foil::net::stopTuning();
    CHECK(foil::net::tunedConnections() == 0);
  }
  return failures;
}
