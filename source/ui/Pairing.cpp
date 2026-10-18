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
  request.guide = tr("Enter a message for the admin");
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
  if (!note.empty()) message(tr("Asking for approval again with your message."));
  // Asked again now, and for another full wait; a request already running
  // finishes first and the next poll carries the note.
  pairSince_ = SDL_GetTicks();
  pairPollAt_ = 0;
  refresh();
}
void Application::pairing() {
  pageTitle(tr("Connect server"));
  paint_.rect(260, 90, 992, 466, Surface, 20);
  paint_.pill(tr("Awaiting device approval"), 290, 120, false, 17);
  paint_.fonts.text(tr("Scan this QR code from the server website to approve."), 290, 176, 28, Ink, true, 580);
  // A download that no longer connects says why; the server already lists
  // this console under that download's account, so approving it is the rest.
  const auto &refusal = pairingInfo_.refusal;
  const char *why = refusal == "used"      ? "This download is already connected on another console. Approve this one too in device approval to add it to the same account."
                    : refusal == "expired" ? "This download is more than a day old and no longer connects by itself. Approve this console in device approval to add it to your account."
                    : refusal == "revoked" ? "This download was canceled. Approve this console in device approval to add it to your account."
                                           : "Sign in to the shop on your phone, open device approval and scan the code to approve this device for that account.";
  paint_.fonts.wrapped(tr(why), 290, 230, 21, Secondary, 570, 3);
  if (!pairingInfo_.code.empty()) {
    paint_.fonts.text(tr("Device code"), 290, 326, 18, Secondary);
    paint_.fonts.text(pairingInfo_.code, 290, 357, 30, Ink, true);
  }
  paint_.fonts.wrapped(tr("If you cannot scan it, enter the device code in device approval. Connects automatically once approved."),
                       290, 440, 19, Secondary, 570, 3);
  // The note for the admin, under the card: Y or a touch edits it.
  constexpr float NoteY = 572, NoteH = 60;
  hit(260, NoteY, 992, NoteH, [this] { askPairMessage(); });
  paint_.rect(260, NoteY, 992, NoteH, Surface, 16);
  const float noteMid = NoteY + NoteH / 2;
  const float labelRight = paint_.keyLabel("Y", tr("Message"), 290, noteMid, 18, Secondary, true);
  const auto note = pairMessage();
  paint_.fonts.middle(note.empty() ? tr("Message for the admin (optional)") : note, labelRight + 24, noteMid, 20,
                      note.empty() ? Secondary : Ink, !note.empty(), 1222 - labelRight - 24);
  if (qr_.encode(pairingInfo_.url))
    qr_.draw(paint_.renderer, 908, 165, 300);
  else {
    paint_.fonts.wrapped(tr("This shop does not support QR approval. Enter the device code in device approval on the shop website."),
                         918, 216, 20, Secondary, 270, 6);
  }
}
} // namespace foil::ui
