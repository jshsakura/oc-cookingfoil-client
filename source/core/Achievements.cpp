#include "core/Achievements.hpp"
#include "core/Json.hpp"
#include "core/I18n.hpp"
#include "core/Sha256.hpp"
#include "platform/Seal.hpp"
#include <algorithm>
#include <ctime>
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
Summary parseSummary(json_object *object) { return summaryOf(object); }
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
Games parseGames(const std::string &json, const std::string &base) {
  Games g;
  Error ignored;
  auto root = parseJson(json, ignored);
  if (!root) return g;
  auto *user = field(root.get(), "user");
  if (user && json_object_is_type(user, json_type_object)) {
    g.hasUser = true;
    g.username = stringField(user, "username");
    g.userPoints = count(user, "points");
  }
  auto *list = field(root.get(), "list");
  if (!json_object_is_type(list, json_type_array)) return g;
  for (size_t i = 0; i < json_object_array_length(list) && g.list.size() < 5000; ++i) {
    auto *object = json_object_array_get_idx(list, i);
    if (!json_object_is_type(object, json_type_object)) continue;
    GameRow r;
    r.gameId = stringField(object, "gameId");
    r.title = stringField(object, "title");
    if (r.gameId.empty() || r.title.empty()) continue;
    r.system = stringField(object, "system");
    r.icon = urlField(object, "iconUrl", base);
    r.raGameId = std::max<int64_t>(0, integerField(object, "raGameId", 0));
    r.count = count(object, "count");
    r.points = count(object, "points");
    r.earned = std::min(count(object, "earned"), r.count); // never more earned than there are
    r.earnedHardcore = std::min(count(object, "earnedHardcore"), r.earned);
    r.byTitle = stringField(object, "matchedBy") == "title";
    r.lastEarnedAt = std::max<int64_t>(0, integerField(object, "lastEarnedAt", 0));
    g.list.push_back(std::move(r));
  }
  return g;
}
std::string localDay(int64_t at) {
  const std::time_t t = at;
  std::tm local{};
  localtime_r(&t, &local);
  char text[16];
  std::strftime(text, sizeof(text), "%Y-%m-%d", &local);
  return text;
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
LinkStart parseLinkStart(const std::string &json) {
  LinkStart l;
  Error ignored;
  auto root = parseJson(json, ignored);
  if (!root) return l;
  l.code = stringField(root.get(), "code");
  l.path = stringField(root.get(), "path");
  l.expiresAt = std::max<int64_t>(0, integerField(root.get(), "expiresAt", 0));
  // Only a path on the same server goes into the QR.
  if (l.path.empty() || l.path[0] != '/' || l.path.rfind("//", 0) == 0) l.path = "/ra-link?c=" + l.code;
  return l;
}
std::string linkUrl(const std::string &server, const std::string &path, const std::string &publicKey) {
  auto base = server;
  while (!base.empty() && base.back() == '/') base.pop_back();
  return base + path + "#k=" + publicKey;
}
Sealed parseSealed(const std::string &json) {
  Sealed s;
  Error ignored;
  auto root = parseJson(json, ignored);
  if (!root) return s;
  s.epk = stringField(root.get(), "epk");
  s.iv = stringField(root.get(), "iv");
  s.ct = stringField(root.get(), "ct");
  return s;
}
std::string decodeBase64Url(const std::string &text) {
  std::string out;
  uint32_t bits = 0;
  int held = 0;
  for (const char c : text) {
    int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
            : c == '-' || c == '+' ? 62 : c == '_' || c == '/' ? 63 : c == '=' ? -2 : -1;
    if (v == -2) break; // padding ends it
    if (v < 0) return "";
    bits = (bits << 6) | uint32_t(v);
    held += 6;
    if (held >= 8) {
      held -= 8;
      out += char((bits >> held) & 0xFF);
    }
  }
  return held >= 6 ? "" : out; // a lone sixth character is not base64
}
std::string hkdf(const std::string &ikm, const std::string &info) {
  // One block: PRK = HMAC(zeros, IKM), OKM = HMAC(PRK, info | 0x01).
  const auto prk = hexBytes(hmacSha256(std::string(32, '\0'), ikm));
  return hexBytes(hmacSha256(prk, info + '\x01'));
}
Parsed<Credentials> openEnvelope(const std::string &secret, const Sealed &sealed, const std::string &code) {
  const Error broken{tr("Could not open what the phone sent. Open a new QR code.")};
  const auto epk = decodeBase64Url(sealed.epk), iv = decodeBase64Url(sealed.iv), ct = decodeBase64Url(sealed.ct);
  if (epk.size() != seal::PointSize || iv.size() != 12 || ct.size() <= seal::TagSize) return {{}, broken};
  const auto shared = seal::sharedX(secret, epk);
  if (shared.empty()) return {{}, broken};
  std::string plain;
  if (!seal::open(hkdf(shared, "cookingfoil-ra-link"), iv, code, ct, plain)) return {{}, broken};
  Error ignored;
  auto root = parseJson(plain, ignored);
  Credentials c;
  if (root) {
    c.user = stringField(root.get(), "u");
    c.password = stringField(root.get(), "p");
  }
  std::fill(plain.begin(), plain.end(), '\0');
  if (c.user.empty() || c.password.empty()) return {{}, broken};
  return {c, {}};
}
std::string linkResultBody(bool ok, const std::string &username, const std::string &reason) {
  Json root(json_object_new_object());
  json_object_object_add(root.get(), "ok", json_object_new_boolean(ok));
  if (ok) json_object_object_add(root.get(), "username", json_object_new_string(username.c_str()));
  else if (!reason.empty()) json_object_object_add(root.get(), "reason", json_object_new_string(reason.c_str()));
  return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}
std::string parseLinkState(const std::string &json) {
  Error ignored;
  auto root = parseJson(json, ignored);
  return root ? stringField(root.get(), "state") : "";
}
} // namespace foil::achievements
