#pragma once
#include "core/I18n.hpp"
#include "install/EngineHooks.hpp"
#include <algorithm>
#include <cstdint>
#include <string>
#include <switch.h>
#include <vector>
namespace inst::ui {
struct MainApplication {
  // The engine asks two things: whether to keep going after a network drop
  // (its own three quick retries already failed), and whether to install an
  // NCA that failed verification (always no).
  int CreateShowDialog(const std::string &title, const std::string &,
                       const std::vector<std::string> &, bool) {
    if (title.find("retry") == std::string::npos)
      return 0; // nca_verify: option 0 is cancel
    // A dropped connection is waited out: each round waits longer (10 s up
    // to a minute), for about ten minutes in all, then gives up. The engine
    // resumes at the byte where it stopped, so nothing is downloaded twice.
    constexpr int MaxRounds = 14;
    auto *sink = foil::install::engineSink();
    if (++retryRounds_ > MaxRounds)
      return 1;
    if (sink)
      sink->phase(foil::tr("Connection lost. Reconnecting."));
    const int waitSeconds = std::min(60, 10 * retryRounds_);
    for (int tenth = 0; tenth < waitSeconds * 10; ++tenth) {
      if (sink && sink->cancellationRequested())
        return 1;
      svcSleepThread(100'000'000ULL);
    }
    return 0;
  }
  // A new install starts with a fresh retry budget.
  void ResetRetries() { retryRounds_ = 0; }
  void RefreshInputDevice(bool = false) {}
  void CallForRender() {}
  void UpdateButtons() {}
  uint64_t GetButtonsDown() { return 0; }

private:
  int retryRounds_ = 0;
};
extern MainApplication *mainApp;
} // namespace inst::ui
