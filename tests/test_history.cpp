#include "Test.hpp"
#include "core/DownloadHistory.hpp"
#include <algorithm>
using namespace foil::history;
namespace {
Entry entry(const std::string &name) {
  Entry e;
  e.install.name = name;
  e.install.target = "sd";
  foil::Item file;
  file.url = "https://shop.example/f/" + name;
  file.name = name;
  file.size = 42;
  e.install.files = {file};
  e.at = 100;
  return e;
}
} // namespace
int runHistoryTests() {
  int failures = 0;
  std::vector<Entry> list;
  CHECK(add(list, entry("a")) == 1);
  CHECK(add(list, entry("b")) == 2);
  CHECK(list.front().install.name == "b"); // newest first
  // The newest running one ends; an older one keeps running.
  CHECK(finish(list, Outcome::Failed, "HTTP 500", 200));
  CHECK(list[0].outcome == Outcome::Failed && list[0].error == "HTTP 500" && list[0].at == 200);
  CHECK(list[1].outcome == Outcome::Running);
  // Round trip keeps the files, so a failed one can be tried again.
  const auto back = parse(serialize(list));
  CHECK(back.size() == 2);
  if (back.size() == 2) {
    CHECK(back[0].id == 2 && back[0].outcome == Outcome::Failed && back[0].error == "HTTP 500");
    CHECK(back[0].install.files.size() == 1 && back[0].install.files[0].url == "https://shop.example/f/b");
    CHECK(back[1].outcome == Outcome::Running);
  }
  // Left running by a closed app: an install goes back in the queue to
  // pick up where it stopped.
  auto loaded = back;
  settle(loaded);
  CHECK(loaded[1].outcome == Outcome::Queued);
  // Capped, oldest dropped; ids keep growing.
  std::vector<Entry> many;
  for (int i = 0; i < 5; ++i) {
    auto done = entry(std::to_string(i));
    done.outcome = Outcome::Done;
    add(many, done, 3);
  }
  CHECK(many.size() == 3 && many.front().id == 5 && many.back().install.name == "2");
  CHECK(parse("not json").empty());
  // Kept for years, but one row a file: the same file ending again goes to
  // the top and its older rows go.
  std::vector<Entry> years;
  for (int i = 0; i < 300; ++i) {
    auto done = entry("g" + std::to_string(i));
    done.outcome = Outcome::Done;
    add(years, done);
  }
  CHECK(parse(serialize(years)).size() == 300); // no cap, read back whole
  auto again = entry("g10");
  const auto againId = add(years, again);
  CHECK(end(years, againId, Outcome::Cancelled, "", 300));
  CHECK(years.size() == 300 && years.front().id == againId && years.front().outcome == Outcome::Cancelled);
  CHECK(std::count_if(years.begin(), years.end(), [](const Entry &e) { return e.install.name == "g10"; }) == 1);
  // The queue lives in the same list: a queued entry keeps its pick (group)
  // through a round trip, and the cap never drops it, only finished ones.
  std::vector<Entry> queue;
  auto waiting = entry("q");
  waiting.outcome = Outcome::Queued;
  waiting.group = 7;
  add(queue, waiting, 2);
  for (int i = 0; i < 4; ++i) {
    auto done = entry("d" + std::to_string(i));
    done.outcome = Outcome::Done;
    add(queue, done, 2);
  }
  CHECK(queue.size() == 3 && queue.back().outcome == Outcome::Queued);
  const auto queuedBack = parse(serialize(queue));
  CHECK(queuedBack.size() == 3 && queuedBack.back().outcome == Outcome::Queued && queuedBack.back().group == 7);
  // Retro downloads share the list; each kind finishes only its own entry.
  std::vector<Entry> mixed;
  add(mixed, entry("install"));
  auto game = entry("Zelda");
  game.kind = "retro";
  game.payload = R"j({"id":"nes-1"})j";
  add(mixed, game);
  CHECK(finish(mixed, Outcome::Done, "", 300));
  CHECK(mixed[0].outcome == Outcome::Done && mixed[1].outcome == Outcome::Running); // the ended one goes on top
  CHECK(finish(mixed, Outcome::Cancelled, "", 300, "retro"));
  CHECK(mixed[0].outcome == Outcome::Cancelled);
  CHECK(!finish(mixed, Outcome::Done, "", 300, "retroarch"));
  const auto kept = parse(serialize(mixed));
  CHECK(kept.size() == 2 && kept[0].kind == "retro" && kept[0].payload == R"j({"id":"nes-1"})j");
  CHECK(kept.size() == 2 && kept[1].kind.empty());
  return failures;
}
