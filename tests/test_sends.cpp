// 내 기기로 보내기: what a reply lists, the same game from several servers
// once, and that the queue takes only what leaves the installer's reserve.
#include "Test.hpp"
#include "core/Sends.hpp"
namespace sd = foil::sends;
namespace {
constexpr uint64_t GB = 1024ull * 1024 * 1024;
}
int runSendsTests() {
  int failures = 0;
  const auto feed = sd::parse(R"({"serverId":"s1","items":[
    {"id":"a1","titleId":"0100","size":10,"sentAt":"2026-10-20T10:00:00Z","url":"https://a.example.com/f/1.nsz","name":"One"},
    {"id":"","url":"https://a.example.com/f/2.nsz"},
    {"id":"a3","url":"not a url"}]})");
  CHECK(feed.serverId == "s1" && feed.items.size() == 1 && feed.items[0].name == "One" && feed.items[0].size == 10);
  CHECK(sd::parse("nonsense").items.empty());
  bool shaped = false;
  CHECK(sd::parse(R"({"serverId":"s1","items":[]})", &shaped).items.empty() && shaped);
  for (const char *broken : {"<html>proxy error</html>", "{}", R"({"items":{}})"}) {
    shaped = true;
    sd::parse(broken, &shaped);
    CHECK(!shaped);
  }

  // Server b sent the same game earlier: it comes once, answered on both.
  sd::Feed a{"s1", {{"a1", "", "One", "0100", "https://a.example.com/1", "2026-10-20T10:00:00Z", "", 10},
                    {"a2", "", "Two", "0200", "https://a.example.com/2", "2026-10-19T09:00:00Z", "", 20}}};
  sd::Feed b{"s2", {{"b1", "", "One", "0100", "https://b.example.org/1", "2026-10-18T08:00:00Z", "", 10}}};
  sd::Feed again{"s1", a.items};
  const auto picks = sd::merge({{"https://a.example.com", a}, {"https://b.example.org", b}, {"https://a2.example.com", again}});
  CHECK(picks.size() == 2);
  CHECK(picks[0].item.titleId == "0100" && picks[0].acks.size() == 2 && picks[0].server == "https://a.example.com");
  CHECK(picks[1].item.titleId == "0200" && picks[1].acks.size() == 1);

  // Free space unknown: nothing is decided yet.
  const auto unknown = sd::admit({{GB, true}}, {std::nullopt, 0, 0});
  CHECK(unknown.waiting && unknown.take.empty() && unknown.held.empty());

  // 10GB free, 3GB already queued: 4GB fits, the 5GB after it would leave
  // less than the reserve, the 2GB after that still fits.
  const auto room = sd::admit({{4 * GB, true}, {5 * GB, true}, {2 * GB, true}}, {10 * GB, 3 * GB, 1});
  CHECK(room.take == std::vector<size_t>({0, 2}) && room.held == std::vector<size_t>({1}));

  // Exactly the reserve left is fine; one byte less is not.
  CHECK(sd::admit({{GB - sd::Reserve, true}}, {GB, 0, 0}).take.size() == 1);
  CHECK(sd::admit({{GB - sd::Reserve + 1, true}}, {GB, 0, 0}).held.size() == 1);

  // A queue that already needs more than is free takes nothing.
  CHECK(sd::admit({{1, true}}, {GB, 2 * GB, 1}).held.size() == 1);

  // Firmware too old, and a full queue.
  CHECK(sd::admit({{1, false}}, {10 * GB, 0, 0}).held.size() == 1);
  CHECK(sd::admit({{1, true}}, {10 * GB, 0, sd::MaxQueue}).held.size() == 1);

  // Only as many as can wait on screen: the rest are left for the next round.
  const auto cards = sd::admit(std::vector<sd::Candidate>(25, {1, true}), {10 * GB, 0, 0, 20});
  CHECK(cards.take.size() == 20 && cards.held.empty() && cards.take.back() == 19);
  CHECK(sd::admit({{1, true}}, {10 * GB, 0, 0, 0}).take.empty());

  CHECK(sd::ackBody({"a1", "b1"}, true) == R"({"ids":["a1","b1"],"result":"accepted"})");
  CHECK(sd::ackBody({}, false) == R"({"ids":[],"result":"declined"})");
  return failures;
}
