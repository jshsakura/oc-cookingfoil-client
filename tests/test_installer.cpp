#include "Test.hpp"
#include "install/Installer.hpp"
#include <algorithm>
int runInstallerTests() {
  int failures = 0;
  foil::Item item;
  item.url = "https://host/game.nsz?signature=test";
  item.size = 4ULL * 1024 * 1024 * 1024;
  CHECK(!foil::install::preflight({item}, 10ULL * 1024 * 1024 * 1024));
  CHECK(foil::install::preflight({item}, 3ULL * 1024 * 1024 * 1024));
  CHECK(foil::install::preflight({item}, std::nullopt));
  CHECK(foil::install::preflight({}, 10ULL * 1024 * 1024 * 1024));
  auto unsupported = item;
  unsupported.url = "https://host/app.nro";
  CHECK(foil::install::preflight({unsupported},
                                 10ULL * 1024 * 1024 * 1024));
  auto huge = item;
  huge.size = UINT64_MAX;
  CHECK(foil::install::preflight({huge, huge}, UINT64_MAX));
  // A compressed .nsz unpacks on the console: room is checked against the
  // installed size the server reports, not the smaller download.
  auto packed = item;
  packed.size = 2ULL * 1024 * 1024 * 1024;
  packed.installSize = 6ULL * 1024 * 1024 * 1024;
  CHECK(foil::install::preflight({packed}, 4ULL * 1024 * 1024 * 1024));
  CHECK(!foil::install::preflight({packed}, 7ULL * 1024 * 1024 * 1024));
  CHECK(foil::install::compareVersions("16.0.3", "17.0.0") < 0);
  CHECK(foil::install::compareVersions("18.1.0", "18.1") == 0);
  CHECK(foil::install::compareVersions("10.0.0", "9.2.0") > 0);
  CHECK(foil::install::compareVersions("18.1.0", "18.1.0") == 0);
  auto newer = item;
  newer.requiredFirmware = "19.0.0";
  CHECK(foil::install::missingFirmware({item, newer}, "18.1.0") == "19.0.0");
  CHECK(foil::install::missingFirmware({newer}, "19.0.1").empty());
  CHECK(foil::install::missingFirmware({newer}, "").empty());
  CHECK(foil::install::missingFirmware({item}, "1.0.0").empty());
  foil::install::Installer installer;
  auto error = installer.start({item}, "Title", "", "sd", {}, {});
  CHECK(error);
  CHECK(!installer.running());
  CHECK(installer.progress().status == foil::install::Status::Idle);
  foil::install::Progress preview;
  preview.status = foil::install::Status::Running;
  preview.total = 1000;
  installer.preview(preview);
  installer.bytes("nca-a", 300, 10);
  installer.bytes("nca-b", 150, 10);
  CHECK(installer.progress().received == 450);
  installer.bytes("nca-a", 350, 10);
  CHECK(installer.progress().received == 500);
  CHECK(installer.progress().percent == 50);
  installer.cancel();
  CHECK(installer.cancellationRequested());
  {
    // A paused install says so, and the engine's own progress lines do not
    // cover it until it resumes; it comes back to downloading.
    foil::install::Installer installer;
    foil::install::Progress running;
    running.status = foil::install::Status::Running;
    running.speed = 12.5;
    installer.preview(running);
    installer.setPaused(true);
    CHECK(installer.paused());
    CHECK(installer.progress().phase == "일시정지했습니다.");
    CHECK(installer.progress().speed == 0);
    installer.phase("다운로드하고 있습니다.");
    CHECK(installer.progress().phase == "일시정지했습니다.");
    installer.setPaused(false);
    CHECK(!installer.paused());
    CHECK(installer.progress().phase == "다운로드하고 있습니다.");
  }
  {
    // The speed shown is the last four seconds' bytes (eight half-second
    // slots): the card's write pauses, bursts then nothing, average out.
    constexpr uint64_t MiB = 1 << 20;
    foil::install::SpeedWindow window;
    window.add(0, 0);
    CHECK(window.speed(1) < 0); // too soon to say
    uint64_t bytes = 0;
    double low = 1e9, high = 0;
    for (int step = 1; step <= 60; ++step) {
      bytes += (step % 2 ? 8 : 0) * MiB; // 8 MB in half a second, then nothing
      const double now = step * 0.5;
      window.add(now, bytes);
      if (now >= 5) {
        low = std::min(low, window.speed(now));
        high = std::max(high, window.speed(now));
      }
    }
    CHECK(low > 7 && high < 9.5);
    // Nothing more arrives: it falls to zero instead of holding the last value.
    CHECK(window.speed(36) == 0);
    // A smaller total is the next file starting: no negative or huge jump.
    window.add(36.5, 4 * MiB);
    CHECK(window.speed(36.5) >= 0 && window.speed(36.5) < 9.5);
    window.reset();
    CHECK(window.speed(40) < 0);
  }
  {
    // The time left follows a slow speed and counts down like a clock.
    constexpr uint64_t MiB = 1 << 20;
    foil::install::TimeLeft left;
    CHECK(left.update(0, 600 * MiB, 0) < 0); // no speed yet
    const double first = left.update(1, 600 * MiB, 10);
    CHECK(first > 59 && first < 61);
    // A slow second (0.1 MB/s) moves it only a little.
    const double next = left.update(2, 590 * MiB, 0.1);
    CHECK(next > first - 20 && next < first + 20);
    // Within the same second it holds still.
    CHECK(left.update(2.5, 585 * MiB, 50) == next);
    // Nothing arriving keeps counting down instead of growing to hours...
    const double stalled = left.update(4, 580 * MiB, 0);
    CHECK(stalled > 0 && stalled <= next);
    // ...and after four seconds of nothing it is unknown again.
    CHECK(left.update(8, 580 * MiB, 0) < 0);
    CHECK(left.update(9, 580 * MiB, 10) > 0);
    CHECK(foil::install::timeLeftText(-1) == "남은 시간 계산 중");
    CHECK(foil::install::timeLeftText(42) == "약 42초 남음");
    CHECK(foil::install::timeLeftText(200) == "약 3분 20초 남음");
    CHECK(foil::install::timeLeftText(1500) == "약 25분 남음");
    CHECK(foil::install::timeLeftText(5100) == "약 1시간 25분 남음");
  }
  return failures;
}
