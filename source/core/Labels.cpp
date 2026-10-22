#include "core/Labels.hpp"
#include "core/Json.hpp"
#include "core/Retro.hpp"
namespace foil::labels {
namespace {
constexpr size_t MostRefs = 8, MostValue = 32;
std::map<std::string, std::string> texts(json_object *object) {
  std::map<std::string, std::string> out;
  if (!json_object_is_type(object, json_type_object)) return out;
  json_object_object_foreach(object, lang, text)
    if (json_object_is_type(text, json_type_string) && *json_object_get_string(text) && std::string(lang).size() <= 8)
      out[lang] = json_object_get_string(text);
  return out;
}
Tone toneOf(const std::string &name) {
  return name == "accent" ? Tone::Accent
         : name == "good" ? Tone::Good
         : name == "warn" ? Tone::Warn
         : name == "bad"  ? Tone::Bad
                          : Tone::Neutral;
}
// A value the server gives as text or a number.
std::string valueOf(json_object *value) {
  if (json_object_is_type(value, json_type_int)) return std::to_string(json_object_get_int64(value));
  if (!json_object_is_type(value, json_type_string)) return {};
  std::string text = json_object_get_string(value);
  return text.size() <= MostValue ? text : std::string();
}
const std::string *inLanguage(const std::map<std::string, std::string> &texts, const std::string &lang) {
  auto found = texts.find(lang);
  if (found == texts.end()) found = texts.find("en");
  return found == texts.end() ? nullptr : &found->second;
}
} // namespace
Book parseBook(json_object *root) {
  Book book;
  auto *table = field(root, "labels");
  if (!json_object_is_type(table, json_type_object)) return book;
  json_object_object_foreach(table, key, entry) {
    if (!retro::keyText(key) || !json_object_is_type(entry, json_type_object)) continue;
    Def def;
    // Every language at the entry's top level ("ko", "en"…), beside its
    // tone, icon, order and plural forms.
    json_object_object_foreach(entry, name, value) {
      const std::string member = name;
      if (member == "tone") def.tone = toneOf(valueOf(value));
      else if (member == "icon") def.icon = retro::keyText(valueOf(value)) ? valueOf(value) : "";
      else if (member == "order") def.order = int(json_object_get_int(value));
      else if (member == "one") def.one = texts(value);
      else if (json_object_is_type(value, json_type_string) && *json_object_get_string(value) && member.size() <= 8)
        def.text[member] = json_object_get_string(value);
    }
    if (!def.text.empty()) book[key] = std::move(def);
  }
  return book;
}
std::vector<Ref> parseRefs(json_object *game, bool &present) {
  std::vector<Ref> refs;
  auto *list = field(game, "labels");
  present = json_object_is_type(list, json_type_array);
  if (!present) return refs;
  for (size_t i = 0, n = json_object_array_length(list); i < n && refs.size() < MostRefs; ++i) {
    auto *entry = json_object_array_get_idx(list, i);
    std::string key, value;
    if (json_object_is_type(entry, json_type_string)) key = json_object_get_string(entry);
    else if (json_object_is_type(entry, json_type_array) && json_object_array_length(entry) >= 1) {
      key = valueOf(json_object_array_get_idx(entry, 0));
      value = valueOf(json_object_array_get_idx(entry, 1));
    }
    if (retro::keyText(key)) refs.push_back({Word(key), Word(value)});
  }
  return refs;
}
void merge(Book &into, const Book &from) {
  for (const auto &[key, def] : from) into.emplace(key, def);
}
std::string text(const Book &book, const Ref &ref, const std::string &lang) {
  const auto found = book.find(ref.key.str());
  if (found == book.end()) return {};
  const auto &def = found->second;
  // One language for both forms, the first of languageChain the label has.
  // Korean has no singular and must not take English's.
  std::string language = "en";
  for (const auto &want : languageChain(lang))
    if (def.text.count(want)) {
      language = want;
      break;
    }
  const auto single = ref.value == "1" ? def.one.find(language) : def.one.end();
  const std::string *pattern = single != def.one.end() ? &single->second : inLanguage(def.text, language);
  if (!pattern) return {};
  std::string out = *pattern;
  if (const auto at = out.find("{0}"); at != std::string::npos) out.replace(at, 3, ref.value.str());
  return out;
}
} // namespace foil::labels
