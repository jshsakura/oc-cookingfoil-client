#include "ui/Application.hpp"
#include "net/Api.hpp"
namespace foil::ui {
std::string Application::pairMessage() const {
  if (!session_) return "";
  const auto found = state_.pairMessages.find(session_->server().url);
  return found == state_.pairMessages.end() ? "" : found->second;
}
void Application::askPairMessage() {
  if (!session_) return;
  TextRequest request;
  request.guide = tr("관리자에게 보낼 메시지를 입력합니다");
  request.initial = pairMessage();
  request.maxLength = int(net::PairMessageLimit);
  textRequest_ = request;
  textField_ = -1;
  pairMessageField_ = true;
}
void Application::setPairMessage(const std::string &text) {
  if (!session_) return;
  const auto note = net::cleanPairMessage(text);
  if (note == pairMessage()) return;
  const auto &url = session_->server().url;
  if (note.empty()) state_.pairMessages.erase(url);
  else state_.pairMessages[url] = note;
  saveStateQuietly();
  if (!note.empty()) message(tr("메시지를 담아 승인을 다시 요청합니다."));
  // Asked again now, and for another full wait; a request already running
  // finishes first and the next poll carries the note.
  pairSince_ = SDL_GetTicks();
  pairPollAt_ = 0;
  refresh();
}
void Application::pairing() {
  pageTitle(tr("서버 연결"));
  paint_.rect(260, 90, 992, 466, Surface, 20);
  paint_.pill(tr("기기 승인 대기"), 290, 120, false, 17);
  paint_.fonts.text(tr("서버 웹페이지에서 이 QR 코드를 찍어 승인합니다."), 290, 176, 28, Ink, true, 580);
  paint_.fonts.wrapped(tr("휴대폰으로 서버에 로그인해 기기 승인 화면을 열고 QR 코드를 찍으면 그 계정으로 이 기기를 승인합니다."),
                       290, 230, 21, Secondary, 570, 3);
  if (!pairingInfo_.code.empty()) {
    paint_.fonts.text(tr("기기 코드"), 290, 326, 18, Secondary);
    paint_.fonts.text(pairingInfo_.code, 290, 357, 30, Ink, true);
  }
  paint_.fonts.wrapped(tr("QR 코드를 찍기 어려우면 기기 승인 화면에 기기 코드를 입력합니다. 승인되면 자동으로 연결합니다."),
                       290, 440, 19, Secondary, 570, 3);
  // The note for the admin, under the card: Y or a touch edits it.
  constexpr float NoteY = 572, NoteH = 60;
  hit(260, NoteY, 992, NoteH, [this] { askPairMessage(); });
  paint_.rect(260, NoteY, 992, NoteH, Surface, 16);
  const float noteMid = NoteY + NoteH / 2;
  const float labelRight = paint_.keyLabel("Y", tr("메시지"), 290, noteMid, 18, Secondary, true);
  const auto note = pairMessage();
  paint_.fonts.middle(note.empty() ? tr("관리자에게 보낼 메시지 (선택)") : note, labelRight + 24, noteMid, 20,
                      note.empty() ? Secondary : Ink, !note.empty(), 1222 - labelRight - 24);
  if (qr_.encode(pairingInfo_.url))
    qr_.draw(paint_.renderer, 908, 165, 300);
  else {
    paint_.fonts.wrapped(tr("이 서버는 QR 승인을 지원하지 않습니다. 서버 웹페이지의 기기 승인 화면에 기기 코드를 입력합니다."),
                         918, 216, 20, Secondary, 270, 6);
  }
}
} // namespace foil::ui
