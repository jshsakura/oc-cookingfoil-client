#include "core/I18n.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
namespace foil {
namespace {
std::atomic<Language> current{Language::Korean};
struct Table {
  Language language;
  const Translation *rows;
  size_t count;
};
// Keys get an id the first time any table names them; each id holds its
// text per language (null where that table has none).
struct Registry {
  std::vector<Table> tables;
  std::unordered_map<std::string, size_t> ids;
  std::vector<const char *> keys;                             // by id
  std::vector<std::array<const char *, LanguageCount>> texts; // by id
  // tr()'s keys by address: most are literals drawn every frame, so this
  // spares copying and hashing the text each time. A hit is checked
  // against the text, since a buffer can hold another key later.
  std::unordered_map<const char *, size_t> byAddress;
  size_t indexed = 0; // tables already in ids
  std::mutex lock;
};
constexpr size_t None = size_t(-1);
Registry &registry() {
  static Registry r; // filled during static initialization, read after
  return r;
}
// Tables register during static initialization (tests add one later), so
// the index catches up with any table it has not seen yet.
void catchUp(Registry &r) {
  for (; r.indexed < r.tables.size(); ++r.indexed) {
    const auto &table = r.tables[r.indexed];
    for (size_t i = 0; i < table.count; ++i) {
      const auto [found, fresh] = r.ids.emplace(table.rows[i].key, r.texts.size());
      if (fresh) {
        r.keys.push_back(table.rows[i].key);
        r.texts.emplace_back();
      }
      auto &text = r.texts[found->second][size_t(table.language)];
      if (!text) text = table.rows[i].text; // a second row for the key is ignored (the tests refuse it)
    }
  }
}
size_t lookupKey(const char *key) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  const auto seen = r.byAddress.find(key);
  if (seen != r.byAddress.end() && std::strcmp(r.keys[seen->second], key) == 0)
    return seen->second;
  catchUp(r);
  const auto found = r.ids.find(key);
  if (found == r.ids.end()) {
    if (seen != r.byAddress.end()) r.byAddress.erase(seen);
    return None;
  }
  r.byAddress[key] = found->second;
  return found->second;
}
const char *textOf(size_t id, Language language) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  return r.texts[id][size_t(language)];
}
} // namespace
TranslationTable::TranslationTable(Language language, const Translation *rows, size_t count) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  r.tables.push_back({language, rows, count});
}
void setLanguage(Language language) { current = language; }
Language language() { return current; }
namespace {
struct LanguageInfo {
  const char *code, *name;
};
// By Language.
constexpr LanguageInfo Languages[LanguageCount] = {
    {"ko", "한국어"},   {"en", "English"},  {"ja", "日本語"},   {"zh", "简体中文"},
    {"zh-Hant", "繁體中文"}, {"es", "Español"}, {"fr", "Français"}, {"de", "Deutsch"},
    {"it", "Italiano"}, {"pt", "Português (Brasil)"}, {"nl", "Nederlands"}, {"ru", "Русский"}};
} // namespace
const char *languageCode(Language language) { return Languages[int(language)].code; }
const char *languageName(Language language) { return Languages[int(language)].name; }
std::vector<std::string> languageChain(const std::string &code) {
  std::vector<std::string> chain{code};
  if (code == "zh-Hant") chain.push_back("zh");
  if (code != "en") chain.push_back("en");
  return chain;
}
std::string languageLabel(const std::string &code) {
  // Built per call so the names follow the interface language.
  const std::pair<const char *, const char *> names[] = {
      {"ko", tr("Korean")}, {"en", tr("English")}, {"ja", tr("Japanese")}, {"zh", tr("Chinese")},
      {"fr", tr("French")}, {"de", tr("German")}, {"es", tr("Spanish")}, {"it", tr("Italian")},
      {"ru", tr("Russian")}, {"pt", tr("Portuguese")}, {"nl", tr("Dutch")}};
  for (const auto &[key, name] : names)
    if (code.rfind(key, 0) == 0)
      return name;
  return code;
}
const std::string *textIn(const LanguageTexts &texts, const std::string &code) {
  for (const auto &t : texts)
    if (t.code == code && !t.text.empty()) return &t.text;
  return nullptr;
}
const std::string *nearestText(const LanguageTexts &texts, const std::string &code) {
  for (const auto &want : languageChain(code))
    if (const auto *found = textIn(texts, want)) return found;
  return nullptr;
}
const std::string *displayText(const LanguageTexts &texts, const std::string &code) {
  return code == "ko" ? textIn(texts, code) : nearestText(texts, code);
}
Language languageFromCode(const std::string &code, Language fallback) {
  for (int i = 0; i < LanguageCount; ++i)
    if (code == languageCode(Language(i)))
      return Language(i);
  return fallback;
}
std::vector<Language> translatedLanguages() {
  std::vector<Language> out;
  for (int i = 0; i < LanguageCount; ++i)
    if (Language(i) != Language::English) out.push_back(Language(i));
  return out;
}
bool hasTranslation(const std::string &key) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  catchUp(r);
  return r.ids.count(key) > 0;
}
std::vector<Translation> translations(Language language) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  std::vector<Translation> out;
  for (const auto &table : r.tables)
    if (table.language == language) out.insert(out.end(), table.rows, table.rows + table.count);
  return out;
}
const char *english(const char *key) {
  if (!key || !std::strchr(key, '|'))
    return key;
  static std::mutex lock;
  static std::unordered_map<const char *, std::string> shown;
  std::lock_guard<std::mutex> guard(lock);
  auto [it, fresh] = shown.try_emplace(key);
  if (fresh)
    it->second.assign(key, size_t(std::strchr(key, '|') - key));
  return it->second.c_str();
}
const char *tr(const char *key) {
  const auto lang = language();
  if (!key || lang == Language::English)
    return english(key);
  const auto id = lookupKey(key);
  const char *text = id == None ? nullptr : textOf(id, lang);
  return text && *text ? text : english(key);
}
std::string trf(const char *key, std::initializer_list<std::string> args) {
  std::string text = tr(key);
  size_t n = 0;
  for (const auto &arg : args) {
    const auto mark = "{" + std::to_string(n++) + "}";
    for (auto at = text.find(mark); at != std::string::npos; at = text.find(mark, at + arg.size()))
      text.replace(at, mark.size(), arg);
  }
  return text;
}
} // namespace foil
