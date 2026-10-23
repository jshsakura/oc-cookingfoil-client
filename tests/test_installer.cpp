#include "Test.hpp"
#include "core/TitleGuard.hpp"
#include "install/Installer.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
namespace foil::install {
void setHostInstall(std::function<void(const Item &)> run);
}
int runInstallerTests() {
  int failures = 0;
  foil::Item item;
  item.url = "https://host/game.nsz?signature=test";
  item.size = 4ULL * 1024 * 1024 * 1024;
  CHECK(!foil::install::preflight({item}, 10ULL * 1024 * 1024 * 1024));
  CHECK(foil::install::preflight({item}, 3ULL * 1024 * 1024 * 1024));
  CHECK(foil::install::preflight({item}, std::nullopt));
  CHECK(foil::install::preflight({}, 10ULL * 1024 * 1024 * 1024));
  {
    // A homebrew title's address ends in /download: its format is named.
    auto title = item;
    title.url = "https://host/api/homebrew/tv/download";
    CHECK(foil::install::preflight({title}, 10ULL * 1024 * 1024 * 1024));
    title.format = "nsp";
    CHECK(!foil::install::preflight({title}, 10ULL * 1024 * 1024 * 1024));
  }
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
  // A newer master key than the console's is never tried; the same key with
  // a lower version installs with a warning.
  CHECK(foil::install::masterKeyOf("1.0.0") == 0 && foil::install::masterKeyOf("9.2.0") == 10);
  CHECK(foil::install::masterKeyOf("12.1.0") == 11 && foil::install::masterKeyOf("20.5.0") == 19);
  CHECK(foil::install::masterKeyOf("21.0.0") == 20 && foil::install::masterKeyOf("23.1.0") == 22);
  CHECK(foil::install::unreachableFirmware({item, newer}, "18.1.0") == "19.0.0");
  auto patch = item;
  patch.requiredFirmware = "19.0.1";
  CHECK(foil::install::missingFirmware({patch}, "19.0.0") == "19.0.1");
  CHECK(foil::install::unreachableFirmware({patch}, "19.0.0").empty());
  CHECK(foil::install::unreachableFirmware({newer}, "").empty()); // unknown console: never blocks
  // Past the firmware: the file asks its server for the console's firmware,
  // and then counts as reachable.
  auto forced = newer;
  forced.url = foil::install::rewrapUrl("http://192.0.2.1/api/files/a.nsp", "18.1.0");
  CHECK(forced.url == "http://192.0.2.1/api/files/a.nsp?rewrap=18.1.0");
  CHECK(foil::install::rewrapUrl("http://192.0.2.1/f?t=1", "12.1.0") == "http://192.0.2.1/f?t=1&rewrap=12.1.0");
  CHECK(foil::install::rewrapUrl(forced.url, "12.1.0") == forced.url); // asked once
  CHECK(foil::install::rewrapped(forced.url) && !foil::install::rewrapped("http://192.0.2.1/rewrap=1"));
  CHECK(foil::install::unreachableFirmware({forced}, "18.1.0").empty());
  CHECK(foil::install::unreachableFirmware({forced, newer}, "18.1.0") == "19.0.0");
  // The key its NCAs use counts even when its own firmware field is lower.
  auto keyed = item;
  keyed.requiredFirmware = "18.0.0";
  keyed.keyFirmware = "20.0.0";
  CHECK(foil::install::unreachableFirmware({keyed}, "18.1.0") == "20.0.0");
  CHECK(foil::install::keyFirmwareOf({item, keyed, newer}) == "20.0.0" && foil::install::keyFirmwareOf({item}).empty());
  // A server that cannot re-encrypt says why, at the top or under "error".
  CHECK(foil::install::rewrapRefusal(R"({"code":"rewrap.keysTooOld","message":"keys too old"})") == "keys too old");
  CHECK(foil::install::rewrapRefusal(R"({"error":{"code":"rewrap.noKeys","message":"no keys"}})") == "no keys");
  // The server's own shape: the text in "error", the reason in "code".
  CHECK(foil::install::rewrapRefusal(R"({"error":"서버 키가 낮습니다","code":"rewrap.keysTooOld"})") == "서버 키가 낮습니다");
  CHECK(!foil::install::rewrapRefusal("<html>").empty());
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
    // Going on from an earlier try: the bar starts at what its saved points
    // say is on the card (this file's, on this target), not at 0.
    foil::install::Installer resumed;
    foil::Item game;
    game.url = "https://host/resumed.nsp";
    game.size = 4000;
    resumed.loadResumePoints({{"sd:aaaa", {game.url, "", 2000, "0-1000-600;1000-2000-1300"}},
                              {"sd:bbbb", {"https://host/other.nsp", "", 2000, "0-2000-2000"}},
                              {"nand:cccc", {game.url, "", 2000, "0-2000-2000"}}});
    std::atomic<uint64_t> seen{UINT64_MAX};
    foil::install::setHostInstall([&](const foil::Item &) { seen = resumed.progress().received; });
    CHECK(!resumed.start({game}, "Resumed", "", "sd", {}, {}, UINT64_MAX / 2));
    for (int i = 0; i < 300 && resumed.running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(seen == 900);
    foil::install::setHostInstall(nullptr);
  }
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
    // As other downloaders: a number from the first second, the last five
    // seconds eased, refreshed every second; the card's half-second bursts
    // read as their average.
    constexpr uint64_t MiB = 1 << 20;
    foil::install::SpeedWindow window;
    window.add(0, 0);
    CHECK(window.speed(0.5) < 0);
    window.add(1, 6 * MiB);
    CHECK(window.speed(1) > 0);
    window.add(4, 12 * MiB);
    uint64_t bytes = 12 * MiB;
    double low = 1e9, high = 0, previous = -1, at = -1;
    bool heldBetween = true;
    for (int step = 10; step <= 120; ++step) {
      bytes += (step % 2 ? 6 : 0) * MiB; // 6 MB in half a second, then nothing
      const double now = step * 0.5;
      window.add(now, bytes);
      const double shown = window.speed(now);
      CHECK(shown >= 0);
      if (previous >= 0 && shown != previous && now - at < 1)
        heldBetween = false;
      if (shown != previous)
        at = now;
      previous = shown;
      if (now >= 30) {
        low = std::min(low, shown);
        high = std::max(high, shown);
      }
    }
    CHECK(heldBetween);
    CHECK(low > 5 && high < 7);
    // A download held back (bursts at 10 MB/s, then waits on the card) shows
    // what really comes in, not the bursts: 4 MB/s here.
    {
      foil::install::SpeedWindow paced;
      uint64_t got = 0;
      paced.add(0, 0);
      double shown = 0;
      for (int step = 1; step <= 60; ++step) {
        const double now = step * 0.5;
        if (step % 10 < 4) got += 5 * MiB; // 2 s at 10 MB/s, then 3 s with nothing
        paced.add(now, got);
        shown = paced.speed(now);
      }
      CHECK(shown > 3 && shown < 5);
      CHECK(paced.average() > 3.5 && paced.average() < 4.5);
    }
    // A slow start shows at once, small.
    foil::install::SpeedWindow slow;
    slow.add(0, 0);
    slow.add(2, 1 * MiB);
    CHECK(slow.speed(2) > 0 && slow.speed(2) < 1);
    // Nothing more arrives: after ten seconds it shows 0, then picks up again.
    CHECK(window.speed(65) > 0);
    CHECK(window.speed(71) == 0);
    window.add(72, bytes + 8 * MiB);
    CHECK(window.speed(72) > 0);
    window.reset();
    CHECK(window.speed(140) < 0);
  }
  {
    // The time left counts down by the second and is worked out again every
    // ten: a close result keeps the count, a far one replaces it.
    constexpr uint64_t MiB = 1 << 20;
    foil::install::TimeLeft left;
    CHECK(left.update(0, 600 * MiB, 0) < 0); // no speed yet
    const double first = left.update(1, 600 * MiB, 10);
    CHECK(first > 66 && first < 67.5); // 600 MB at 10 MB/s, a tenth slower
    const double counted = left.update(8, 580 * MiB, 50); // not worked out again yet: counted down
    CHECK(counted > first - 7.5 && counted < first - 6.5);
    const double next = left.update(11, 550 * MiB, 5); // far off (122 against 57): replaced
    CHECK(next > 121 && next < 123);
    CHECK(left.update(12, 545 * MiB, 5) > next - 1.5 && left.update(12, 545 * MiB, 5) < next - 0.5);
    const double kept = left.update(21, 515 * MiB, 5); // 114 against 112: the count stays
    CHECK(kept > next - 10.5 && kept < next - 9.5);
    // A stall holds it; fifteen seconds of nothing make it unknown.
    const double held = left.update(23, 515 * MiB, 0);
    CHECK(held > kept - 2.5 && held < kept - 1.5 && left.update(30, 515 * MiB, 0) == held);
    CHECK(left.update(38, 515 * MiB, 0) < 0);
    CHECK(left.update(40, 550 * MiB, 10) > 0);
    CHECK(foil::install::timeLeftText(-1) == "남은 시간 계산 중");
    CHECK(foil::install::timeLeftText(42) == "1분 미만 남음");
    CHECK(foil::install::timeLeftText(3) == "1분 미만 남음");
    CHECK(foil::install::timeLeftText(200) == "약 4분 남음");
    CHECK(foil::install::timeLeftText(1500) == "약 25분 남음");
    // Whole hours drop the "0분"; past a day it counts days.
    CHECK(foil::install::timeLeftText(3600) == "약 1시간 남음");
    CHECK(foil::install::timeLeftText(3900) == "약 1시간 5분 남음");
    CHECK(foil::install::timeLeftText(26 * 3600) == "약 1일 2시간 남음");
    CHECK(foil::install::timeLeftText(48 * 3600) == "약 2일 남음");
    CHECK(foil::install::timeLeftText(1501) == "약 26분 남음");
    CHECK(foil::install::timeLeftText(5100) == "약 1시간 25분 남음");
  }
  {
    // After a long outage the speed is the link's again within half a
    // minute (an average since the start stayed low for long).
    constexpr uint64_t MiB = 1 << 20;
    foil::install::SpeedWindow window;
    uint64_t bytes = 0;
    window.add(0, 0);
    for (int step = 1; step <= 40; ++step) // 2 MB/s for 20 s
      window.add(step * 0.5, bytes += MiB);
    for (int step = 41; step <= 160; ++step) // 60 s of nothing
      window.add(step * 0.5, bytes);
    for (int step = 161; step <= 240; ++step) // 4 MB/s for 40 s
      window.add(step * 0.5, bytes += 2 * MiB);
    const double shown = window.speed(120);
    CHECK(shown > 3.6 && shown < 4.4);
  }
    {
    // Never a system title, whatever the signature check: below the first
    // application id, or a system kind of content, is refused.
    using foil::installAllowed;
    CHECK(!installAllowed(0x0100000000001000ULL, 0x80)); // the HOME Menu
    CHECK(!installAllowed(0x0100000000000809ULL, 0x80)); // system data
    CHECK(!installAllowed(0x0100000000000816ULL, 0x03)); // a system update
    CHECK(!installAllowed(0x0100000000000000ULL, 0x80) && !installAllowed(0, 0x80));
    CHECK(!installAllowed(0x01007EF00011E000ULL, 0x01)); // a game's id as a system program
    CHECK(installAllowed(0x01007EF00011E000ULL, 0x80) && installAllowed(0x01007EF00011E800ULL, 0x81) &&
          installAllowed(0x01007EF00011F001ULL, 0x82) && installAllowed(0x0595CE38EDAC1000ULL, 0x80));
    CHECK(!foil::catalogTitleAllowed("0100000000001000") && !foil::catalogTitleAllowed("010000000000FFFF"));
    CHECK(foil::catalogTitleAllowed("01007EF00011E000") && foil::catalogTitleAllowed("") && foil::catalogTitleAllowed("nope"));
  }
  return failures;
}
