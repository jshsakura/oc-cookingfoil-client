#include "Test.hpp"
#include "core/DownloadHistory.hpp"
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
  // Left running by a closed app: reads as failed.
  auto loaded = back;
  settle(loaded);
  CHECK(loaded[1].outcome == Outcome::Failed);
  // Capped, oldest dropped; ids keep growing.
  std::vector<Entry> many;
  for (int i = 0; i < 5; ++i) add(many, entry(std::to_string(i)), 3);
  CHECK(many.size() == 3 && many.front().id == 5 && many.back().install.name == "2");
  CHECK(parse("not json").empty());
  // Retro downloads share the list; each kind finishes only its own entry.
  std::vector<Entry> mixed;
  add(mixed, entry("install"));
  auto game = entry("Zelda");
  game.kind = "retro";
  game.payload = R"j({"id":"nes-1"})j";
  add(mixed, game);
  CHECK(finish(mixed, Outcome::Done, "", 300));
  CHECK(mixed[0].outcome == Outcome::Running && mixed[1].outcome == Outcome::Done);
  CHECK(finish(mixed, Outcome::Cancelled, "", 300, "retro"));
  CHECK(mixed[0].outcome == Outcome::Cancelled);
  CHECK(!finish(mixed, Outcome::Done, "", 300, "retroarch"));
  const auto kept = parse(serialize(mixed));
  CHECK(kept.size() == 2 && kept[0].kind == "retro" && kept[0].payload == R"j({"id":"nes-1"})j");
  CHECK(kept.size() == 2 && kept[1].kind.empty());
  return failures;
}
