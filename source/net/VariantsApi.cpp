#include "net/VariantsApi.hpp"
#include "core/Json.hpp"
#include "core/Log.hpp"
#include "net/Api.hpp"
#include "net/DropboxUtil.hpp"
namespace foil::net {
namespace {
constexpr size_t MostFiles = 200, MostOptions = 8;
// title and body of an object, or a plain string as the body.
void words(json_object *value, std::string &title, std::string &body) {
  if (json_object_is_type(value, json_type_string)) body = json_object_get_string(value);
  else if (json_object_is_type(value, json_type_object)) {
    title = stringField(value, "title");
    body = stringField(value, "body");
  }
}
Rgb colorField(json_object *object, const char *name) {
  auto *value = field(object, name);
  return json_object_is_type(value, json_type_string) ? parseRgb(json_object_get_string(value)) : Rgb{};
}
std::optional<FileOption> parseOption(json_object *object) {
  if (!json_object_is_type(object, json_type_object)) return std::nullopt;
  FileOption option;
  option.id = stringField(object, "id");
  option.label = stringField(object, "label");
  if (option.id.empty() || option.label.empty()) return std::nullopt;
  option.note = stringField(object, "note");
  words(field(object, "confirm"), option.confirmTitle, option.confirmBody);
  option.progress = stringField(object, "progress");
  option.failed = stringField(object, "failed");
  if (auto *blocked = field(object, "blocked"); json_object_is_type(blocked, json_type_object) ||
                                                json_object_is_type(blocked, json_type_string)) {
    option.blocked = true;
    words(blocked, option.blockedTitle, option.blockedBody);
  }
  if (auto *button = field(object, "button"); json_object_is_type(button, json_type_object)) {
    option.buttonColor = colorField(button, "color");
    option.buttonText = colorField(button, "text");
    auto *stripe = field(button, "stripe");
    option.noStripes = json_object_object_get_ex(button, "stripe", nullptr) && !stripe;
    option.buttonStripe = colorField(button, "stripe");
  }
  if (auto *card = field(object, "card"); json_object_is_type(card, json_type_object)) {
    option.cardColor = colorField(card, "color");
    option.cardText = colorField(card, "text");
    option.cardIcon = stringField(card, "icon");
  }
  auto *hashes = field(object, "hashesChange");
  option.hashesChange = json_object_is_type(hashes, json_type_boolean) && json_object_get_boolean(hashes);
  if (auto *query = field(object, "query"); json_object_is_type(query, json_type_object)) {
    json_object_object_foreach(query, key, value) {
      if (json_object_is_type(value, json_type_string) && *key) option.query.emplace_back(key, json_object_get_string(value));
    }
  }
  return option;
}
} // namespace
Rgb parseRgb(const std::string &text) {
  if (text.size() != 7 || text[0] != '#') return {};
  const auto hex = [&](size_t at, uint8_t &out) {
    unsigned value = 0;
    for (size_t i = at; i < at + 2; ++i) {
      const char c = text[i];
      const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (digit < 0) return false;
      value = value * 16 + unsigned(digit);
    }
    out = uint8_t(value);
    return true;
  };
  Rgb rgb;
  rgb.set = hex(1, rgb.r) && hex(3, rgb.g) && hex(5, rgb.b);
  return rgb.set ? rgb : Rgb{};
}
std::optional<std::map<std::string, std::vector<FileOption>>> parseVariants(const std::string &json) {
  Error error;
  const auto root = parseJson(json, error);
  auto *byFile = root ? field(root.get(), "variants") : nullptr;
  if (!json_object_is_type(byFile, json_type_object)) return std::nullopt;
  std::map<std::string, std::vector<FileOption>> out;
  json_object_object_foreach(byFile, file, list) {
    if (!json_object_is_type(list, json_type_array)) continue;
    auto &options = out[file];
    for (size_t i = 0; i < json_object_array_length(list) && options.size() < MostOptions; ++i)
      if (auto option = parseOption(json_object_array_get_idx(list, i))) options.push_back(std::move(*option));
  }
  return out;
}
std::string withQuery(const std::string &url, const std::vector<std::pair<std::string, std::string>> &query) {
  std::string out = url;
  for (const auto &[key, value] : query)
    out += (out.find('?') == std::string::npos ? "?" : "&") + dropbox::percentEncode(key) + "=" + dropbox::percentEncode(value);
  return out;
}
std::map<std::string, std::vector<FileOption>> fileOptions(const Server &server, const State &state,
                                                           const std::vector<std::string> &files,
                                                           const std::atomic<bool> *cancel) {
  if (files.empty()) return {};
  Json body(json_object_new_object());
  auto *list = json_object_new_array();
  for (size_t i = 0; i < files.size() && i < MostFiles; ++i) json_object_array_add(list, json_object_new_string(files[i].c_str()));
  json_object_object_add(body.get(), "files", list);
  const auto url = server.url + "/api/library/variants";
  auto headers = authHeaders(server, state, url);
  headers.push_back("Content-Type: application/json");
  PostOptions options;
  options.timeout = 15;
  options.body = json_object_to_json_string_ext(body.get(), JSON_C_TO_STRING_PLAIN);
  const auto response = post(url, headers, options, cancel);
  if (!response.error.empty() || response.status != 200) {
    logLine("variants: " + (response.error.empty() ? "HTTP " + std::to_string(response.status) : response.error));
    return {};
  }
  auto parsed = parseVariants(response.body);
  if (!parsed) logLine("variants: the reply is not the expected shape");
  return parsed ? std::move(*parsed) : std::map<std::string, std::vector<FileOption>>{};
}
} // namespace foil::net
