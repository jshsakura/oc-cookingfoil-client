#include "core/Achievements.hpp"
#include "core/Json.hpp"
#include <algorithm>
#include <set>
#include <sstream>
namespace foil::achievements {
namespace {
bool flag(json_object *object, const char *key) {
  auto *value = field(object, key);
  return value && json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
}
int count(json_object *object, const char *key) {
  return int(std::clamp<int64_t>(integerField(object, key, 0), 0, 1000000));
}
Summary summaryOf(json_object *object) {
  Summary s;
  if (!object || !json_object_is_type(object, json_type_object)) return s;
  s.raGameId = std::max<int64_t>(0, integerField(object, "raGameId", 0));
  s.count = count(object, "count");
  s.points = count(object, "points");
  s.byTitle = stringField(object, "matchedBy") == "title";
  return s;
}
// `key = "value"` (spaces and quotes optional): the key, and where its value sits.
bool keyOf(const std::string &line, std::string &key) {
  const auto start = line.find_first_not_of(" \t");
  if (start == std::string::npos || line[start] == '#') return false;
  const auto equals = line.find('=', start);
  if (equals == std::string::npos) return false;
  key = line.substr(start, equals - start);
  key.erase(key.find_last_not_of(" \t") + 1);
  return !key.empty();
}
std::string valueOf(const std::string &line) {
  auto value = line.substr(line.find('=') + 1);
  value.erase(0, value.find_first_not_of(" \t"));
  value.erase(value.find_last_not_of(" \t\r") + 1);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
  return value;
}
std::string lineFor(const std::string &key, const std::string &value) {
  // RetroArch's own form; a quote or newline in a value could not be read back.
  std::string clean;
  for (const char c : value)
    if (c != '"' && c != '\n' && c != '\r') clean += c;
  return key + " = \"" + clean + "\"";
}
} // namespace

Account parseAccount(const std::string &json) {
  Account a;
  Error ignored;
  auto root = parseJson(json, ignored);
  if (!root) return a;
  a.enabled = flag(root.get(), "enabled");
  a.username = stringField(root.get(), "username");
  return a;
}
Summary parseSummary(const std::string &detailJson) {
  Error ignored;
  auto root = parseJson(detailJson, ignored);
  return root ? summaryOf(field(root.get(), "achievements")) : Summary{};
}
Game parseGame(const std::string &json, const std::string &base) {
  Game g;
  Error ignored;
  auto root = parseJson(json, ignored);
  if (!root) return g;
  g.summary = summaryOf(root.get());
  g.title = stringField(root.get(), "title");
  auto *user = field(root.get(), "user");
  if (user && json_object_is_type(user, json_type_object)) {
    g.hasUser = true;
    g.username = stringField(user, "username");
    g.earned = count(user, "earned");
    g.earnedHardcore = count(user, "earnedHardcore");
    g.userPoints = count(user, "points");
  }
  auto *list = field(root.get(), "list");
  if (!json_object_is_type(list, json_type_array)) return g;
  for (size_t i = 0; i < json_object_array_length(list) && g.list.size() < 2000; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    if (!json_object_is_type(object, json_type_object)) continue;
    Achievement a;
    a.id = integerField(object, "id", 0);
    a.title = stringField(object, "title");
    if (a.title.empty()) continue;
    a.description = stringField(object, "description");
    a.points = count(object, "points");
    a.earned = flag(object, "earned");
    a.earnedHardcore = flag(object, "earnedHardcore");
    a.earnedAt = std::max<int64_t>(0, integerField(object, "earnedAt", 0));
    a.badge = urlField(object, "badge", base);
    a.badgeLocked = urlField(object, "badgeLocked", base);
    g.list.push_back(std::move(a));
  }
  return g;
}
Login parseLogin(const std::string &json) {
  Login l;
  Error ignored;
  auto root = parseJson(json, ignored);
  if (!root) return l;
  if (flag(root.get(), "Success")) {
    l.user = stringField(root.get(), "User");
    l.token = stringField(root.get(), "Token");
  } else {
    l.error = stringField(root.get(), "Error");
  }
  return l;
}
std::string accountBody(const std::string &username) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "username", json_object_new_string(username.c_str()));
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
std::string setConfig(const std::string &text, const std::map<std::string, std::string> &values) {
  std::istringstream in(text);
  std::string line, out, key;
  std::set<std::string> done;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (keyOf(line, key) && values.count(key)) {
      if (!done.insert(key).second) continue; // a key twice: the first one holds it
      line = lineFor(key, values.at(key));
    }
    out += line + "\n";
  }
  for (const auto &[k, v] : values)
    if (!done.count(k)) out += lineFor(k, v) + "\n";
  return out;
}
std::string configValue(const std::string &text, const std::string &wanted) {
  std::istringstream in(text);
  std::string line, key;
  while (std::getline(in, line))
    if (keyOf(line, key) && key == wanted) return valueOf(line);
  return "";
}
std::string signedInUser(const std::string &configText) {
  if (configValue(configText, "cheevos_enable") != "true" || configValue(configText, "cheevos_token").empty()) return "";
  return configValue(configText, "cheevos_username");
}
std::map<std::string, std::string> signInValues(const std::string &user, const std::string &token) {
  return {{"cheevos_enable", "true"}, {"cheevos_username", user}, {"cheevos_token", token}, {"cheevos_password", ""}};
}
std::map<std::string, std::string> signOutValues() {
  return {{"cheevos_enable", "false"}, {"cheevos_token", ""}, {"cheevos_password", ""}};
}
} // namespace foil::achievements
