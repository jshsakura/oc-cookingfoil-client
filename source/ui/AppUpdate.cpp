#include "ui/Application.hpp"
#include <algorithm>
#include <strings.h>
#include "core/Log.hpp"
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
    appUpdateError_ = tr("서버에 연결한 뒤 확인합니다.");
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
      }
    };
    updateDone_ = true;
  });
}
// Once the server's release is known and the console's titles are read: a
// console without this app's HOME Menu shortcut gets it (설정 can turn that
// off). Only on the console; the PC preview has no HOME Menu.
void Application::ensureOwnShortcut() {
#ifdef __SWITCH__
  if (ownShortcutChecked_ || appRelease_.forwarderTitleId.empty() || !installed_.installed.complete || installer_.running())
    return;
  ownShortcutChecked_ = true;
  const auto &titles = installed_.installed.titles;
  const bool there = std::any_of(titles.begin(), titles.end(), [this](const InstalledTitle &t) {
    return strcasecmp(t.titleId.c_str(), appRelease_.forwarderTitleId.c_str()) == 0;
  });
  if (!there)
    installShortcut("CookingFoil", "", appRelease_.forwarderUrl, appRelease_.forwarderTitleId, appRelease_.forwarderSize,
                    false);
#endif
}
void Application::installAppUpdate() {
  if (updateWorker_.joinable() || appUpdate_ != AppUpdate::Available || !session_) return;
  if (sdFree_ && *sdFree_ < appRelease_.size * 2) {
    alertTitle_ = tr("업데이트할 수 없습니다");
    alert_ = trf("SD 카드 공간이 부족합니다. {0}가 필요합니다.", {formatBytes(appRelease_.size * 2)});
    return;
  }
  appUpdate_ = AppUpdate::Downloading;
  updateReceived_ = 0;
  updateCancel_ = false;
  const auto server = session_->server();
  const auto state = state_;
  const auto release = appRelease_;
  const auto target = nroPath(selfPath_, sdRoot_);
  updateWorker_ = std::thread([this, server, state, release, target] {
    Error error;
    bool ok = false;
    guarded("update", [&] { ok = net::installClient(server, state, release, target, updateReceived_, error, &updateCancel_); });
    logLine(ok ? "update: " + release.version + " -> " + target : "update failed: " + error.message);
    const bool cancelled = updateCancel_;
    updateFinish_ = [this, ok, error, cancelled, target, release] {
      if (!ok) {
        appUpdate_ = AppUpdate::Available;
        if (cancelled) message(tr("업데이트를 취소했습니다."));
        else {
          alertTitle_ = tr("업데이트하지 못했습니다");
          alert_ = error.message;
        }
        return;
      }
      appUpdate_ = AppUpdate::Latest;
      // The loader starts the new build as this one closes; without one the
      // player starts it again.
      if (platform::relaunch(target)) {
        exitRequested_ = requestQuit();
        return;
      }
      alertTitle_ = tr("업데이트했습니다");
      alert_ = trf("v{0}을(를) 설치했습니다. 앱을 다시 실행하면 새 버전으로 열립니다.", {release.version});
    };
    updateDone_ = true;
  });
}
void Application::sendLog(const std::string &reason) {
  if (logWorker_.joinable() || !session_ || !connected_) return;
  constexpr size_t MaxLog = 60 * 1024;
  auto log = logTail(MaxLog);
  if (log.empty()) return;
  const auto server = session_->server();
  const auto state = state_;
  logWorker_ = std::thread([this, server, state, log = std::move(log), reason] {
    Error error;
    guarded("log", [&] { net::sendClientLog(server, state, log, reason, error, &stop_); });
    logFinish_ = [this, error] {
      if (error) return;
      char at[16]{};
      const auto now = std::time(nullptr);
      std::strftime(at, sizeof(at), "%H:%M", std::localtime(&now));
      logStatus_ = trf("{0}에 보냈습니다", {at});
    };
    logDone_ = true;
  });
}
void Application::pollLog() {
  // An error popup that just appeared: into the log, then to the server.
  if (!alert_.empty() && alert_ != lastAlert_) {
    lastAlert_ = alert_;
    logLine("alert: " + alertTitle_ + " | " + alert_);
    if (state_.reportErrors) sendLog("alert: " + alertTitle_ + " " + alert_);
  } else if (alert_.empty())
    lastAlert_.clear();
  if (!logDone_.exchange(false)) return;
  logWorker_.join();
  if (logFinish_) std::exchange(logFinish_, nullptr)();
}
void Application::pollAppUpdate() {
  if (!updateDone_.exchange(false)) return;
  updateWorker_.join();
  if (updateFinish_) std::exchange(updateFinish_, nullptr)();
}
std::string Application::appUpdateStatus() const {
  const std::string current = std::string("v") + COOKINGFOIL_VERSION;
  switch (appUpdate_) {
  case AppUpdate::Checking: return trf("{0} · 확인하고 있습니다", {current});
  case AppUpdate::Latest: return trf("{0} · 최신 버전입니다", {current});
  case AppUpdate::Available: return trf("새 버전 v{0}이(가) 있습니다", {appRelease_.version});
  case AppUpdate::Missing: return trf("{0} · 서버에 업데이트가 없습니다", {current});
  case AppUpdate::Failed: return current + " · " + appUpdateError_;
  case AppUpdate::Downloading:
    return trf("받는 중 {0} / {1}", {formatBytes(updateReceived_.load()), formatBytes(appRelease_.size)});
  default: return trf("{0} · A로 확인합니다", {current});
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
    alertTitle_ = tr("업데이트할 수 없습니다");
    alert_ = tr("PC 미리보기에서는 앱을 바꿀 수 없습니다. 본체의 설정 > 앱 업데이트에서 받습니다.");
    return;
  }
#endif
  if (appUpdate_ == AppUpdate::Available) appUpdateAsk_ = true;
  else if (appUpdate_ != AppUpdate::Checking) checkAppUpdate();
}
void Application::appUpdateDialog() {
  if (appUpdate_ == AppUpdate::Downloading) {
    const auto body = modal(640, 24 + 60 + 24, tr("업데이트를 받고 있습니다"), tr("취소"), "", "", 4);
    const float ratio = appRelease_.size ? std::min(1.f, float(updateReceived_.load()) / float(appRelease_.size)) : 0.f;
    paint_.fonts.text(trf("v{0} · {1} / {2}", {appRelease_.version, formatBytes(updateReceived_.load()),
                                              formatBytes(appRelease_.size)}),
                      body.x + 32, body.y + 24, 19, Ink, true, body.w - 64);
    paint_.rect(body.x + 32, body.y + 66, body.w - 64, 10, Line, 5);
    paint_.rect(body.x + 32, body.y + 66, std::max(10.f, (body.w - 64) * ratio), 10, Butter, 5);
    return;
  }
  const auto body = modal(660, 24 + 60 + 24, tr("업데이트하시겠습니까?"), tr("취소"), "", tr("업데이트|동작"), 3);
  paint_.fonts.wrapped(trf("v{0}에서 v{1}(으)로 바꿉니다. 받은 뒤 앱을 다시 시작합니다.",
                           {COOKINGFOIL_VERSION, appRelease_.version}),
                       body.x + 32, body.y + 24, 19, Secondary, body.w - 64, 2);
}
} // namespace foil::ui
