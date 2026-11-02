#include "ui/Application.hpp"
#include "platform/Cores.hpp"
#include "core/Log.hpp"
#include "net/Api.hpp"
#include "net/Ntp.hpp"
#include "platform/Clock.hpp"
#include <cstdlib>
#include <ctime>
namespace foil::ui {
namespace {
// Further off than this, the clock may be why a certificate did not check
// out; closer, the fault is elsewhere and nothing is asked.
constexpr int64_t ClockSlack = 120;
constexpr uint32_t CheckEvery = 60 * 1000;
// A console clock before this day (2026-11-01 UTC) is wrong for certain: a
// console whose clock was reset reads 2024-01-01. Such a clock is set at
// start, without a question, before a connection has to fail on it.
constexpr int64_t ClockFloor = 1793491200;
bool clockSurelyWrong() { return int64_t(std::time(nullptr)) < ClockFloor; }
} // namespace
void Application::pollClock() {
  if (clockDone_.exchange(false)) {
    clockWorker_.join();
    if (clockFinish_) std::exchange(clockFinish_, nullptr)();
  }
  const bool atStart = !clockStartChecked_;
  clockStartChecked_ = true;
  if (atStart && clockSurelyWrong()) logLine("clock: before " + platform::localTime(ClockFloor) + " at start");
  else if (!net::takeCertificateFailure()) return;
  if (clockAsked_ || clockFix_ != ClockFix::None || clockWorker_.joinable()) return;
  const auto now = SDL_GetTicks();
  if (!atStart && clockCheckedAt_ && now - clockCheckedAt_ < CheckEvery) return;
  clockCheckedAt_ = now;
  clockFix_ = ClockFix::Checking; // nothing shows until it is known to be the clock
  clockWorker_ = std::thread([this] {
    const platform::OnCore off(platform::NetworkCore);
    std::optional<int64_t> internet;
    guarded("clock", [&] { internet = net::networkTime(net::ntpServers()); });
    clockFinish_ = [this, internet] {
      clockFix_ = ClockFix::None;
      const auto console = int64_t(std::time(nullptr));
      if (!internet || std::llabs(*internet - console) <= ClockSlack) return;
      clockInternet_ = *internet;
      clockAskedAt_ = SDL_GetTicks();
      clockAsked_ = true;
      // Nothing to ask about a clock that is surely wrong.
      if (clockSurelyWrong()) setClockNow();
      else clockFix_ = ClockFix::Ask;
    };
    clockDone_ = true;
  });
}
void Application::setClockNow() {
  clockFix_ = ClockFix::Setting;
  // Internet time as read, moved on by the time since.
  const auto target = clockInternet_ + int64_t((SDL_GetTicks() - clockAskedAt_) / 1000);
  clockWorker_ = std::thread([this, target] {
    const platform::OnCore off(platform::NetworkCore);
    platform::ClockSet set;
    guarded("clock", [&] { set = platform::setClock(target); });
    clockFinish_ = [this, set] {
      if (!set.ok) {
        logLine("clock not set: " + set.detail);
        // No report can go now, since the clock stops it too: it goes on its
        // own once a server answers, after the clock is set by hand.
        keepReport("The console clock could not be set.");
        clockFix_ = ClockFix::Failed;
        return;
      }
      logLine("clock set: " + set.detail);
      clockFix_ = ClockFix::None;
      // The connection the clock stopped, again.
      if (!connected_ && serverIndex_ < config_.servers.size()) connect(serverIndex_);
      message(tr("The console clock now matches internet time."));
    };
    clockDone_ = true;
  });
}
void Application::clockAction(Action a) {
  if (clockFix_ == ClockFix::Ask) {
    if (a == Action::Back) clockFix_ = ClockFix::None;
    if (a == Action::Accept && settled() && !clockWorker_.joinable()) setClockNow();
    return;
  }
  if (clockFix_ == ClockFix::Failed && (a == Action::Back || (a == Action::Accept && settled())))
    clockFix_ = ClockFix::None;
}
void Application::clockDialog() {
  constexpr int ClockIcon = 30;
  if (clockFix_ == ClockFix::Setting) {
    const auto body = modal(640, 24 + 30 + 24, tr("Setting the clock"), "", "", "", ClockIcon);
    paint_.fonts.text(tr("Setting the console clock to internet time."), body.x + 32, body.y + 24, 19, Secondary, false,
                      body.w - 64);
    return;
  }
  if (clockFix_ == ClockFix::Failed) {
    const auto body = modal(720, 24 + 60 + 24, tr("Could not set the clock"), "", "", tr("Close"), ClockIcon);
    paint_.fonts.wrapped(tr("Set the date and time in System Settings > System, then open the app again. What happened goes to the server on its own once it connects."),
                         body.x + 32, body.y + 24, 19, Secondary, body.w - 64, 3);
    return;
  }
  const auto body = modal(760, 24 + 30 + 12 + 60 + 24, tr("The console clock is off"), tr("Keep it"), "",
                          tr("Set the clock"), ClockIcon);
  const auto now = clockInternet_ + int64_t((SDL_GetTicks() - clockAskedAt_) / 1000);
  paint_.fonts.text(trf("Console {0}  ·  Internet {1}", {platform::localTime(std::time(nullptr)), platform::localTime(now)}),
                    body.x + 32, body.y + 24, 20, Ink, true, body.w - 64);
  paint_.fonts.wrapped(tr("With the clock off, secure connections to the server fail. Set it to internet time? The region and time zone stay as they are."),
                       body.x + 32, body.y + 24 + 30 + 12, 19, Secondary, body.w - 64, 3);
}
} // namespace foil::ui
