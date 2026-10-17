// HOME Menu shortcuts made on the console: this app's own (offered once
// when it is opened from the album), each homebrew app's from its popup or
// after a download, and a retro game's when the server has none.
#include "ui/Application.hpp"
#include "core/Forwarder.hpp"
#include "core/Log.hpp"
#include "platform/Shortcut.hpp"
#include "platform/Storage.hpp"
#include "core/Json.hpp"
#include <cstdlib>
#include "net/Api.hpp"
#include "ui/ShortcutIcon.hpp"
namespace foil::ui {
namespace {
// The app's own icon on the self-shortcut offer.
constexpr int HomeIcon = 0;
// What a queued shortcut is made of, decided when it is asked for and kept
// in its row ("plan:" and this as JSON).
struct Plan {
  std::string file, launch, extra, name, author, display, iconUrl;
  uint64_t replaces = 0; // the server's shortcut for it, which goes
};
std::string serializePlan(const Plan &plan) {
  Json body(json_object_new_object());
  const auto put = [&](const char *key, const std::string &value) {
    json_object_object_add(body.get(), key, json_object_new_string(value.c_str()));
  };
  put("file", plan.file), put("launch", plan.launch), put("extra", plan.extra), put("name", plan.name);
  put("author", plan.author), put("display", plan.display), put("iconUrl", plan.iconUrl);
  put("replaces", std::to_string(plan.replaces));
  return "plan:" + std::string(json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN));
}
bool parsePlan(const std::string &payload, Plan &plan) {
  if (payload.rfind("plan:", 0) != 0) return false;
  Error error;
  const auto body = parseJson(payload.substr(5), error);
  if (!body) return false;
  plan.file = stringField(body.get(), "file"), plan.launch = stringField(body.get(), "launch");
  plan.extra = stringField(body.get(), "extra"), plan.name = stringField(body.get(), "name");
  plan.author = stringField(body.get(), "author"), plan.display = stringField(body.get(), "display");
  plan.iconUrl = stringField(body.get(), "iconUrl");
  plan.replaces = std::strtoull(stringField(body.get(), "replaces", "0").c_str(), nullptr, 10);
  return !plan.file.empty();
}
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
  if (platform::canMakeShortcuts() && homebrew::cardNroPath(homebrewFile(app))) {
    queueShortcut(app.name, homebrewIcon(app),
                  serializePlan({homebrewFile(app), "", "", app.name, "", app.name, app.iconUrl, homebrewServerShortcut(app)}));
    return true;
  }
  return installShortcut(app.name, app.iconUrl, app.forwarderUrl, app.forwarderTitleId, app.forwarderSize);
}
// Made here: the game's core with the ROM, under the game's name and the
// server's picture of it. The server's own only where none can be made here.
bool Application::installRetroShortcut(const retro::Game &game) {
  if (!retroReady(game)) return false; // it would open to an error
  if (state_.makeShortcuts && !retro::corePath(game.core).empty() && !game.romPath.empty() &&
      platform::canMakeShortcuts() && retroArchPresent()) {
    const auto rom = "sdmc:/" + game.romPath;
    // The id comes from the core's path and the ROM, so each game has its own.
    queueShortcut(game.name, game.iconUrl,
                  serializePlan({retro::RetroArchApp, retro::corePath(game.core),
                                 rom.find(' ') == std::string::npos ? rom : "\"" + rom + "\"", game.name, "RetroArch",
                                 game.name, game.iconUrl, forwarder::serverShortcutId(game.forwarderTitleId)}));
    return true;
  }
  if (!game.forwarderUrl.empty())
    return installShortcut(game.name, game.iconUrl, game.forwarderUrl, game.forwarderTitleId, game.forwarderSize);
  return false;
}
void Application::queueShortcut(const std::string &name, const std::string &icon, const std::string &payload) {
  queueJob("shortcut", trf("{0} shortcut", {name}), icon, "", 0, payload);
}
// Its turn: the picture fetched and the shortcut written on the writer core,
// so the screen keeps moving; said and its row ended back here.
bool Application::startShortcut(const QueuedInstall &next) {
  Plan plan;
  if (!parsePlan(next.payload, plan)) {
    // A row from before plans: a homebrew app's JSON.
    const auto apps = homebrew::parse("{\"homebrew\":[" + next.payload + "]}", "");
    if (apps.empty() || !homebrew::cardNroPath(homebrewFile(apps.front()))) return false;
    const auto &app = apps.front();
    plan = {homebrewFile(app), "", "", app.name, "", app.name, app.iconUrl, homebrewServerShortcut(app)};
  }
  const auto server = session_ ? session_->server() : Server{};
  retroBytes_ = 0; // nothing to download: no percent on its row
  return runRetroJob("shortcut", [this, plan, server, state = state_, sd = sdRoot_,
                                  languages = homebrew::nacpLanguages(languageCode())]() -> std::function<void()> {
    Error error;
    uint64_t id = 0;
    homebrew::NroInfo info;
    if (!homebrew::readNroFile(sd + forwarder::loaderPath(plan.file), languages, info))
      error.message = trf("Could not read the app file: {0}", {plan.file});
    else {
      // The server's picture, as the web shows it now (its address carries
      // the version); the .nro's own when it cannot be had.
      std::string icon;
      if (plan.iconUrl.rfind("http", 0) == 0) {
        auto headers = net::authHeaders(server, state, plan.iconUrl);
        headers.push_back("Accept: image/jpeg,image/png"); // no WebP (see shortcutIconJpeg)
        const auto got = net::get(plan.iconUrl, headers);
        if (got.status == 200 && got.error.empty()) icon = shortcutIconJpeg(got.body);
        if (!icon.empty())
          logLine("shortcut: " + plan.display + " icon from the server's picture (" +
                  (uint8_t(got.body[0]) == 0xFF ? "JPEG " : "PNG ") + std::to_string(got.body.size() >> 10) + "KB -> " +
                  std::to_string(icon.size() >> 10) + "KB)");
        else logLine("shortcut: " + plan.display + " keeps the app's own icon (HTTP " + std::to_string(got.status) + ")");
      }
      platform::ShortcutRequest request;
      request.nroPath = forwarder::loaderPath(plan.launch.empty() ? plan.file : plan.launch);
      request.extraArgs = plan.extra;
      request.name = plan.name;
      request.author = plan.author;
      request.nacp = info.nacp.empty() ? forwarder::blankNacp(info.name.empty() ? plan.display : info.name, info.author, info.version)
                                       : info.nacp;
      request.icon = icon.empty() ? info.icon : icon;
      error = platform::makeShortcut(request, id);
    }
    const bool patches = !error && forwarder::signaturePatchesFound(sd);
    return [this, plan, error, id, patches] {
      finishRetroEntry("shortcut", error ? history::Outcome::Failed : history::Outcome::Done, error.message, 0, 0);
      if (error) {
        alertTitle_ = tr("Could not make the shortcut");
        alert_ = error.message;
        return;
      }
      shortcuts_.insert(id);
      // The server's for the same app goes, so the HOME Menu shows it once.
      if (plan.replaces && plan.replaces != id && shortcuts_.count(plan.replaces)) {
        if (const auto removed = platform::removeShortcut(plan.replaces)) logLine("shortcut: server one stays: " + removed.message);
        else shortcuts_.erase(plan.replaces);
      }
      // Unsigned, the shortcut opens only with signature patches: said now,
      // not when the HOME Menu shows an error code.
      if (!patches) {
        alertTitle_ = tr("Shortcut made");
        alert_ = trf("Made the {0} shortcut. No signature patches were found, so it may fail to open. Install signature patches such as sys-patch first.", {plan.display});
      } else
        message(trf("Added the {0} shortcut to the HOME Menu.", {plan.display}));
    };
  });
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
    queueShortcut(app.name, homebrewIcon(app),
                  serializePlan({homebrewFile(app), "", "", app.name, "", app.name, app.iconUrl, homebrewServerShortcut(app)}));
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
