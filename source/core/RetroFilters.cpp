#include "core/RetroFilters.hpp"
#include "core/Json.hpp"
#include "core/Retro.hpp"
#include <algorithm>
#include <cctype>
namespace foil::retro {
namespace {
constexpr size_t MostGroups = 16, MostOptions = 64, MostSorts = 12, MostParams = 4, MostValue = 64;
bool paramName(const std::string &name) {
  return !name.empty() && name.size() <= 24 && std::isalpha(static_cast<unsigned char>(name[0])) &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) { return std::isalnum(c); });
}
bool paramValue(const std::string &value) {
  return !value.empty() && value.size() <= MostValue && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '.' || c == '_';
  });
}
// Every language at the entry's top level, beside its own members.
Texts texts(json_object *entry) {
  Texts out;
  json_object_object_foreach(entry, name, value) {
    const std::string lang = name;
    if (lang == "key" || lang == "pick" || lang == "icon") continue;
    if (lang.size() >= 2 && lang.size() <= 8 && json_object_is_type(value, json_type_string) && *json_object_get_string(value) &&
        std::all_of(lang.begin(), lang.end(), [](unsigned char c) { return std::isalpha(c) || c == '-'; })) // zh-Hant keeps its capital
      out[lang] = json_object_get_string(value);
  }
  return out;
}
FilterOption parseOption(json_object *entry) {
  FilterOption option{stringField(entry, "key"), texts(entry), int(std::clamp<int64_t>(integerField(entry, "count"), 0, 1 << 30)), {}};
  auto *params = field(entry, "params");
  if (!json_object_is_type(params, json_type_object)) return {};
  json_object_object_foreach(params, name, value) {
    std::string text = json_object_is_type(value, json_type_int) ? std::to_string(json_object_get_int64(value))
                       : json_object_is_type(value, json_type_string) ? json_object_get_string(value)
                                                                       : "";
    if (paramName(name) && paramValue(text) && option.params.size() < MostParams) option.params.push_back({name, text});
  }
  if (!keyText(option.key) || option.text.empty() || option.params.empty()) return {};
  return option;
}
} // namespace
FilterBook parseFilterBook(const std::string &json) {
  FilterBook book;
  Error error;
  auto root = parseJson(json, error);
  if (!root) return book;
  auto *groups = field(root.get(), "groups");
  for (size_t i = 0; json_object_is_type(groups, json_type_array) && i < json_object_array_length(groups) &&
                     book.groups.size() < MostGroups;
       ++i) {
    auto *entry = json_object_array_get_idx(groups, i);
    if (!json_object_is_type(entry, json_type_object)) continue;
    FilterGroup group{stringField(entry, "key"), texts(entry), stringField(entry, "pick") == "one", {}};
    auto *options = field(entry, "options");
    for (size_t n = 0; json_object_is_type(options, json_type_array) && n < json_object_array_length(options) &&
                       group.options.size() < MostOptions;
         ++n) {
      auto option = parseOption(json_object_array_get_idx(options, n));
      const bool seen = std::any_of(group.options.begin(), group.options.end(), [&](const auto &o) { return o.key == option.key; });
      if (!option.key.empty() && !seen) group.options.push_back(std::move(option));
    }
    const bool seen = std::any_of(book.groups.begin(), book.groups.end(), [&](const auto &g) { return g.key == group.key; });
    if (keyText(group.key) && !group.text.empty() && !group.options.empty() && !seen) book.groups.push_back(std::move(group));
  }
  auto *sorts = field(root.get(), "sorts");
  for (size_t i = 0; json_object_is_type(sorts, json_type_array) && i < json_object_array_length(sorts) &&
                     book.sorts.size() < MostSorts;
       ++i) {
    auto *entry = json_object_array_get_idx(sorts, i);
    if (!json_object_is_type(entry, json_type_object)) continue;
    SortOption sort{stringField(entry, "key"), texts(entry)};
    if (keyText(sort.key) && !sort.text.empty()) book.sorts.push_back(std::move(sort));
  }
  return book;
}
FilterBook mergeFilterBooks(const std::vector<FilterBook> &books) {
  FilterBook out;
  for (const auto &book : books) {
    for (const auto &group : book.groups) {
      auto into = std::find_if(out.groups.begin(), out.groups.end(), [&](const auto &g) { return g.key == group.key; });
      if (into == out.groups.end()) {
        out.groups.push_back(group);
        continue;
      }
      for (const auto &option : group.options) {
        auto same = std::find_if(into->options.begin(), into->options.end(), [&](const auto &o) { return o.key == option.key; });
        if (same == into->options.end()) into->options.push_back(option);
        else same->count += option.count;
      }
    }
    for (const auto &sort : book.sorts)
      if (std::none_of(out.sorts.begin(), out.sorts.end(), [&](const auto &s) { return s.key == sort.key; }))
        out.sorts.push_back(sort);
  }
  return out;
}
std::string textIn(const Texts &texts, const std::string &lang) {
  for (const auto &want : languageChain(lang))
    if (const auto found = texts.find(want); found != texts.end()) return found->second;
  return texts.empty() ? std::string() : texts.begin()->second;
}
const FilterOption *findOption(const FilterBook &book, const std::string &group, const std::string &option) {
  for (const auto &g : book.groups)
    if (g.key == group)
      for (const auto &o : g.options)
        if (o.key == option) return &o;
  return nullptr;
}
Picks togglePick(const FilterBook &book, Picks picks, const std::string &group, const std::string &option) {
  const std::pair<std::string, std::string> pick{group, option};
  if (const auto found = std::find(picks.begin(), picks.end(), pick); found != picks.end()) {
    picks.erase(found);
    return picks;
  }
  if (!findOption(book, group, option)) return picks;
  const auto g = std::find_if(book.groups.begin(), book.groups.end(), [&](const auto &x) { return x.key == group; });
  if (g->single)
    picks.erase(std::remove_if(picks.begin(), picks.end(), [&](const auto &p) { return p.first == group; }), picks.end());
  picks.push_back(pick);
  return picks;
}
Picks keptPicks(const FilterBook &book, const Picks &picks) {
  Picks out;
  for (const auto &pick : picks)
    if (findOption(book, pick.first, pick.second)) out.push_back(pick);
  return out;
}
Params pickParams(const FilterBook &book, const Picks &picks) {
  Params out;
  for (const auto &group : book.groups) {
    Params mine;
    for (const auto &option : group.options) {
      if (std::find(picks.begin(), picks.end(), std::make_pair(group.key, option.key)) == picks.end()) continue;
      for (const auto &[name, value] : option.params) {
        auto same = std::find_if(mine.begin(), mine.end(), [&](const auto &p) { return p.first == name; });
        if (same == mine.end()) mine.push_back({name, value});
        else if (("," + same->second + ",").find("," + value + ",") == std::string::npos) same->second += "," + value;
      }
    }
    out.insert(out.end(), mine.begin(), mine.end());
  }
  return out;
}
} // namespace foil::retro
