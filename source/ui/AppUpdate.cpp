#include "ui/Application.hpp"
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <strings.h>
#include "core/Config.hpp"
#include "core/Log.hpp"
#include "core/Json.hpp"
#include "core/Version.hpp"
#include "net/ClientUpdateApi.hpp"
#include "platform/Storage.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
namespace foil::ui {
namespace {
// Launched with no usable argv (a forwarder, or the PC preview): the
// README's place on the SD card, which on the PC is the preview's fake SD.
std::string nroPath(const std::string &self, const std::string &sd) {
  const bool usable = self.rfind("sdmc:/", 0) == 0 && self.size() > 4 && self.compare(self.size() - 4, 4, ".nro") == 0;
  return usable ? self : sd + "/switch/cookingfoil/cookingfoil.nro";
}
} // namespace
void Application::checkAppUpdate() {
  if (updateWorker_.joinable()) return;
  if (!session_ || !connected_) {
    appUpdate_ = AppUpdate::Failed;
    appUpdateError_ = tr("Connect to a server to check.");
    return;
  }
  appUpdate_ = AppUpdate::Checking;
  const auto servers = serversWith("");
  const auto state = state_;
  updateWorker_ = std::thread([this, servers, state] {
    // The one in use says how the check went; another server with a newer
    // build is where it comes from.
    net::ClientUpdateReply reply;
    std::optional<update::Release> release;
    std::string from;
    guarded("update", [&] {
      for (size_t i = 0; i < servers.size(); ++i) {
        net::ClientUpdateReply theirs;
        auto found = net::latestClient(servers[i], state, theirs, &stop_);
        if (!i) reply = theirs;
        if (found && (!release || update::compareVersions(found->version, release->version) > 0)) {
          release = std::move(found);
          from = servers[i].url;
        }
      }
    });
    if (release && reply.missing) reply = {}; // the one in use has none, another has
    updateFinish_ = [this, reply, release, from] {
      appUpdateError_ = reply.error.message;
      if (reply.missing) appUpdate_ = AppUpdate::Missing;
      else if (!release) appUpdate_ = AppUpdate::Failed;
      else {
        appRelease_ = *release;
        appReleaseServer_ = from;
        appUpdate_ = update::compareVersions(release->version, COOKINGFOIL_VERSION) > 0 ? AppUpdate::Available
                                                                                      : AppUpdate::Latest;
        updateNewer_ = appUpdate_ == AppUpdate::Available;
      }
    };
    updateDone_ = true;
  });
}
// What the last update came to, said once at start when it went wrong:
// undone or not put in.
void Application::noticeRollBack() {
  if (selfPath_.empty()) return;
  std::error_code ec;
  const auto take = [&](const std::string &suffix, std::string &text) {
    const auto marker = selfPath_ + suffix;
    if (!std::filesystem::exists(marker, ec)) return false;
    text = readFile(marker);
    std::filesystem::remove(marker, ec);
    return true;
  };
  std::string text;
  if (take(".rolledback", text)) {
    alertTitle_ = tr("Update undone");
    alert_ = trf("The new version stopped before it opened, so v{0} is back. Wait for the next update.",
                 {COOKINGFOIL_VERSION});
    return;
  }
  if (take(".updatefailed", text)) {
    alertTitle_ = tr("Update failed");
    alert_ = text == "partial"
                 ? trf("The downloaded update was not complete, so v{0} stays. Get it again in Settings.", {COOKINGFOIL_VERSION})
                 : trf("The app's file could not be replaced, so v{0} stays. Try the update again.", {COOKINGFOIL_VERSION});
    return;
  }
  // A finished update says nothing: the marker only goes.
  std::string from;
  take(".updated", from);
}
void Application::installAppUpdate() {
  if (updateWorker_.joinable() || appUpdate_ != AppUpdate::Available || !session_) return;
  if (sdFree_ && *sdFree_ < appRelease_.size * 2) {
    alertTitle_ = tr("Cannot update");
    alert_ = trf("Not enough space on the SD card. {0} is needed.", {formatBytes(appRelease_.size * 2)});
    return;
  }
  appUpdate_ = AppUpdate::Downloading;
  updateReceived_ = 0;
  updateCancel_ = false;
  const auto server = serverFor(appReleaseServer_);
  const auto state = state_;
  const auto target = nroPath(selfPath_, sdRoot_);
  updateWorker_ = std::thread([this, server, state, release = appRelease_, target]() mutable {
    Error error;
    bool ok = false;
    guarded("update", [&] {
      // Asked again right before the download: the build on the server may
      // have changed since the app last looked.
      // A build put on the server while this one came down makes it differ
      // from what was asked: the newest is asked for and taken once more.
      for (int tries = 0; tries < 2 && !ok && !updateCancel_; ++tries) {
        net::ClientUpdateReply reply;
        const auto fresh = net::latestClient(server, state, reply, &updateCancel_);
        if (tries && (!fresh || fresh->sha256 == release.sha256)) break; // the same build: a real mismatch
        if (fresh) release = *fresh;
        updateReceived_ = 0;
        ok = net::installClient(server, state, release, target, updateReceived_, error, &updateCancel_);
      }
    });
    if (!ok) logLine("update failed: " + error.message);
    const bool cancelled = updateCancel_;
    updateFinish_ = [this, ok, error, cancelled, target, release] {
      if (!ok) {
        appUpdate_ = AppUpdate::Available;
        if (cancelled) message(tr("Cancelled the update."));
        else {
          alertTitle_ = tr("Update failed");
          alert_ = error.message;
        }
        return;
      }
      // Downloaded and checked as target + ".new"; the next start puts it in
      // place before anything opens the app's file (main.cpp). Swapping it
      // here, with the fonts still reading the running file, failed.
      appUpdate_ = AppUpdate::Latest;
      // The loader starts the new build as this one closes; without one the
      // player starts it again.
      if (platform::relaunch(target)) {
        exitRequested_ = requestQuit();
        return;
      }
      alertTitle_ = tr("Updated");
      alert_ = trf("v{0} installed. It takes effect when you start the app again.", {release.version});
    };
    updateDone_ = true;
  });
}
// The newest crash report Atmosphere wrote in the last day
// (atmosphere/crash_reports/<unix time>_<program id>.log), its start up to
// `room` bytes (the backtrace is near the top), or nothing.
std::string Application::recentCrashReport(size_t room) const {
  const std::string dir = sdRoot_ + "/atmosphere/crash_reports";
  std::error_code ec;
  std::string newest;
  long long newestAt = 0;
  for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
    const auto name = entry.path().filename().string();
    if (entry.path().extension() != ".log") continue;
    const long long at = std::atoll(name.c_str());
    if (at > newestAt) newestAt = at, newest = entry.path().string();
  }
  if (newest.empty() || newestAt < (long long)std::time(nullptr) - 24 * 3600) return {};
  auto text = readFile(newest);
  const std::string head = "\n# crash report " + std::filesystem::path(newest).filename().string() + "\n";
  if (text.size() + head.size() > room) text.resize(room > head.size() ? room - head.size() : 0);
  return head + text;
}
std::vector<size_t> Application::reportServers() const {
  std::vector<size_t> on;
  for (size_t i = 0; i < config_.servers.size(); ++i)
    if (config_.servers[i].enabled) on.push_back(i);
  return on;
}
// It goes where the lists come from unless the player picks another.
void Application::openBugReport(const std::string &note) {
  const auto on = reportServers();
  const auto open = std::find(on.begin(), on.end(), serverIndex_);
  bugServer_ = open == on.end() ? 0 : int(open - on.begin());
  bugMessage_ = note;
  bugAsk_ = true;
}
void Application::bugReportDialog() {
  constexpr int Shown = 3;
  constexpr float Row = 62, Gap = 8, Note = 88;
  const auto on = reportServers();
  const int count = int(on.size());
  const bool pick = count > 1; // one server: nothing to choose, as everywhere else
  const float listH = float(std::min(count, Shown)) * (Row + Gap) - Gap;
  const std::string where = pick ? tr("This goes to the server chosen above.\nTo reach the app's developer, open an issue on GitHub.")
                                 : tr("This goes to the server you are connected to.\nTo reach the app's developer, open an issue on GitHub.");
  const float width = 760, textW = width - 64;
  const float whereH = float(paint_.fonts.lineCount(where, 16, textW, 3)) * 21;
  const float bodyH = 24 + 26 + 16 + Note + (pick ? 24 + 30 + listH : 0) + 20 + whereH + 24;
  const auto body = modal(width, bodyH, tr("Send a bug report"), tr("Close"), tr("Write"), tr("Send"), 33);
  const float x = body.x + 32;
  float y = body.y + 24;
  paint_.fonts.text(tr("Write what went wrong if you like. The app's log goes with it."), x, y, 19, Secondary, false, textW);
  // The note: Y or a tap opens the keyboard.
  y += 26 + 16;
  hit(x, y, textW, Note, [this] { action(Action::Queue); });
  paint_.rect(x, y, textW, Note, Ground, 12);
  if (bugMessage_.empty())
    paint_.fonts.text(tr("Tap to write (optional)"), x + 20, y + 16, 19, Secondary);
  else
    paint_.fonts.wrapped(bugMessage_, x + 20, y + 14, 19, Ink, textW - 40, 3);
  y += Note;
  if (pick) {
    y += 24;
    paint_.fonts.middle(tr("Send to"), x, y + 11, 17, Ink, true);
    y += 30;
    bugServer_ = std::clamp(bugServer_, 0, count - 1);
    // Rows of 설정's servers: its dot, its name and address; the cursor's
    // row is the one it goes to. Past three it scrolls, by the d-pad or a finger.
    const float rowW = textW - (count > Shown ? 18 : 0);
    const float top = glide("bugServers", x, y, rowW, listH, float(std::clamp(bugServer_ - Shown / 2, 0, std::max(0, count - Shown))),
                            float(std::max(0, count - Shown)), Row + Gap);
    clipTo(x, y, rowW, listH);
    for (int n = int(top); n < count && n <= int(top) + Shown; ++n) {
      const auto &server = config_.servers[on[size_t(n)]];
      const float ry = y + (float(n) - top) * (Row + Gap), mid = ry + Row / 2;
      const bool chosen = n == bugServer_;
      hit(x, ry, rowW, Row, [this, n] { bugServer_ = n; });
      if (chosen) paint_.focusRow(x, ry, rowW, Row, Ground, 12);
      else paint_.rect(x, ry, rowW, Row, Ground, 12);
      paint_.rect(x + 20, mid - 6, 12, 12, serverDot(server.url), 6);
      const auto host = hostOf(server.url);
      const float nameX = x + 46, nameW = rowW - 46 - 56;
      if (host.empty() || host == server.title) {
        paint_.fonts.middle(server.title, nameX, mid, 19, Ink, true, nameW);
      } else {
        paint_.fonts.middle(server.title, nameX, mid - 11, 19, Ink, true, nameW);
        paint_.fonts.middle(host, nameX, mid + 13, 15, Secondary, false, nameW);
      }
      if (chosen) paint_.icon(8, x + rowW - 44, mid - 13, Good, 26);
    }
    unclip();
    if (count > Shown) paint_.scrollbar(x + rowW + 8, y, listH, top, Shown, float(count));
    y += listH;
  }
  y += 20;
  paint_.fonts.wrapped(where, x, y, 16, Secondary, textW, 3, false, 1.3f);
}
void Application::sendBugReport(const std::string &said) {
  if (bugWorker_.joinable()) return;
  const size_t to = bugReportServer();
  if (to == std::string::npos) {
    message(tr("Connect to a server to send a bug report."));
    return;
  }
  const auto server = config_.servers[to];
  const auto state = state_;
  // The server takes 64 KB at most; the end of the log is what tells.
  // What the player wrote comes first, then the log, then the console's
  // report of the app's last crash when one is from the last day (it says
  // where the app stopped, which the log cannot).
  const auto note = said.empty() ? std::string() : "message: " + said + "\n\n";
  bugWorker_ = std::thread([this, server, state, note] {
    Error error;
    bool ok = false;
    guarded("bug report", [&] {
      constexpr size_t Limit = 60 * 1024, CrashRoom = 20 * 1024;
      const auto crash = recentCrashReport(CrashRoom);
      const auto log = note + logTail(Limit - std::min<size_t>(note.size(), 8 * 1024) - crash.size()) + crash;
      ok = net::sendBugReport(server, state, log, error, &stop_);
    });
    incomingBugStatus_ = ok ? std::string(tr("Sent. Thank you.")) : error.message;
    bugDone_ = true;
  });
}
void Application::pollLog() {
  if (bugDone_.exchange(false)) {
    bugWorker_.join();
    message(incomingBugStatus_); // said once; the row reads as before
  }
  // An error popup that just appeared goes into the log on the card.
  if (!alert_.empty() && alert_ != lastAlert_) {
    lastAlert_ = alert_;
    logLine("alert: " + alertTitle_ + " | " + alert_);
  } else if (alert_.empty())
    lastAlert_.clear();
}
void Application::pollAppUpdate() {
  if (!updateDone_.exchange(false)) return;
  updateWorker_.join();
  if (updateFinish_) std::exchange(updateFinish_, nullptr)();
}
std::string Application::appUpdateStatus() const {
  const std::string current = std::string("v") + COOKINGFOIL_VERSION;
  switch (appUpdate_) {
  case AppUpdate::Checking: return trf("{0} · Checking", {current});
  case AppUpdate::Latest: return trf("{0} · Up to date", {current});
  case AppUpdate::Available: return trf("v{0} update available", {appRelease_.version});
  case AppUpdate::Missing: return trf("{0} · The server offers no update", {current});
  case AppUpdate::Failed: return current + " · " + appUpdateError_;
  case AppUpdate::Downloading:
    return trf("Downloading {0} / {1}", {formatBytes(updateReceived_.load()), formatBytes(appRelease_.size)});
  default: return trf("{0} · Press A to check", {current});
  }
}
// A on the row: check, or with a newer build ask before getting it.
void Application::appUpdateAction(Action a) {
  if (appUpdate_ == AppUpdate::Downloading) {
    if (a == Action::Back) updateCancel_ = true;
    return;
  }
  if (appUpdateAsk_) {
    if (a == Action::Back) appUpdateAsk_ = false;
    if (a == Action::Accept && settled()) {
      appUpdateAsk_ = false;
      installAppUpdate();
    }
    return;
  }
  if (a != Action::Accept) return;
#ifndef __SWITCH__
  // The PC preview has no .nro to replace: say where it works instead.
  if (appUpdate_ == AppUpdate::Available) {
    alertTitle_ = tr("Cannot update");
    alert_ = tr("The PC preview cannot replace the app. Get it on the console in Settings > App update.");
    return;
  }
#endif
  if (appUpdate_ == AppUpdate::Available) appUpdateAsk_ = true;
  else if (appUpdate_ != AppUpdate::Checking) checkAppUpdate();
}
void Application::appUpdateDialog() {
  if (appUpdate_ == AppUpdate::Downloading) {
    const auto body = modal(640, 24 + 60 + 24, tr("Downloading the update"), tr("Cancel"), "", NoAccept, 4);
    const float ratio = appRelease_.size ? std::min(1.f, float(updateReceived_.load()) / float(appRelease_.size)) : 0.f;
    paint_.fonts.text("v" + appRelease_.version + " · " + formatBytes(updateReceived_.load()) + " / " +
                          formatBytes(appRelease_.size),
                      body.x + 32, body.y + 24, 19, Ink, true, body.w - 64);
    paint_.rect(body.x + 32, body.y + 66, body.w - 64, 10, Line, 5);
    paint_.rect(body.x + 32, body.y + 66, std::max(10.f, (body.w - 64) * ratio), 10, Butter, 5);
    return;
  }
  const auto body = modal(660, 24 + 60 + 24, tr("Update now?"), tr("Cancel"), "", tr("Update|action"), 3);
  paint_.fonts.wrapped(trf("Goes from v{0} to v{1}. The app starts again once it is in.",
                           {COOKINGFOIL_VERSION, appRelease_.version}),
                       body.x + 32, body.y + 24, 19, Secondary, body.w - 64, 2);
}
} // namespace foil::ui
