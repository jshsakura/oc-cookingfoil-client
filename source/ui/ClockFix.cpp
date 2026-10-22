#include "ui/Application.hpp"
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
} // namespace
void Application::pollClock() {
  if (clockDone_.exchange(false)) {
    clockWorker_.join();
    if (clockFinish_) std::exchange(clockFinish_, nullptr)();
  }
  if (!net::takeCertificateFailure() || clockAsked_ || clockFix_ != ClockFix::None || clockWorker_.joinable())
    return;
  const auto now = SDL_GetTicks();
  if (clockCheckedAt_ && now - clockCheckedAt_ < CheckEvery) return;
  clockCheckedAt_ = now;
  clockFix_ = ClockFix::Checking; // nothing shows until it is known to be the clock
  clockWorker_ = std::thread([this] {
    std::optional<int64_t> internet;
    guarded("clock", [&] { internet = net::networkTime(net::ntpServers()); });
    clockFinish_ = [this, internet] {
      clockFix_ = ClockFix::None;
      const auto console = int64_t(std::time(nullptr));
      if (!internet) {
        logLine("clock: no internet time to compare with");
        return;
      }
      if (std::llabs(*internet - console) <= ClockSlack) {
        logLine("clock: within " + std::to_string(ClockSlack) + "s of internet time; not the cause");
        return;
      }
      logLine("clock: console " + std::to_string(console) + ", internet " + std::to_string(*internet));
      clockInternet_ = *internet;
      clockAskedAt_ = SDL_GetTicks();
      clockAsked_ = true;
      clockFix_ = ClockFix::Ask;
    };
    clockDone_ = true;
  });
}
void Application::clockAction(Action a) {
  if (clockFix_ == ClockFix::Ask) {
    if (a == Action::Back) {
      clockFix_ = ClockFix::None;
      logLine("clock: the player kept the console clock");
    }
    if (a != Action::Accept || !settled() || clockWorker_.joinable()) return;
    clockFix_ = ClockFix::Setting;
    // Internet time as asked, moved on by the time the question was up.
    const auto target = clockInternet_ + int64_t((SDL_GetTicks() - clockAskedAt_) / 1000);
    clockWorker_ = std::thread([this, target] {
      platform::ClockSet set;
      guarded("clock", [&] { set = platform::setClock(target); });
      clockFinish_ = [this, set] {
        logLine(std::string(set.ok ? "clock set: " : "clock not set: ") + set.detail);
        if (!set.ok) {
          clockFix_ = ClockFix::Failed;
          return;
        }
        clockFix_ = ClockFix::None;
        // The connection the clock stopped, again.
        if (!connected_ && serverIndex_ < config_.servers.size()) connect(serverIndex_);
        message(tr("The console clock now matches internet time."));
      };
      clockDone_ = true;
    });
    return;
  }
  if (clockFix_ == ClockFix::Failed) {
    if (a == Action::Back) clockFix_ = ClockFix::None;
    if (a == Action::Accept && settled()) {
      // The log already holds each step's answer.
      clockFix_ = ClockFix::None;
      bugAsk_ = true;
      bugMessage_ = tr("The console clock could not be set.");
    }
  }
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
    const auto body = modal(720, 24 + 60 + 24, tr("Could not set the clock"), tr("Close"), "", tr("Send a bug report"),
                            ClockIcon);
    paint_.fonts.wrapped(tr("Set the date and time in System Settings > System. A bug report sends the log of what happened so it can be looked into."),
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
