// HOME Menu shortcuts made on the console: this app's own (offered once
// when it is opened from the album), each homebrew app's from its popup or
// after a download, and a retro game's when the server has none.
#include "ui/Application.hpp"
#include "core/Forwarder.hpp"
#include "core/Log.hpp"
#include "platform/Shortcut.hpp"
#include "platform/Storage.hpp"
namespace foil::ui {
namespace {
// The app's own icon on the self-shortcut offer.
constexpr int HomeIcon = 0;
} // namespace
bool Application::homebrewShortcutRow(const homebrew::App &app) const {
  return homebrewStatus(app) != homebrew::Status::Available && homebrew::cardNroPath(homebrewFile(app)) &&
         platform::canMakeShortcuts();
}
uint64_t Application::homebrewShortcutId(const homebrew::App &app) const {
  return forwarder::titleId(forwarder::loaderPath(homebrewFile(app)));
}
uint64_t Application::homebrewServerShortcut(const homebrew::App &app) const {
  return app.cardOnly ? 0 : forwarder::parseHexId(app.forwarderTitleId);
}
bool Application::homebrewHasShortcut(const homebrew::App &app) const {
  const auto server = homebrewServerShortcut(app);
  return shortcuts_.count(homebrewShortcutId(app)) || (server && shortcuts_.count(server));
}
bool Application::makeShortcutFromNro(const std::string &file, const std::string &launch, const std::string &name,
                                      const std::string &extra, const std::string &nameOverride,
                                      const std::string &authorOverride, bool justDownloaded) {
  // NCM and NS are the installer's while it runs.
  if (installer_.running()) {
    // A retro game (another .nro started) is got again; an app has its card.
    message(!justDownloaded ? std::string(tr("지금 설치가 끝난 뒤 다시 시도합니다."))
            : launch.empty() ? trf("{0}을(를) 받았습니다. 지금 설치가 끝난 뒤 홈브류의 앱 카드에서 바로가기를 만들 수 있습니다.", {name})
                             : trf("{0}을(를) 받았습니다. 지금 설치가 끝난 뒤 다시 받으면 홈 메뉴 바로가기를 만듭니다.", {name}));
    return true;
  }
  homebrew::NroInfo info;
  if (!homebrew::readNroFile(sdRoot_ + forwarder::loaderPath(file), homebrew::nacpLanguages(languageCode()), info)) {
    alertTitle_ = tr("바로가기를 만들지 못했습니다");
    alert_ = trf("앱 파일을 읽지 못했습니다: {0}", {file});
    return false;
  }
  platform::ShortcutRequest request;
  request.nroPath = forwarder::loaderPath(launch.empty() ? file : launch);
  request.extraArgs = extra;
  request.name = nameOverride;
  request.author = authorOverride;
  request.nacp = info.nacp.empty() ? forwarder::blankNacp(info.name.empty() ? name : info.name, info.author, info.version)
                                   : info.nacp;
  request.icon = info.icon;
  uint64_t id = 0;
  if (const auto error = platform::makeShortcut(request, id)) {
    alertTitle_ = tr("바로가기를 만들지 못했습니다");
    alert_ = error.message;
    return false;
  }
  shortcuts_.insert(id);
  // Unsigned, the shortcut opens only with signature patches: said now,
  // not when the HOME Menu shows an error code.
  if (!forwarder::signaturePatchesFound(sdRoot_)) {
    alertTitle_ = tr("바로가기를 만들었습니다");
    alert_ = trf("{0} 바로가기를 만들었습니다. 서명 패치가 보이지 않아 실행하면 오류가 날 수 있으니 sys-patch 같은 서명 패치를 먼저 설치합니다.", {name});
  } else {
    message(justDownloaded ? trf("{0}을(를) 받고 홈 메뉴에 바로가기를 만들었습니다.", {name})
                           : trf("홈 메뉴에 {0} 바로가기를 만들었습니다.", {name}));
  }
  return true;
}
// Made here, it points at the file actually on the card and needs no
// download, so it wins over the server's; the server's, when installed, goes
// so the HOME Menu does not show the app twice.
bool Application::makeHomebrewShortcut(const homebrew::App &app, bool justDownloaded) {
  const auto file = homebrewFile(app);
  if (!homebrew::cardNroPath(file)) return false;
  if (!makeShortcutFromNro(file, "", app.name, "", "", "", justDownloaded)) return true;
  const auto server = homebrewServerShortcut(app);
  if (server && server != homebrewShortcutId(app) && shortcuts_.count(server)) {
    if (const auto error = platform::removeShortcut(server)) logLine("shortcut: server one stays: " + error.message);
    else shortcuts_.erase(server);
  }
  return true;
}
void Application::removeHomebrewShortcut(const homebrew::App &app) {
  removeShortcuts({homebrewShortcutId(app), homebrewServerShortcut(app)}, app.name);
}
void Application::removeShortcuts(const std::vector<uint64_t> &ids, const std::string &name) {
  bool removed = false;
  for (const auto id : ids) {
    if (!id || !shortcuts_.count(id)) continue;
    if (const auto error = platform::removeShortcut(id)) {
      alertTitle_ = tr("바로가기를 삭제하지 못했습니다");
      alert_ = error.message;
      return;
    }
    shortcuts_.erase(id);
    removed = true;
  }
  if (removed) message(trf("홈 메뉴에서 {0} 바로가기를 삭제했습니다.", {name}));
}
bool Application::autoHomebrewShortcut(const homebrew::App &app) {
  if (!state_.makeShortcuts) return false;
  if (platform::canMakeShortcuts() && makeHomebrewShortcut(app, true)) return true;
  return installShortcut(app.name, app.iconUrl, app.forwarderUrl, app.forwarderTitleId, app.forwarderSize);
}
// The server's shortcut first: it carries the game's own icon. Without one,
// a shortcut made here starts the game's core with the ROM, under RetroArch's
// icon and the game's name.
bool Application::installRetroShortcut(const retro::Game &game) {
  if (!game.forwarderUrl.empty())
    return installShortcut(game.name, game.iconUrl, game.forwarderUrl, game.forwarderTitleId, game.forwarderSize);
  const auto core = retro::corePath(game.core);
  if (!state_.makeShortcuts || core.empty() || game.romPath.empty() || !platform::canMakeShortcuts() ||
      !retroArchPresent())
    return false;
  const auto rom = "sdmc:/" + game.romPath;
  const auto extra = rom.find(' ') == std::string::npos ? rom : "\"" + rom + "\"";
  // The id comes from the core's path and the ROM, so each game has its own.
  const auto made = makeShortcutFromNro(retro::RetroArchApp, core, game.name, extra, game.name, "RetroArch", true);
  if (made) logLine("retro shortcut " + game.name + " -> " + core);
  return true;
}
void Application::shortcutRemoveDialog() {
  const auto *app = homebrewPicked();
  if (!app) {
    shortcutRemoveAsk_ = false;
    return;
  }
  const auto body = modal(700, 24 + 70 + 16, tr("바로가기를 삭제하시겠습니까?"), tr("취소"), "", tr("바로가기 삭제"), 37);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(app->name, x, y, 21, Ink, true, body.w - 64);
  paint_.fonts.text(tr("홈 메뉴에서 바로가기만 삭제하고 앱 파일은 그대로 둡니다."), x, y + 38, 19, Secondary, false,
                    body.w - 64);
}
void Application::considerSelfShortcut() {
  if (selfShortcutAsk_ || state_.selfShortcutAsked || !platform::canMakeShortcuts()) return;
  const auto path = forwarder::loaderPath(selfPath_);
  if (selfPath_.rfind("sdmc:/", 0) != 0) return; // not started from the SD card
  const auto id = forwarder::titleId(path);
  const bool present = platform::presentShortcuts({id}).count(id) > 0;
  if (present) shortcuts_.insert(id);
  if (!forwarder::offerSelfShortcut(platform::applicationMode(), state_.selfShortcutAsked, selfPath_, present)) return;
  selfShortcutAsk_ = true;
  // The offer says what the applet mode notice says, and how to fix it.
  alert_.clear();
  alertTitle_.clear();
}
void Application::selfShortcutAction(Action a) {
  if (a != Action::Back && !(a == Action::Accept && settled())) return;
  selfShortcutAsk_ = false;
  state_.selfShortcutAsked = true; // asked once, whatever the answer
  saveStateQuietly();
  if (a == Action::Accept) makeShortcutFromNro(forwarder::loaderPath(selfPath_).substr(1), "", "CookingFoil", "", "", "", false);
}
void Application::selfShortcutDialog() {
  const bool patches = forwarder::signaturePatchesFound(sdRoot_);
  constexpr int Lines = 3;
  const float textW = 760 - 64;
  const auto why = tr("앨범에서 열면 메모리가 적어 큰 게임 설치가 느리거나 실패할 수 있습니다. 홈 메뉴에 바로가기를 만들면 다음부터 홈 메뉴에서 바로 열립니다.");
  const float described = paint_.fonts.wrapped(why, -9999, -9999, 19, Ink, textW, Lines);
  const float bodyH = 24 + described + 16 + 26 + (patches ? 0 : 56) + 16;
  const auto body = modal(760, bodyH, tr("홈 메뉴에 바로가기를 만들까요?"), tr("닫기"), "", tr("만들기"), HomeIcon);
  const float x = body.x + 32;
  float y = body.y + 24;
  y += paint_.fonts.wrapped(why, x, y, 19, Ink, textW, Lines) + 16;
  paint_.fonts.text(tr("한 번만 묻습니다. 닫으면 다시 묻지 않습니다."), x, y, 17, Secondary, false, textW);
  if (!patches)
    paint_.fonts.wrapped(tr("서명 패치가 보이지 않습니다. 바로가기는 서명 패치가 있어야 실행됩니다."), x, y + 34, 17,
                         Pending, textW, 2);
}
} // namespace foil::ui
