// 잘 돼요 / 안 돼요 on a homebrew or retro page: two chips on the dark band,
// left of the admin one. Up from the page's top reaches them, left and
// right pick one, A casts it (or takes it back), up again goes on to the
// admin chip.
#include "net/VotesApi.hpp"
#include "ui/Application.hpp"
namespace foil::ui {
std::optional<Application::VoteTarget> Application::voteTarget() const {
  VoteTarget target;
  if (screen_ == Screen::RetroDetail && !retroPick_.id.empty())
    target = {"retro", retroPick_.id, retroPick_.votes, retroPick_.server};
  else if (homebrewAsk_ && (screen_ == Screen::Homebrew || screen_ == Screen::Downloads)) {
    const auto *app = homebrewPicked();
    if (!app || app->cardOnly) return std::nullopt;
    target = {"homebrew", app->id, app->votes, app->server};
  } else
    return std::nullopt;
  if (const auto cast = votesCast_.find(target.server + "|" + target.kind + "/" + target.id); cast != votesCast_.end())
    target.votes = cast->second;
  if (!target.votes.offered) return std::nullopt;
  return target;
}
bool Application::voteAction(Action a, bool atTop) {
  if (!voteTarget()) {
    voteFocus_ = 0;
    return false;
  }
  if (!voteFocus_) {
    if (a != Action::Up || !atTop || descriptionFocus_ || adminFocus_) return false;
    voteFocus_ = 1;
    return true;
  }
  switch (a) {
  case Action::Back: voteFocus_ = 0; return false;
  case Action::Down: voteFocus_ = 0; break;
  case Action::Up:
    if (adminTarget()) {
      voteFocus_ = 0;
      adminFocus_ = true;
    }
    break;
  case Action::Left: voteFocus_ = 1; break;
  case Action::Right: voteFocus_ = 2; break;
  case Action::Accept:
    if (settled()) castVote(voteFocus_ == 1 ? "up" : "down");
    break;
  default: break;
  }
  return true;
}
void Application::castVote(const std::string &vote) {
  const auto target = voteTarget();
  if (!target || voteJob_.busy()) return;
  const auto key = target->server + "|" + target->kind + "/" + target->id;
  const auto body = votes::body(target->votes, vote);
  if (!session_) {
    // The preview has no server: the count moves as the server's would.
    auto v = target->votes;
    (v.mine == "up" ? v.up : v.down) -= !v.mine.empty();
    v.mine = v.mine == vote ? "" : vote;
    (v.mine == "up" ? v.up : v.down) += !v.mine.empty();
    votesCast_[key] = v;
    return;
  }
  voteJob_.run("vote", [this, server = serverFor(target->server), state = state_, target = *target, key, body]() -> Job::Finish {
    auto answer = net::castVote(server, state, target.kind, target.id, body, &stop_);
    return [this, key, answer = std::move(answer)] {
      if (answer.error) message(answer.error.message);
      else votesCast_[key] = answer.value;
    };
  }, [this] { message(tr("Something went wrong. Check log.txt.")); });
}
std::string Application::voteSummary(const votes::Votes &v) {
  if (!v.offered || (!v.up && !v.down)) return "";
  return std::string(tr("Works")) + " " + std::to_string(v.up) + " · " + tr("Doesn't work") + " " + std::to_string(v.down);
}
float Application::voteChips(float right) {
  const auto target = voteTarget();
  if (!target) {
    voteFocus_ = 0;
    return right;
  }
  constexpr float Height = 34, Top = ChipLine - Height / 2, Gap = 8;
  float x = right + Gap;
  // Drawn right to left: 안 돼요, then 잘 돼요 before it.
  for (int which = 2; which >= 1; --which) {
    const bool up = which == 1;
    const auto count = std::to_string(up ? target->votes.up : target->votes.down);
    const std::string label = std::string(up ? tr("Works") : tr("Doesn't work")) + " " + count;
    const float w = 12 + 18 + 8 + paint_.fonts.measure(label, 16, true) + 14;
    x -= w + Gap;
    const bool focused = voteFocus_ == which, mine = target->votes.mine == (up ? "up" : "down");
    hit(x - Gap / 2, 0, w + Gap, Top + Height + 8, [this, which, up] {
      voteFocus_ = which;
      castVote(up ? "up" : "down");
    });
    const SDL_Color fill = focused ? Butter : mine ? SDL_Color{255, 255, 255, 230} : SDL_Color{255, 255, 255, 41};
    const SDL_Color ink = focused || mine ? Ink : Surface;
    paint_.rect(x, Top, w, Height, fill, 17);
    paint_.icon(up ? 47 : 48, x + 12, Top + (Height - 18) / 2, ink, 18);
    paint_.fonts.middle(label, x + 38, Top + Height / 2, 16, ink, true);
  }
  return x;
}
} // namespace foil::ui
