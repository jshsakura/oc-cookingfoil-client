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
  struct Ready {
    Item item;
    std::string name, icon;
  };
  std::vector<Ready> ready;
  std::vector<sends::Candidate> candidates;
  for (const auto &p : picks) {
    Ready r;
    r.item = sentItem(p, r.name, r.icon);
    candidates.push_back({std::max(r.item.size, r.item.installSize), true});
    ready.push_back(std::move(r));
  }
  const bool console = installTarget() == "nand";
  const auto admitted = sends::admit(candidates, {console ? nandFree_ : sdFree_, queuedBytes(), installQueue_.size()});
  // Free space not read yet: asked again shortly, not after the interval.
  if (admitted.waiting) {
    sendsAt_ = SDL_GetTicks() - SendsEveryMs + SendsRetryMs;
    return;
  }
  int added = 0;
  for (const size_t i : admitted.take) {
    bool fresh = false;
    if (auto error = enqueueInstall({ready[i].item}, ready[i].name, ready[i].icon, installTarget(), {}, false, fresh)) {
      // Refused here (a system title): the server is told, not asked again.
      logLine("device sends: " + ready[i].name + " not queued: " + error.message);
      for (const auto &a : picks[i].acks) {
        sendsTaken_.insert(sendKey(a.server, a.id));
        sendsDeclines_[a.server].push_back(a.id);
      }
      continue;
    }
    added += fresh ? 1 : 0;
    for (const auto &a : picks[i].acks) {
      sendsTaken_.insert(sendKey(a.server, a.id));
      sendsAcks_[a.server].push_back(a.id);
    }
  }
  // Said once for each: it waits on the server until there is room.
  bool held = false;
  for (const size_t i : admitted.held)
    for (const auto &a : picks[i].acks) held = sendsHeld_.insert(sendKey(a.server, a.id)).second || held;
  if (added) {
    if (auto error = launchAdded(added)) alert_ = error.message;
    else message(added > 1 ? trf("Added {0} games sent from the web to the download queue.", {std::to_string(added)})
                           : tr("Added a game sent from the web to the download queue."));
  } else if (held) {
    message(tr("A game sent from the web waits until the console has room."));
  }
}
} // namespace foil::ui
