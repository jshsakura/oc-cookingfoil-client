#include "core/Votes.hpp"
#include "core/Json.hpp"
#include <algorithm>
namespace foil::votes {
namespace {
int count(json_object *object, const char *key) {
  return int(std::clamp<int64_t>(integerField(object, key), 0, 1 << 30));
}
std::string choice(const std::string &vote) { return vote == "up" || vote == "down" ? vote : ""; }
} // namespace
Votes fromItem(json_object *item) {
  Votes v;
  if (auto *counts = field(item, "votes"); json_object_is_type(counts, json_type_object)) {
    v.up = count(counts, "up");
    v.down = count(counts, "down");
    v.offered = true;
  }
  v.mine = choice(stringField(item, "my_vote", stringField(item, "myVote")));
  return v;
}
Votes parseAnswer(const std::string &json) {
  Votes v;
  Error ignored;
  const auto root = parseJson(json, ignored);
  if (!root) return v;
  v.up = count(root.get(), "up");
  v.down = count(root.get(), "down");
  v.mine = choice(stringField(root.get(), "mine"));
  v.offered = true;
  return v;
}
std::string body(const Votes &now, const std::string &pressed) {
  const auto vote = choice(pressed);
  return vote.empty() || vote == now.mine ? R"({"vote":null})" : R"({"vote":")" + vote + R"("})";
}
} // namespace foil::votes
