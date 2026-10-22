#pragma once
// A retro list's filters as the server words them (/api/retro/systems/:id/filters):
// groups of options, each option the query parameters it adds, and the
// sorts. The client draws them in the server's order and never knows a
// filter by name, so a new one (벨트스크롤 액션, a new kind of group) needs
// no client change. A server without them gets the client's own few.
#include <map>
#include <string>
#include <utility>
#include <vector>
namespace foil::retro {
using Texts = std::map<std::string, std::string>; // per language; "" for the client's own
using Params = std::vector<std::pair<std::string, std::string>>;
struct FilterOption {
  std::string key;
  Texts text;
  int count = 0; // games it keeps; 0 when the server does not say
  Params params;
};
struct FilterGroup {
  std::string key;
  Texts text;
  bool single = false; // "pick":"one": choosing an option drops the group's other
  std::vector<FilterOption> options;
};
struct SortOption {
  std::string key; // sent as sort=<key>
  Texts text;
};
struct FilterBook {
  std::vector<FilterGroup> groups;
  std::vector<SortOption> sorts;
  bool empty() const { return groups.empty() && sorts.empty(); }
};
// What the user picked: (group key, option key), in the order picked.
using Picks = std::vector<std::pair<std::string, std::string>>;
FilterBook parseFilterBook(const std::string &json);
// Several servers' books as one: groups and options by key in the order
// they first appear, counts summed (an upper bound when they share games).
FilterBook mergeFilterBooks(const std::vector<FilterBook> &books);
// The text in `lang`, else English, else any.
std::string textIn(const Texts &texts, const std::string &lang);
const FilterOption *findOption(const FilterBook &book, const std::string &group, const std::string &option);
// Picking an option already picked drops it; in a single group it replaces
// the group's other.
Picks togglePick(const FilterBook &book, Picks picks, const std::string &group, const std::string &option);
// Only the picks the book still offers (another system's genres go).
Picks keptPicks(const FilterBook &book, const Picks &picks);
// The list's query parameters: groups AND, a parameter repeated within a
// group joined with commas (any of them).
Params pickParams(const FilterBook &book, const Picks &picks);
} // namespace foil::retro
