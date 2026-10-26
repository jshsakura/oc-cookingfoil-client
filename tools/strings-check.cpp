// The server dictionary's checks, run where the dictionary lives (its CI) so
// what the console would refuse is caught before it ships:
//   strings-check keys          every key the app draws, as JSON
//   strings-check locked        the keys a server may not reword, as JSON
//   strings-check languages     the languages the app ships, as JSON
//   strings-check check FILE    one language's dictionary (a flat JSON object)
// `check` prints a JSON report and exits 1 when anything in it is an error:
// an unknown key, a text that fails i18n::checkText, or a character the
// console cannot draw. A character no bundled font has is a warning: the
// console draws it from its own shared fonts, in another face.
#include "core/I18n.hpp"
#include "core/I18nChecks.hpp"
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <json-c/json.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
namespace {
using foil::Language;
// Every key once, in the Korean table's order (all tables hold the same).
std::vector<std::string> keys() {
  std::vector<std::string> out;
  for (const auto &row : foil::translations(Language::Korean)) out.push_back(row.key);
  return out;
}
json_object *strings(const std::set<std::string> &values) {
  auto *array = json_object_new_array();
  for (const auto &value : values) json_object_array_add(array, json_object_new_string(value.c_str()));
  return array;
}
void print(json_object *value) {
  std::puts(json_object_to_json_string_ext(value, JSON_C_TO_STRING_PRETTY | JSON_C_TO_STRING_NOSLASHESCAPE));
  json_object_put(value);
}
int listKeys() {
  auto *list = json_object_new_array();
  for (const auto &key : keys()) {
    auto *entry = json_object_new_object();
    json_object_object_add(entry, "key", json_object_new_string(key.c_str()));
    const auto shown = foil::i18n::englishOf(key);
    json_object_object_add(entry, "english", json_object_new_string(shown.c_str()));
    json_object_object_add(entry, "placeholders", strings(foil::i18n::placeholders(shown)));
    json_object_object_add(entry, "locked", json_object_new_boolean(foil::i18n::locked(key)));
    json_object_array_add(list, entry);
  }
  print(list);
  return 0;
}
int listLocked() {
  auto *list = json_object_new_array();
  for (const auto *key : foil::i18n::lockedKeys()) json_object_array_add(list, json_object_new_string(key));
  print(list);
  return 0;
}
int listLanguages() {
  auto *list = json_object_new_array();
  for (int i = 0; i < foil::LanguageCount; ++i) {
    const auto language = Language(i);
    const std::string code = foil::languageCode(language);
    auto *entry = json_object_new_object();
    json_object_object_add(entry, "code", json_object_new_string(code.c_str()));
    json_object_object_add(entry, "name", json_object_new_string(foil::languageName(language)));
    const auto chain = foil::languageChain(code);
    // The language it falls back to before English, when there is one.
    json_object_object_add(entry, "base",
                           chain.size() > 2 ? json_object_new_string(chain[1].c_str()) : nullptr);
    json_object_array_add(list, entry);
  }
  print(list);
  return 0;
}
struct Fonts {
  std::vector<TTF_Font *> faces;
  explicit Fonts(const std::string &dir) {
    for (const auto *file : {"CookingFoilSans-%s.otf", "CookingFoilSansJP-%s.otf", "CookingFoilSansSC-%s.ttf",
                             "CookingFoilSansTC-%s.ttf"})
      for (const auto *weight : {"Regular", "Bold"}) {
        char name[64];
        std::snprintf(name, sizeof name, file, weight);
        if (auto *font = TTF_OpenFont((dir + "/" + name).c_str(), 18)) faces.push_back(font);
      }
  }
  ~Fonts() {
    for (auto *font : faces) TTF_CloseFont(font);
  }
  bool bundled(uint32_t point) const {
    for (auto *font : faces)
      if (TTF_GlyphIsProvided32(font, point)) return true;
    return false;
  }
};
json_object *finding(const std::string &key, const std::string &what) {
  auto *entry = json_object_new_object();
  json_object_object_add(entry, "key", json_object_new_string(key.c_str()));
  json_object_object_add(entry, "problem", json_object_new_string(what.c_str()));
  return entry;
}
std::string hex(uint32_t point) {
  char text[16];
  std::snprintf(text, sizeof text, "U+%04X", point);
  return text;
}
int check(const std::string &path, const std::string &fontDir) {
  auto *root = json_object_from_file(path.c_str());
  if (!json_object_is_type(root, json_type_object)) {
    std::fprintf(stderr, "%s: not a JSON object\n", path.c_str());
    json_object_put(root);
    return 2;
  }
  const Fonts fonts(fontDir);
  if (fonts.faces.size() != 8) {
    std::fprintf(stderr, "%s: the eight bundled fonts did not open\n", fontDir.c_str());
    json_object_put(root);
    return 2;
  }
  const auto known = keys();
  const std::set<std::string> knownSet(known.begin(), known.end());
  auto *errors = json_object_new_array(), *warnings = json_object_new_array();
  size_t entries = 0, lockedEntries = 0;
  json_object_object_foreach(root, key, value) {
    ++entries;
    if (!knownSet.count(key)) {
      json_object_array_add(errors, finding(key, "unknown-key"));
      continue;
    }
    if (!json_object_is_type(value, json_type_string)) {
      json_object_array_add(errors, finding(key, "not-a-string"));
      continue;
    }
    const std::string text = json_object_get_string(value);
    lockedEntries += foil::i18n::locked(key);
    if (const auto problem = foil::i18n::checkText(key, text); problem != foil::i18n::Problem::None) {
      json_object_array_add(errors, finding(key, foil::i18n::problemName(problem)));
      continue;
    }
    std::set<uint32_t> undrawable, shared;
    for (const auto point : foil::i18n::codePoints(text)) {
      if (point < 0x20 || point == 0xFE0F) continue; // line breaks and variation selectors draw nothing
      if (foil::i18n::undrawable(point)) undrawable.insert(point);
      else if (!fonts.bundled(point)) shared.insert(point);
    }
    for (const auto point : undrawable) json_object_array_add(errors, finding(key, "undrawable " + hex(point)));
    for (const auto point : shared) json_object_array_add(warnings, finding(key, "shared-font " + hex(point)));
  }
  json_object_put(root);
  auto *report = json_object_new_object();
  const auto failed = json_object_array_length(errors) > 0;
  json_object_object_add(report, "entries", json_object_new_int64(int64_t(entries)));
  json_object_object_add(report, "keys", json_object_new_int64(int64_t(known.size())));
  json_object_object_add(report, "locked", json_object_new_int64(int64_t(lockedEntries)));
  json_object_object_add(report, "errors", errors);
  json_object_object_add(report, "warnings", warnings);
  print(report);
  return failed ? 1 : 0;
}
int usage() {
  std::fputs("usage: strings-check keys | locked | languages | check FILE [--fonts DIR]\n", stderr);
  return 2;
}
} // namespace
int main(int argc, char **argv) {
  if (argc < 2) return usage();
  const std::string command = argv[1];
  if (command == "keys") return listKeys();
  if (command == "locked") return listLocked();
  if (command == "languages") return listLanguages();
  if (command != "check" || argc < 3) return usage();
  std::string fontDir = "host/fonts";
  for (int i = 3; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], "--fonts") == 0) fontDir = argv[i + 1];
  if (TTF_Init() != 0) {
    std::fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
    return 2;
  }
  const int result = check(argv[2], fontDir);
  TTF_Quit();
  return result;
}
