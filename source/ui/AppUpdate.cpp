#include <filesystem>
#include "ui/Application.hpp"
#include <algorithm>
#include <strings.h>
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
  const auto server = session_->server();
  const auto state = state_;
  updateWorker_ = std::thread([this, server, state] {
    net::ClientUpdateReply reply;
    std::optional<update::Release> release;
    guarded("update", [&] { release = net::latestClient(server, state, reply, &stop_); });
    updateFinish_ = [this, reply, release] {
      appUpdateError_ = reply.error.message;
      if (reply.missing) appUpdate_ = AppUpdate::Missing;
      else if (!release) appUpdate_ = AppUpdate::Failed;
      else {
        appRelease_ = *release;
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
  const auto server = session_->server();
  const auto state = state_;
  const auto target = nroPath(selfPath_, sdRoot_);
  updateWorker_ = std::thread([this, server, state, release = appRelease_, target]() mutable {
    Error error;
    bool ok = false;
    guarded("update", [&] {
      // Asked again right before the download: the build on the server may
      // have changed since the app last looked.
      net::ClientUpdateReply reply;
      if (auto fresh = net::latestClient(server, state, reply, &updateCancel_)) release = *fresh;
      ok = net::installClient(server, state, release, target, updateReceived_, error, &updateCancel_);
    });
    logLine(ok ? "update: " + release.version + " -> " + target : "update failed: " + error.message);
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
void Application::sendBugReport(const std::string &said) {
  if (bugWorker_.joinable()) return;
  if (!session_ || !connected_) {
    message(tr("Connect to a server to send a bug report."));
    return;
  }
  logLine("bug report sent by the player");
  const auto server = session_->server();
  const auto state = state_;
  // The server takes 64 KB at most; the end of the log is what tells.
  // What the player wrote comes first, then the log.
  const auto note = said.empty() ? std::string() : "message: " + said + "\n\n";
  const auto log = note + logTail(60 * 1024 - std::min<size_t>(note.size(), 8 * 1024));
  bugWorker_ = std::thread([this, server, state, log] {
    Error error;
    bool ok = false;
    guarded("bug report", [&] { ok = net::sendBugReport(server, state, log, error, &stop_); });
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
    const auto body = modal(640, 24 + 60 + 24, tr("Downloading the update"), tr("Cancel"), "", "", 4);
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
