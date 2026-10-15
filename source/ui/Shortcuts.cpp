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
  // Shown even where none can be made: the row says so before A is pressed.
  return homebrewStatus(app) != homebrew::Status::Available && homebrew::cardNroPath(homebrewFile(app));
}
uint64_t Application::homebrewShortcutId(const homebrew::App &app) const {
  return forwarder::titleId(forwarder::loaderPath(homebrewFile(app)));
}
uint64_t Application::homebrewServerShortcut(const homebrew::App &app) const {
  return app.cardOnly ? 0 : forwarder::serverShortcutId(app.forwarderTitleId);
}
bool Application::homebrewHasShortcut(const homebrew::App &app) const {
  const auto server = homebrewServerShortcut(app);
  return shortcuts_.count(homebrewShortcutId(app)) || (server && shortcuts_.count(server));
}
bool Application::makeShortcutFromNro(const std::string &file, const std::string &launch, const std::string &name,
                                      const std::string &extra, const std::string &nameOverride,
                                      const std::string &authorOverride, bool justDownloaded) {
  // NCM and NS are the installer's while it runs. Every caller comes in its
  // queue turn (or with nothing downloading), so this does not happen; if
  // it did, nothing is half made.
  if (installer_.running()) {
    logLine("shortcut: the installer holds NCM; " + name + " not made");
    return true;
  }
  homebrew::NroInfo info;
  if (!homebrew::readNroFile(sdRoot_ + forwarder::loaderPath(file), homebrew::nacpLanguages(languageCode()), info)) {
    alertTitle_ = tr("Could not make the shortcut");
    alert_ = trf("Could not read the app file: {0}", {file});
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
    alertTitle_ = tr("Could not make the shortcut");
    alert_ = error.message;
    return false;
  }
  shortcuts_.insert(id);
  // Unsigned, the shortcut opens only with signature patches: said now,
  // not when the HOME Menu shows an error code.
  if (!forwarder::signaturePatchesFound(sdRoot_)) {
    alertTitle_ = tr("Shortcut made");
    alert_ = trf("Made the {0} shortcut. No signature patches were found, so it may fail to open. Install signature patches such as sys-patch first.", {name});
  } else {
    message(justDownloaded ? trf("Got {0} and added its shortcut to the HOME Menu.", {name})
                           : trf("Added the {0} shortcut to the HOME Menu.", {name}));
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
    if (!forwarder::shortcutId(id) || !shortcuts_.count(id)) continue;
    if (const auto error = platform::removeShortcut(id)) {
      alertTitle_ = tr("Could not delete the shortcut");
      alert_ = error.message;
      return;
    }
    shortcuts_.erase(id);
    removed = true;
  }
  if (removed) message(trf("Deleted the {0} shortcut from the HOME Menu.", {name}));
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
  if (!retroReady(game)) return false; // it would open to an error
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
  const auto body = modal(700, 24 + 70 + 16, tr("Delete the shortcut?"), tr("Cancel"), "", tr("Delete shortcut"), 37);
  const float x = body.x + 32, y = body.y + 24;
  paint_.fonts.text(app->name, x, y, 21, Ink, true, body.w - 64);
  paint_.fonts.text(tr("Removes only the HOME Menu shortcut and keeps the app file."), x, y + 38, 19, Secondary, false,
                    body.w - 64);
}
void Application::considerSelfShortcut() {
  // The applet mode notice with a button to fix it: every start from the
  // album while there is no shortcut, until closed for this run.
  if (selfShortcutAsk_ || selfShortcutClosed_ || !platform::canMakeShortcuts()) return;
  const auto path = forwarder::loaderPath(selfPath_);
  if (selfPath_.rfind("sdmc:/", 0) != 0) return; // not started from the SD card
  const auto id = forwarder::titleId(path);
  const bool present = platform::presentShortcuts({id}).count(id) > 0;
  if (present) shortcuts_.insert(id);
  if (!forwarder::offerSelfShortcut(platform::applicationMode(), selfShortcutClosed_, selfPath_, present)) return;
  selfShortcutAsk_ = true;
  // It takes the place of the plain applet mode notice.
  alert_.clear();
  alertTitle_.clear();
}
void Application::selfShortcutAction(Action a) {
  if (a != Action::Back && !(a == Action::Accept && settled())) return;
  selfShortcutAsk_ = false;
  selfShortcutClosed_ = true; // not again this run; the next start from the album asks again
  if (a != Action::Accept) return;
  // A download (a resumed one) holds NCM: it is made in its turn.
  if (downloadRunning() || !installQueue_.empty()) {
    queueJob("shortcut", trf("{0} shortcut", {"CookingFoil"}), "", "", 0, "self");
    return;
  }
  makeShortcutFromNro(forwarder::loaderPath(selfPath_).substr(1), "", "CookingFoil", "", "", "", false);
}
// Possible here (made from the app's file) or from the server (its
// forwarder, installed like a game).
bool Application::homebrewShortcutPossible(const homebrew::App &app) const {
  return (platform::canMakeShortcuts() && homebrew::cardNroPath(homebrewFile(app))) ||
         (!app.cardOnly && !app.forwarderUrl.empty() && session_);
}
// 바로가기 on the app's card: made now when nothing downloads, else queued
// to be made in its turn; the server's forwarder when none can be made here.
void Application::requestHomebrewShortcut(const homebrew::App &app) {
  if (platform::canMakeShortcuts() && homebrew::cardNroPath(homebrewFile(app))) {
    if (downloadRunning() || !installQueue_.empty())
      queueJob("shortcut", trf("{0} shortcut", {app.name}), homebrewIcon(app), "", 0, homebrew::serialize(app));
    else
      makeHomebrewShortcut(app, false);
    return;
  }
  if (!app.cardOnly && !app.forwarderUrl.empty() && session_) {
    Item shortcut;
    shortcut.url = app.forwarderUrl;
    shortcut.name = app.name;
    shortcut.titleId = shortcut.baseId = app.forwarderTitleId;
    shortcut.size = app.forwarderSize;
    shortcut.icon = app.iconUrl;
    const bool waits = downloadRunning() || !installQueue_.empty();
    if (auto error = startInstall({shortcut}, trf("{0} shortcut", {app.name}), app.iconUrl, "sd")) {
      alertTitle_ = tr("Could not make the shortcut");
      alert_ = error.message;
    } else if (!waits)
      message(trf("Installing the {0} HOME Menu shortcut.", {app.name}));
    return;
  }
  alertTitle_ = tr("A shortcut can't be made here");
  const auto reason = platform::shortcutBlocker();
  alert_ = reason.empty() ? std::string(tr("The server has no shortcut for this app.")) : reason;
}
void Application::selfShortcutDialog() {
  const bool patches = forwarder::signaturePatchesFound(sdRoot_);
  constexpr int Lines = 3;
  const float textW = 760 - 64;
  const auto why = tr("Opened from the Album there is little memory, so big installs can be slow or fail. With a HOME Menu shortcut it opens straight from the HOME Menu next time.");
  const float described = paint_.fonts.wrapped(why, -9999, -9999, 19, Ink, textW, Lines);
  const float bodyH = 24 + described + (patches ? 0 : 16 + 48) + 24;
  const auto body = modal(760, bodyH, tr("Running in applet mode"), tr("Close"), "", tr("Make shortcut"), HomeIcon);
  const float x = body.x + 32;
  float y = body.y + 24;
  y += paint_.fonts.wrapped(why, x, y, 19, Ink, textW, Lines) + 16;
  if (!patches)
    paint_.fonts.wrapped(tr("No signature patches were found. A shortcut opens only with signature patches installed."), x, y, 17,
                         Pending, textW, 2);
}
} // namespace foil::ui
