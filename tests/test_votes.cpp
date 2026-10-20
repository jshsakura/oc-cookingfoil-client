// 잘 돼요 / 안 돼요: what a list carries, what a vote answers, and that
// pressing the vote already cast takes it back.
#include "Test.hpp"
#include "core/Json.hpp"
#include "core/Votes.hpp"
namespace vt = foil::votes;
int runVotesTests() {
  int failures = 0;
  foil::Error ignored;
  const auto item = foil::parseJson(R"({"id":"a","votes":{"up":5,"down":2},"my_vote":"up"})", ignored);
  const auto v = vt::fromItem(item.get());
  CHECK(v.up == 5 && v.down == 2 && v.mine == "up" && v.offered);
  const auto none = foil::parseJson(R"({"id":"b","my_vote":null})", ignored);
  const auto n = vt::fromItem(none.get());
  CHECK(n.up == 0 && n.down == 0 && n.mine.empty() && !n.offered);
  const auto odd = foil::parseJson(R"({"votes":{"up":-3},"my_vote":"sideways"})", ignored);
  const auto o = vt::fromItem(odd.get());
  CHECK(o.up == 0 && o.mine.empty());
  const auto answer = vt::parseAnswer(R"({"up":6,"down":2,"mine":"down"})");
  CHECK(answer.up == 6 && answer.down == 2 && answer.mine == "down");
  CHECK(vt::body(v, "up") == R"({"vote":null})");
  CHECK(vt::body(v, "down") == R"({"vote":"down"})");
  CHECK(vt::body(n, "up") == R"({"vote":"up"})");
  return failures;
}
