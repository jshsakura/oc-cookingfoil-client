#include "ui/Application.hpp"
#include "core/Config.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
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
// What parseConfig names a server without a title: its host and port.
std::string hostOf(const std::string &url) {
  const auto origin = originOf(url);
  const auto scheme = origin.find("://");
  return scheme == std::string::npos ? "" : origin.substr(scheme + 3);
}
bool sameServer(const Server &a, const Server &b) {
  return a.title == b.title && a.url == b.url && a.username == b.username && a.password == b.password &&
         a.cfClientId == b.cfClientId && a.cfClientSecret == b.cfClientSecret && a.enabled == b.enabled;
}
std::string &fieldOf(Server &server, int field) {
  switch (field) {
  case 0: return server.url;
  case 1: return server.username;
  case 2: return server.password;
  default: return server.title;
  }
}
} // namespace
void Application::openServerForm(int index) {
  ServerForm form;
  // A new server starts empty; nothing is filled in for the player.
  if (index >= 0 && size_t(index) < config_.servers.size()) {
    form.index = index;
    form.server = config_.servers[index];
    form.row = ServerSaveRow;
  }
  serverForm_ = form;
  textField_ = -1;
}
void Application::askServerField(int field) {
  static const int lengths[ServerFields] = {512, 128, 128, 64};
  TextRequest request;
  request.guide = field == 0 ? tr("서버 주소를 입력합니다") : field == 1 ? tr("아이디를 입력합니다")
                : field == 2 ? tr("비밀번호를 입력합니다") : tr("목록에 보일 이름을 입력합니다");
  request.initial = fieldOf(serverForm_->server, field);
  request.password = field == 2;
  request.maxLength = lengths[field];
  textRequest_ = request;
  textField_ = field;
}
void Application::enterText(const std::string &text) {
  if (!serverForm_ || textField_ < 0 || textField_ >= ServerFields) return;
  fieldOf(serverForm_->server, textField_) = text;
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
  case Action::Up: form.row = form.row >= ServerSaveRow ? ServerFields - 1 : std::max(0, form.row - 1); break;
  case Action::Down: if (form.row < ServerSaveRow) ++form.row; break;
  case Action::Left: if (form.row == ServerDeleteRow) form.row = ServerSaveRow; break;
  case Action::Right: if (form.row == ServerSaveRow && existing) form.row = ServerDeleteRow; break;
  case Action::Accept:
    if (form.row < ServerFields) askServerField(form.row);
    else if (form.row == ServerSaveRow) saveServerForm();
    else form.confirmDelete = true;
    break;
  // Y saves from any row; in 설정 the console sends it as Reload.
  case Action::Queue:
  case Action::Reload: saveServerForm(); break;
  case Action::Back: serverForm_.reset(); break;
  default: break;
  }
}
void Application::saveServerForm() {
  auto &form = *serverForm_;
  auto server = form.server;
  const auto fail = [&](const std::string &message, int row) {
    form.error = message;
    form.row = row;
  };
  const auto url = trimmed(server.url);
  if (url.empty()) return fail(tr("주소를 입력해 주세요."), 0);
  // A bare address like example.com gets https://, as most servers use it.
  const bool scheme = startsWith(url, "http://") || startsWith(url, "https://");
  if (!scheme && url.find("://") != std::string::npos)
    return fail(tr("주소는 http:// 또는 https://로 시작해야 합니다."), 0);
  server.url = normalizeUrl(scheme ? url : "https://" + url); // trailing slashes go here
  if (server.url.empty()) return fail(tr("주소를 확인해 주세요."), 0);
  for (int field = 1; field < ServerFields; ++field)
    if (fieldOf(server, field).find_first_of("\r\n") != std::string::npos)
      return fail(tr("줄바꿈 없이 입력해 주세요."), field);
  server.title = trimmed(server.title);
  if (server.title.empty()) server.title = hostOf(server.url);
  auto next = config_;
  int index = form.index;
  if (index < 0) {
    if (next.servers.size() >= 32) return fail(tr("서버는 32개까지 등록할 수 있습니다."), ServerSaveRow);
    next.servers.push_back(server);
    index = int(next.servers.size()) - 1;
  } else if (size_t(index) < next.servers.size()) {
    // Nothing changed: 저장 just connects to it, as A on the row used to.
    if (sameServer(next.servers[index], server)) {
      serverForm_.reset();
      if (!(size_t(index) == serverIndex_ && session_)) connect(index);
      return;
    }
    next.servers[index] = server;
  } else {
    serverForm_.reset();
    return;
  }
  if (writeServers(next, index, tr("서버를 저장했습니다."))) {
    serverForm_.reset();
    selected_ = index;
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
  if (writeServers(next, prefer, tr("서버를 삭제했습니다."))) {
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
    alertTitle_ = tr("서버를 저장하지 못했습니다");
    alert_ = tr("config.json에 오류가 있어 덮어쓰지 않았습니다. 파일을 고친 뒤 다시 저장합니다.");
    return false;
  }
  std::string error;
  if (!saveConfig(configPath_, next, error)) {
    logLine("config write failed: " + error);
    alertTitle_ = tr("서버를 저장하지 못했습니다");
    alert_ = trf("config.json을 쓰지 못했습니다. {0}", {error});
    return false;
  }
  logLine("config saved: " + std::to_string(next.servers.size()) + " servers");
  // A connection still running finishes first; - reads the new file then.
  if (network_.joinable() && !done_) {
    message(tr("서버를 저장했습니다. 연결이 끝나면 - 버튼으로 새로고침합니다."));
    return true;
  }
  reload(prefer);
  message(done);
  return true;
}
void Application::serverFormDialog() {
  auto &form = *serverForm_;
  const bool existing = form.index >= 0;
  constexpr float Width = 760, Row = 56, Gap = 8, Button = 52;
  const float errorSpace = form.error.empty() ? 0 : 30;
  const float bodyH = 20 + ServerFields * (Row + Gap) - Gap + 18 + Button + 16 + 26 + errorSpace + 18;
  const auto accept = form.row < ServerFields ? tr("입력") : form.row == ServerSaveRow ? tr("저장") : tr("삭제");
  const auto body = modal(Width, bodyH, existing ? tr("서버 편집") : tr("서버 추가"), tr("취소"),
                          form.row == ServerSaveRow ? "" : tr("저장"), accept, 21);
  const float x = body.x + 32, w = body.w - 64, valueX = x + 170, valueW = w - 190;
  const char *labels[ServerFields] = {tr("주소"), tr("아이디"), tr("비밀번호"), tr("이름")};
  for (int field = 0; field < ServerFields; ++field) {
    const float y = body.y + 20 + field * (Row + Gap), mid = y + Row / 2;
    hit(x, y, w, Row, [this, field] {
      serverForm_->row = field;
      action(Action::Accept);
    });
    if (form.row == field) paint_.focusRow(x, y, w, Row, Ground, 12);
    else paint_.rect(x, y, w, Row, Ground, 12);
    paint_.fonts.middle(labels[field], x + 20, mid, 18, Secondary, true, 140);
    const auto &value = fieldOf(form.server, field);
    if (value.empty()) {
      // What an empty field means: 주소 is needed, 이름 falls back to the host.
      const auto host = field == 3 ? hostOf(normalizeUrl(trimmed(form.server.url))) : "";
      const std::string hint = field == 0 ? tr("예: example.com 또는 http://192.168.0.2:8000")
                             : !host.empty() ? host
                                             : tr("비워 둘 수 있습니다");
      paint_.fonts.middle(hint, valueX, mid, 19, Secondary, false, valueW);
    } else if (field == 2) {
      // The password shows as dots, one per character up to a row's worth.
      const size_t dots = std::min<size_t>(utf8Characters(value).size(), 24);
      for (size_t n = 0; n < dots; ++n) paint_.rect(valueX + n * 18, mid - 5, 10, 10, Ink, 5);
    } else {
      paint_.fonts.middle(value, valueX, mid, 20, Ink, field == 3, valueW);
    }
  }
  // 저장, and 삭제 beside it for a server already there.
  const float buttonY = body.y + 20 + ServerFields * (Row + Gap) - Gap + 18;
  const auto button = [&](int row, const std::string &label, float left, float width, SDL_Color color) {
    hit(left, buttonY, width, Button, [this, row] {
      serverForm_->row = row;
      action(Action::Accept);
    });
    if (form.row == row) paint_.focusRow(left, buttonY, width, Button, Ground, 12);
    else paint_.rect(left, buttonY, width, Button, Ground, 12);
    paint_.fonts.middle(label, left + (width - paint_.fonts.measure(label, 20, true)) / 2, buttonY + Button / 2, 20,
                        color, true);
  };
  button(ServerSaveRow, tr("저장"), x, 200, Ink);
  if (existing) button(ServerDeleteRow, tr("삭제"), x + 216, 160, Bad);
  float lineY = buttonY + Button + 16;
  if (!form.error.empty()) {
    paint_.fonts.text(form.error, x, lineY, 18, Bad, true, w);
    lineY += errorSpace;
  }
  paint_.fonts.text(tr("기기 승인을 쓰는 서버는 저장한 뒤 QR 승인 화면으로 이어집니다."), x, lineY, 17, Secondary,
                    false, w);
  if (!form.confirmDelete) return;
  paint_.rect(0, 0, 1280, 720, {0, 0, 0, 120});
  blockTouch();
  const auto ask = modal(640, 24 + 70 + 16, tr("이 서버를 삭제하시겠습니까?"), tr("취소"), "", tr("삭제"), 37);
  paint_.fonts.text(serverTitle(form.server.title), ask.x + 32, ask.y + 24, 21, Ink, true, ask.w - 64);
  paint_.fonts.text(tr("다른 서버는 그대로 둡니다."), ask.x + 32, ask.y + 62, 19, Secondary, false, ask.w - 64);
}
} // namespace foil::ui
