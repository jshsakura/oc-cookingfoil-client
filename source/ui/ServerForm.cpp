#include "ui/Application.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "net/Api.hpp"
#include <algorithm>
#include <cctype>
#include <filesystem>
namespace foil::ui {
namespace {
std::string trimmed(const std::string &text) {
  const auto first = text.find_first_not_of(" \t");
  return first == std::string::npos ? "" : text.substr(first, text.find_last_not_of(" \t") - first + 1);
}
bool startsWith(const std::string &text, const std::string &prefix) {
  if (text.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(text[i])) != prefix[i]) return false;
  return true;
}
bool sameServer(const Server &a, const Server &b) {
  return a.title == b.title && a.url == b.url && a.username == b.username && a.password == b.password &&
         a.cfClientId == b.cfClientId && a.cfClientSecret == b.cfClientSecret && a.enabled == b.enabled &&
         a.color == b.color;
}
// The form's rows: the server's four fields, then the approval message.
std::string &fieldOf(Server &server, std::string &note, int field) {
  switch (field) {
  case 0: return server.url;
  case 1: return server.username;
  case 2: return server.password;
  case 3: return server.title;
  default: return note;
  }
}
} // namespace
void Application::openServerForm(int index) {
  ServerForm form;
  // A new server starts empty; nothing is filled in for the player.
  if (index >= 0 && size_t(index) < config_.servers.size()) {
    form.index = index;
    form.server = config_.servers[index];
    if (const auto found = state_.pairMessages.find(form.server.url); found != state_.pairMessages.end())
      form.note = found->second;
  }
  serverForm_ = form;
  textField_ = -1;
}
void Application::askServerField(int field) {
  static const int lengths[ServerFields] = {512, 128, 128, 64, int(net::PairMessageLimit)};
  TextRequest request;
  request.guide = field == 0 ? tr("Enter the server address") : field == 1 ? tr("Enter the username")
                : field == 2 ? tr("Enter the password") : field == 3 ? tr("Enter the name shown in the list")
                             : tr("Enter a message for the admin");
  request.initial = fieldOf(serverForm_->server, serverForm_->note, field);
  request.password = field == 2;
  request.maxLength = lengths[field];
  textRequest_ = request;
  textField_ = field;
  raField_ = -1; // the keyboard is this field's now
  pairMessageField_ = false;
}
void Application::enterText(const std::string &text) {
  if (const int field = std::exchange(raField_, -1); field >= 0) {
    (field == 0 ? raUser_ : raPassword_) = text;
    raError_ = {};
    return;
  }
  if (std::exchange(bugMessageField_, false)) {
    bugMessage_ = text;
    return;
  }
  if (std::exchange(pairMessageField_, false)) {
    setPairMessage(text);
    return;
  }
  if (!serverForm_ || textField_ < 0 || textField_ >= ServerFields) return;
  fieldOf(serverForm_->server, serverForm_->note, textField_) = text;
  serverForm_->error.clear();
}
void Application::serverFormAction(Action a) {
  auto &form = *serverForm_;
  if (form.confirmDelete) {
    if (a == Action::Back) form.confirmDelete = false;
    else if (a == Action::Accept && settled()) deleteServerForm();
    return;
  }
  const bool existing = form.index >= 0;
  switch (a) {
  case Action::Up: form.row = std::max(0, form.row - 1); break;
  case Action::Down: form.row = std::min(ServerFields, form.row + 1); break;
  // The last row is the dot's colour: left and right, or A, step through them.
  case Action::Left:
  case Action::Right:
    if (form.row == ServerFields) stepServerColor(a == Action::Left ? -1 : 1);
    break;
  case Action::Accept:
    if (form.row == ServerFields) stepServerColor(1);
    else askServerField(form.row);
    break;
  // X deletes a server already there.
  case Action::Search: if (existing) form.confirmDelete = true; break;
  // Y saves from any row; in 설정 the console sends it as Reload.
  case Action::Queue:
  case Action::Reload: saveServerForm(); break;
  case Action::Back: serverForm_.reset(); break;
  default: break;
  }
}
// The colour shown is the one chosen, else the one its place gives; stepping
// from that one keeps the first step next to what the player sees.
static_assert(std::size(ServerColors) == ServerColorCount, "one colour per choice");
int Application::serverFormColor() const {
  const auto &form = *serverForm_;
  if (form.server.color >= 0) return form.server.color;
  const size_t place = form.index >= 0 ? size_t(form.index) : config_.servers.size();
  return int(place % ServerColorCount);
}
void Application::stepServerColor(int step) {
  serverForm_->server.color = (serverFormColor() + step + ServerColorCount) % ServerColorCount;
}
void Application::saveServerForm() {
  auto &form = *serverForm_;
  auto server = form.server;
  const auto fail = [&](const std::string &message, int row) {
    form.error = message;
    form.row = row;
  };
  const auto url = trimmed(server.url);
  if (url.empty()) return fail(tr("Enter the address."), 0);
  // A bare address like example.com gets https://, as most servers use it.
  const bool scheme = startsWith(url, "http://") || startsWith(url, "https://");
  if (!scheme && url.find("://") != std::string::npos)
    return fail(tr("The address must start with http:// or https://."), 0);
  server.url = normalizeUrl(scheme ? url : "https://" + url); // trailing slashes go here
  if (server.url.empty()) return fail(tr("Check the address."), 0);
  for (int field = 1; field < 4; ++field)
    if (fieldOf(server, form.note, field).find_first_of("\r\n") != std::string::npos)
      return fail(tr("Enter it without line breaks."), field);
  // The approval message goes to state.json under the address; the server
  // reads it with the next approval request.
  const auto note = net::cleanPairMessage(form.note);
  const bool noteChanged = note != (state_.pairMessages.count(server.url) ? state_.pairMessages[server.url] : "");
  if (noteChanged) {
    if (note.empty()) state_.pairMessages.erase(server.url);
    else state_.pairMessages[server.url] = note;
    saveStateQuietly();
  }
  server.title = trimmed(server.title);
  if (server.title.empty()) server.title = hostOf(server.url);
  auto next = config_;
  int index = form.index;
  std::string from; // its address before, when it is one already set up
  if (index < 0) {
    if (next.servers.size() >= 32) return fail(tr("You can add up to 32 servers."), form.row);
    next.servers.push_back(server);
    index = int(next.servers.size()) - 1;
  } else if (size_t(index) < next.servers.size()) {
    // Nothing changed: 저장 just connects to it, as A on the row used to.
    if (sameServer(next.servers[index], server)) {
      serverForm_.reset();
      // A new message for a server already open is asked with again.
      if (!(size_t(index) == serverIndex_ && session_)) connect(index);
      else if (noteChanged && !note.empty()) {
        message(tr("Asking for approval again with your message."));
        retryPairing();
      }
      return;
    }
    from = next.servers[index].url;
    next.servers[index] = server;
  } else {
    serverForm_.reset();
    return;
  }
  if (writeServers(next, index, tr("Saved the server."))) {
    serverForm_.reset();
    selected_ = index;
    // Its downloads, waiting or done, go on naming it at its new address.
    if (from != server.url && history::moveServer(history_, from, server.url)) {
      saveHistory();
      rebuildQueue();
    }
  }
}
void Application::deleteServerForm() {
  const int index = serverForm_->index;
  if (index < 0 || size_t(index) >= config_.servers.size()) {
    serverForm_.reset();
    return;
  }
  auto next = config_;
  next.servers.erase(next.servers.begin() + index);
  // The server in use stays in use when another one goes.
  int prefer = -1;
  if (session_ && serverIndex_ != size_t(index)) prefer = int(serverIndex_) - (serverIndex_ > size_t(index) ? 1 : 0);
  if (writeServers(next, prefer, tr("Deleted the server."))) {
    serverForm_.reset();
    selected_ = std::min(index, int(next.servers.size()));
  } else if (serverForm_) {
    serverForm_->confirmDelete = false;
  }
}
bool Application::writeServers(const Config &next, int prefer, const std::string &done) {
  // The PC preview without a config file keeps the change in memory.
  if (configPath_.empty()) {
    config_ = next;
    message(done);
    return true;
  }
  // A config.json edited by hand that no longer reads is left alone.
  std::error_code ec;
  if (std::filesystem::exists(configPath_, ec) && parseConfig(readFile(configPath_)).error) {
    alertTitle_ = tr("Could not save the server");
    alert_ = tr("config.json has an error, so it was not overwritten. Fix the file, then save again.");
    return false;
  }
  std::string error;
  if (!saveConfig(configPath_, next, error)) {
    logLine("config write failed: " + error);
    alertTitle_ = tr("Could not save the server");
    alert_ = trf("Could not write config.json. {0}", {error});
    return false;
  }
  // A connection still running finishes first; - reads the new file then.
  if (network_.joinable() && !done_) {
    message(tr("Saved the server. Press - to refresh once the connection finishes."));
    return true;
  }
  // What was just written and the records in memory are what a read of the
  // card would give: applied now, with no worker to wait for.
  std::vector<std::string> urls;
  for (const auto &s : next.servers) urls.push_back(s.url);
  setKnownServers(std::move(urls));
  ReloadRead read;
  read.config.value = next;
  read.state.value = state_;
  reloadDrop_ = reloadWorker_.joinable(); // a read still going is older than this file
  applyReload(prefer, std::move(read));
  message(done);
  return true;
}
void Application::serverFormDialog() {
  auto &form = *serverForm_;
  const bool existing = form.index >= 0;
  constexpr float Width = 760, Row = 56, Gap = 8;
  const float errorSpace = form.error.empty() ? 0 : 30;
  // An http address off the home network: said, not refused (a server may
  // have no https), so the owner can choose.
  const bool exposed = plainHttpOverInternet(normalizeUrl(trimmed(form.server.url)));
  const float exposedSpace = exposed ? 52 : 0;
  constexpr int Rows = ServerFields + 1; // the fields, then the colour
  const float bodyH = 20 + Rows * (Row + Gap) - Gap + 18 + 50 + errorSpace + exposedSpace + 18;
  const auto body = modal(Width, bodyH, existing ? tr("Edit server") : tr("Add server"), tr("Cancel"), tr("Save"),
                          form.row == ServerFields ? tr("Change|admin") : tr("Type"),
                          21, existing ? tr("Delete") : "");
  const float x = body.x + 32, w = body.w - 64, valueX = x + 170, valueW = w - 190;
  const char *labels[ServerFields] = {tr("Address"), tr("Username"), tr("Password"), tr("Name"), tr("Message")};
  for (int field = 0; field < ServerFields; ++field) {
    const float y = body.y + 20 + field * (Row + Gap), mid = y + Row / 2;
    rowHit(x, y, w, Row, 12, [this, field] {
      serverForm_->row = field;
      action(Action::Accept);
    });
    if (form.row == field) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.fonts.middle(labels[field], x + 20, mid, 18, Secondary, true, 140);
    const auto &value = fieldOf(form.server, form.note, field);
    if (value.empty()) {
      // A placeholder per field; 이름 shows the host it falls back to.
      const char *placeholders[ServerFields] = {tr("Server address (e.g. example.com)"), tr("Login username"),
                                                tr("Login password"), tr("Server name"),
                                                tr("Message for the admin (optional)")};
      const auto host = field == 3 ? hostOf(normalizeUrl(trimmed(form.server.url))) : "";
      const std::string hint = host.empty() ? placeholders[field] : host;
      paint_.fonts.middle(hint, valueX, mid, 19, Secondary, false, valueW);
    } else if (field == 2) {
      // The password shows as dots, one per character up to a row's worth.
      const size_t dots = std::min<size_t>(utf8Characters(value).size(), 24);
      for (size_t n = 0; n < dots; ++n) paint_.rect(valueX + n * 18, mid - 5, 10, 10, Ink, 5);
    } else {
      paint_.fonts.middle(value, valueX, mid, 20, Ink, field == 3, valueW);
    }
  }
  {
    // The dot this server wears where several servers are listed together.
    const float y = body.y + 20 + ServerFields * (Row + Gap), mid = y + Row / 2;
    if (form.row == ServerFields) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.fonts.middle(tr("Color"), x + 20, mid, 18, Secondary, true, 140);
    constexpr float Dot = 22, Step = 40;
    const int chosen = serverFormColor();
    for (int c = 0; c < ServerColorCount; ++c) {
      const float cx = valueX + c * Step + Dot / 2;
      if (c == chosen) paint_.rect(cx - Dot / 2 - 5, mid - Dot / 2 - 5, Dot + 10, Dot + 10, Ink, int(Dot / 2 + 5));
      if (c == chosen) paint_.rect(cx - Dot / 2 - 2, mid - Dot / 2 - 2, Dot + 4, Dot + 4, Surface, int(Dot / 2 + 2));
      paint_.rect(cx - Dot / 2, mid - Dot / 2, Dot, Dot, ServerColors[c], int(Dot / 2));
      hit(cx - Step / 2, y, Step, Row, [this, c] {
        serverForm_->row = ServerFields;
        serverForm_->server.color = c;
      });
    }
  }
  float lineY = body.y + 20 + Rows * (Row + Gap) - Gap + 18;
  if (!form.error.empty()) {
    paint_.fonts.text(form.error, x, lineY, 18, Bad, true, w);
    lineY += errorSpace;
  }
  if (exposed) {
    paint_.fonts.wrapped(tr("This address is not encrypted (http), so the password and keys cross the internet as they are. Use the https address if the server has one."),
                         x, lineY, 17, Pending, w, 2);
    lineY += exposedSpace;
  }
  paint_.fonts.wrapped(tr("For a server that uses device approval, enter only the address and save to go to the QR approval screen; the message goes to the admin with the request."),
                       x, lineY, 17, Secondary, w, 2);
  if (!form.confirmDelete) return;
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 120});
  blockTouch();
  const auto ask = modal(640, 24 + 70 + 16, tr("Delete this server?"), tr("Cancel"), "", tr("Delete"), 37);
  paint_.fonts.text(form.server.title, ask.x + 32, ask.y + 24, 21, Ink, true, ask.w - 64);
  paint_.fonts.text(tr("Other servers stay as they are."), ask.x + 32, ask.y + 62, 19, Secondary, false, ask.w - 64);
}
} // namespace foil::ui
