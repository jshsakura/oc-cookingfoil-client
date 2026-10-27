// 내 기기로 보내기: downloads someone sent this console from a server's
// website. A worker reads every server's queue; the main thread puts what
// fits into the download queue, and the next round answers the servers.
#include "net/SendsApi.hpp"
#include "platform/Cores.hpp"
#include "platform/Focus.hpp"
#include "ui/Application.hpp"
#include <algorithm>
namespace foil::ui {
namespace {
// Every 30 s on 홈 and 다운로드, where a send is looked for (60 s while a
// download runs); 120 s elsewhere.
constexpr uint32_t SendsEveryMs = 30000, SendsBusyMs = 60000, SendsAwayMs = 120000, SendsRetryMs = 3000;
std::string sendKey(const std::string &server, const std::string &id) { return server + "|" + id; }
} // namespace
void Application::pollSends() {
  stepArrivals();
  if (sendsDone_.exchange(false)) {
    sendsWorker_.join();
    takeSends(std::exchange(sendsIncoming_, {}));
  }
  // Only while the app is on screen: under the HOME Menu nothing is asked.
  if (sendsWorker_.joinable() || !session_ || !connected_ || !platform::onScreen()) return;
  // Answers go out at once; a download running asks every 60 s on 홈 and
  // 다운로드 (a 304 is a few hundred bytes) and leaves what it gets alone.
  const bool answers = !sendsAcks_.empty() || !sendsDeclines_.empty();
  const uint32_t now = SDL_GetTicks();
  const bool looked = screen_ == Screen::Home || screen_ == Screen::Downloads;
  const uint32_t every = !looked ? SendsAwayMs : downloadRunning() ? SendsBusyMs : SendsEveryMs;
  if (sendsRead_ && !answers && now - sendsAt_ < every) return;
  const auto servers = serversWith("");
  if (servers.empty()) return;
  sendsRead_ = true;
  sendsAt_ = now;
  const auto state = state_;
  auto acks = std::exchange(sendsAcks_, {});
  auto declines = std::exchange(sendsDeclines_, {});
  sendsWorker_ = std::thread([this, servers, state, acks = std::move(acks), declines = std::move(declines)]() mutable {
    const platform::OnCore off(platform::NetworkCore);
    guarded("device sends", [&] {
      // Answers first, so the lists read next no longer hold what was taken.
      for (const auto &server : servers) {
        const auto ids = acks.find(server.url);
        if (ids == acks.end()) continue;
        if (net::ackSends(server, state, ids->second, true, &stop_)) acks.erase(ids);
        else logLine("device sends: answer to " + server.url + " not sent");
      }
      // One that failed is answered again when the list still holds it.
      for (const auto &server : servers)
        if (const auto ids = declines.find(server.url); ids != declines.end())
          net::ackSends(server, state, ids->second, false, &stop_);
      std::vector<sends::Source> sources;
      for (const auto &server : servers) {
        auto &kept = sendsKept_[server.url];
        auto reply = net::deviceSends(server, state, kept.first, &stop_);
        if (reply.feed) kept = {reply.etag, std::move(*reply.feed)};
        sources.push_back({server.url, kept.second});
      }
      sendsIncoming_ = sends::merge(sources);
    });
    // Answers that did not go out wait for the next round.
    sendsUnsent_ = std::move(acks);
    sendsDone_ = true;
  });
}
// A sent file as the catalog lists it (name, cover, install size), or as
// the server described it when the catalog does not hold it.
Item Application::sentItem(const sends::Pick &pick, std::string &name, std::string &icon) const {
  const auto &sent = pick.item;
  const auto same = [&](const Item &i) {
    return !sent.titleId.empty() && i.size == sent.size &&
           std::equal(i.titleId.begin(), i.titleId.end(), sent.titleId.begin(), sent.titleId.end(),
                      [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); });
  };
  for (const auto &g : groups_) {
    std::vector<const Item *> files;
    if (g.base) files.push_back(&*g.base);
    if (g.update) files.push_back(&*g.update);
    for (const auto &d : g.dlc) files.push_back(&d);
    for (const auto &a : g.alternatives) files.push_back(&a);
    for (const auto *f : files) {
      if (!same(*f)) continue;
      name = g.representative.name;
      icon = g.representative.icon;
      Item item = *f;
      // The copy on the server that sent it, at the address it gave.
      if ((item.server.empty() ? serverInUse() : item.server) != pick.server) {
        item.url = sent.url;
        item.server = pick.server;
      }
      return item;
    }
  }
  Item item;
  item.url = sent.url;
  item.name = sent.name;
  item.titleId = sent.titleId;
  item.size = sent.size;
  item.server = pick.server;
  name = sent.name.empty() ? sent.path : sent.name;
  return item;
}
void Application::takeSends(std::vector<sends::Pick> picks) {
  for (auto &[server, ids] : std::exchange(sendsUnsent_, {}))
    sendsAcks_[server].insert(sendsAcks_[server].end(), ids.begin(), ids.end());
  // What is still listed stays remembered; the rest the servers forgot.
  std::set<std::string> listed;
  for (const auto &p : picks)
    for (const auto &a : p.acks) listed.insert(sendKey(a.server, a.id));
  const auto forget = [&](std::set<std::string> &keys) {
    for (auto it = keys.begin(); it != keys.end();) it = listed.count(*it) ? std::next(it) : keys.erase(it);
  };
  forget(sendsTaken_);
  forget(sendsHeld_);
  picks.erase(std::remove_if(picks.begin(), picks.end(),
                             [&](const sends::Pick &p) {
                               return std::any_of(p.acks.begin(), p.acks.end(), [&](const sends::Ack &a) {
                                 return sendsTaken_.count(sendKey(a.server, a.id)) > 0;
                               });
                             }),
              picks.end());
  // A console scan or the PC holding the card: the next round tries again.
  if (picks.empty() || deviceBusy_ || usbOpen_) return;
  std::vector<Arrival> came;
  std::vector<sends::Candidate> candidates;
  for (const auto &p : picks) {
    Arrival a;
    a.item = sentItem(p, a.title, a.icon);
    a.acks = p.acks;
    candidates.push_back({std::max(a.item.size, a.item.installSize), true});
    came.push_back(std::move(a));
  }
  // Room for what waits on a card as well as what is queued.
  uint64_t queued = queuedBytes();
  for (const auto &a : arrivals_) queued += std::max(a.item.size, a.item.installSize);
  const bool console = installTarget() == "nand";
  const auto admitted = sends::admit(candidates, {console ? nandFree_ : sdFree_, queued, installQueue_.size() + arrivals_.size()});
  // Free space not read yet: asked again shortly, not after the interval.
  if (admitted.waiting) {
    sendsAt_ = SDL_GetTicks() - SendsEveryMs + SendsRetryMs;
    return;
  }
  // Each one a card in turn: unless cancelled in time it goes to the queue.
  std::vector<Arrival> cards;
  for (const size_t i : admitted.take) {
    for (const auto &a : picks[i].acks) sendsTaken_.insert(sendKey(a.server, a.id));
    cards.push_back(std::move(came[i]));
  }
  arrived(std::move(cards));
  // Said once for each: it waits on the server until there is room.
  bool held = false;
  for (const size_t i : admitted.held)
    for (const auto &a : picks[i].acks) held = sendsHeld_.insert(sendKey(a.server, a.id)).second || held;
  if (held) message(tr("A game sent from the web waits until the console has room."));
}
// Its time ran out: into the queue, and the servers told.
void Application::acceptArrival(Arrival arrival) {
  bool fresh = false;
  auto &answers = [&]() -> std::map<std::string, std::vector<std::string>> & {
    if (auto error = enqueueInstall({arrival.item}, arrival.title, arrival.icon, installTarget(), {}, false, fresh)) {
      // Refused here (a system title): the server is told, not asked again.
      logLine("device sends: " + arrival.title + " not queued: " + error.message);
      return sendsDeclines_;
    }
    return sendsAcks_;
  }();
  for (const auto &a : arrival.acks) answers[a.server].push_back(a.id);
  if (fresh)
    if (auto error = launchAdded(1)) alert_ = error.message;
}
// B or a tap on the card: not downloaded, and the servers told.
void Application::cancelArrival() {
  if (arrivals_.empty()) return;
  for (const auto &a : arrivals_.front().acks) sendsDeclines_[a.server].push_back(a.id);
  arrivals_.pop_front();
  arrivalAt_ = SDL_GetTicks();
  sounds_.play(Cue::Move);
}
} // namespace foil::ui
namespace foil::ui {
namespace {
// One card at a time, in the order they came, 6 s each: cancelled in that
// time it is not downloaded, else it goes to the queue. At most this many
// wait; a game's file and names only, the cover from the image cache.
constexpr size_t MaxArrivals = 20;
constexpr uint32_t ArrivalTurnMs = 6000, ArrivalMoveMs = 240;
float easeOut(float t) { return 1 - (1 - t) * (1 - t) * (1 - t); }
} // namespace
void Application::arrived(std::vector<Arrival> games) {
  if (arrivals_.empty()) arrivalAt_ = SDL_GetTicks();
  for (auto &g : games)
    if (arrivals_.size() < MaxArrivals) arrivals_.push_back(std::move(g));
}
// The front card's time: under a popup it stops (the card cannot be seen or
// cancelled there), and with no internet (the game could not come down); once
// it runs out, the game goes to the queue.
void Application::stepArrivals() {
  const uint32_t now = SDL_GetTicks(), last = std::exchange(arrivalFrame_, now);
  if (arrivals_.empty()) return;
  if (popupOpen() || noInternet()) arrivalAt_ += now - last;
  if (now - arrivalAt_ <= ArrivalTurnMs) return;
  auto done = std::move(arrivals_.front());
  arrivals_.pop_front();
  arrivalAt_ = now;
  if (!done.acks.empty()) acceptArrival(std::move(done));
}
void Application::arrivalCard() {
  if (arrivals_.empty()) return;
  const uint32_t now = SDL_GetTicks();
  const bool covered = popupOpen();
  // Drops in from above, eased, and goes back up at the end of its turn.
  const uint32_t age = now - arrivalAt_;
  const float in = easeOut(std::min(1.f, float(age) / ArrivalMoveMs));
  const float out = age + ArrivalMoveMs > ArrivalTurnMs ? easeOut(float(age + ArrivalMoveMs - ArrivalTurnMs) / ArrivalMoveMs) : 0.f;
  constexpr float W = 540, H = 104, Cover = 72, Right = 1280 - 24, Top = 20;
  const float x = Right - W, y = Top - (H + Top + 24) * (1 - in) - (H + Top + 24) * out;
  paint_.rect(x, y + 4, W, H, {0, 0, 0, 40}, 20); // its shadow
  paint_.rect(x, y, W, H, Surface, 20);
  const auto &front = arrivals_.front();
  const float cy = y + (H - Cover) / 2;
  images_->draw(front.icon, x + 14, cy, int(Cover), int(Cover), 14);
  // A butter download mark on the cover's corner: it goes to the queue.
  constexpr float Mark = 26;
  const float mx = x + 14 + Cover - Mark + 6, my = cy + Cover - Mark + 6;
  paint_.rect(mx - 2, my - 2, Mark + 4, Mark + 4, Surface, int(Mark / 2 + 2));
  paint_.rect(mx, my, Mark, Mark, Butter, int(Mark / 2));
  paint_.icon(4, mx + 5, my + 5, OnButter, int(Mark - 10));
  // B 취소 at the right; the card itself is its touch area.
  const std::string cancel = tr("Cancel");
  const float keyW = paint_.keyWidth("B");
  const float bw = 6 + keyW + 8 + paint_.fonts.measure(cancel, 17, true) + 16, bx = x + W - 16 - bw;
  paint_.rect(bx, y + (H - 44) / 2, bw, 44, Ground, 22);
  paint_.key("B", bx + 6, y + H / 2);
  paint_.fonts.middle(cancel, bx + 6 + keyW + 8, y + H / 2, 17, Ink, true);
  if (!covered) hit(x, y, W, H, [this] { cancelArrival(); }, {bx, y + (H - 44) / 2, bw, 44}, 22);
  const float tx = x + 14 + Cover + 16, textW = bx - 14 - tx;
  paint_.fonts.text(front.title, tx, y + 16, 19, Ink, true, textW);
  paint_.fonts.wrapped(tr("Sent from the web. Goes to the download queue unless cancelled."), tx, y + 44, 15, Secondary,
                       textW, 2);
  // Its time left, a butter line along the bottom that shortens.
  const float left = 1 - std::min(1.f, float(age) / ArrivalTurnMs);
  paint_.rect(x + 20, y + H - 6, (W - 40) * left, 3, Butter, 2);
}
} // namespace foil::ui
