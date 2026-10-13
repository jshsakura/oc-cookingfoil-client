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
    // The speed shown is the last ten seconds' average: bursts and stalls
    // second to second leave it (and the time left) steady.
    constexpr uint64_t MiB = 1 << 20;
    foil::install::SpeedWindow window;
    CHECK(window.add(0, 0) == 0);
    CHECK(window.add(1, 30 * MiB) == 0); // too soon to say
    uint64_t bytes = 30 * MiB;
    double low = 1e9, high = 0;
    for (int second = 2; second <= 30; ++second) {
      bytes += (second % 2 ? 30 : 10) * MiB; // 30 MB, then 10 MB, ...
      const double speed = window.add(second, bytes);
      if (second >= 12) {
        low = std::min(low, speed);
        high = std::max(high, speed);
      }
    }
    CHECK(low > 18 && high < 22);
    window.reset();
    CHECK(window.add(31, bytes) == 0);
  }
  return failures;
}
