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
    // The speed shown waits until it has measured (five seconds and 4 MB),
    // averages ten seconds and changes only every three: bursts from the card
    // do not make the number jump.
    constexpr uint64_t MiB = 1 << 20;
    foil::install::SpeedWindow window;
    window.add(0, 0);
    CHECK(window.speed(1) < 0);
    window.add(4, 12 * MiB);
    CHECK(window.speed(4) < 0); // enough bytes, too soon
    uint64_t bytes = 12 * MiB;
    double low = 1e9, high = 0, previous = -1, at = -1;
    bool heldBetween = true;
    for (int step = 10; step <= 120; ++step) {
      bytes += (step % 2 ? 6 : 0) * MiB; // 6 MB in half a second, then nothing
      const double now = step * 0.5;
      window.add(now, bytes);
      const double shown = window.speed(now);
      CHECK(shown >= 0);
      if (previous >= 0 && shown != previous && now - at < 3)
        heldBetween = false;
      if (shown != previous)
        at = now;
      previous = shown;
      if (now >= 40) {
        low = std::min(low, shown);
        high = std::max(high, shown);
      }
    }
    CHECK(heldBetween);
    CHECK(low > 5.2 && high < 6.8);
    // A slow start (little data) still shows after fifteen seconds.
    foil::install::SpeedWindow slow;
    slow.add(0, 0);
    slow.add(10, 1 * MiB);
    CHECK(slow.speed(10) < 0);
    slow.add(15, 2 * MiB);
    CHECK(slow.speed(15) >= 0);
    // Nothing more arrives: it eases down toward zero.
    double later = window.speed(61);
    for (double t = 62; t < 130; t += 0.5)
      later = window.speed(t);
    CHECK(later == 0);
    // A smaller total is the next file starting: no negative or huge jump.
    window.add(130.5, 4 * MiB);
    CHECK(window.speed(130.5) >= 0 && window.speed(130.5) < 6.8);
    window.reset();
    CHECK(window.speed(140) < 0);
  }
  {
    // The time left changes once every ten seconds and holds between.
    constexpr uint64_t MiB = 1 << 20;
    foil::install::TimeLeft left;
    CHECK(left.update(0, 600 * MiB, 0) < 0); // no speed yet
    const double first = left.update(1, 600 * MiB, 10);
    CHECK(first > 59 && first < 61);
    CHECK(left.update(8, 580 * MiB, 50) == first); // held
    // A slow moment moves it only a little.
    const double next = left.update(11, 550 * MiB, 1);
    CHECK(next > first - 20 && next < first + 120);
    // A short stall holds it; fifteen seconds of nothing make it unknown.
    CHECK(left.update(13, 550 * MiB, 0) == next);
    CHECK(left.update(28, 550 * MiB, 0) < 0);
    CHECK(left.update(29, 550 * MiB, 10) > 0);
    CHECK(foil::install::timeLeftText(-1) == "남은 시간 계산 중");
    CHECK(foil::install::timeLeftText(42) == "약 50초 남음");
    CHECK(foil::install::timeLeftText(3) == "약 10초 남음");
    CHECK(foil::install::timeLeftText(200) == "약 3분 남음");
    CHECK(foil::install::timeLeftText(1500) == "약 25분 남음");
    CHECK(foil::install::timeLeftText(5100) == "약 1시간 25분 남음");
  }
  return failures;
}
