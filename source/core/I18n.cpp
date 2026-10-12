#include "core/I18n.hpp"
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
namespace foil {
namespace {
std::atomic<Language> current{Language::Korean};
struct Registry {
  std::vector<std::pair<const Translation *, size_t>> tables;
  std::unordered_map<std::string, const Translation *> index;
  // tr()'s keys by address: most are literals drawn every frame, so this
  // spares copying and hashing the text each time. A hit is checked
  // against the text, since a buffer can hold another key later.
  std::unordered_map<const char *, const Translation *> byAddress;
  size_t indexed = 0; // tables already in index
  std::mutex lock;
};
Registry &registry() {
  static Registry r; // filled during static initialization, read after
  return r;
}
// Tables register during static initialization (tests add one later), so
// the index catches up with any table it has not seen yet.
void catchUp(Registry &r) {
  for (; r.indexed < r.tables.size(); ++r.indexed) {
    const auto &[rows, count] = r.tables[r.indexed];
    for (size_t i = 0; i < count; ++i)
      r.index.emplace(rows[i].ko, &rows[i]);
  }
}
const Translation *lookup(const std::string &korean) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  catchUp(r);
  const auto found = r.index.find(korean);
  return found == r.index.end() ? nullptr : found->second;
}
const Translation *lookupKey(const char *key) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  const auto seen = r.byAddress.find(key);
  if (seen != r.byAddress.end() && std::strcmp(seen->second->ko, key) == 0)
    return seen->second;
  catchUp(r);
  const auto found = r.index.find(key);
  if (found == r.index.end()) {
    if (seen != r.byAddress.end()) r.byAddress.erase(seen);
    return nullptr;
  }
  r.byAddress[key] = found->second;
  return found->second;
}
} // namespace
TranslationTable::TranslationTable(const Translation *rows, size_t count) {
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  r.tables.emplace_back(rows, count);
}
void setLanguage(Language language) { current = language; }
Language language() { return current; }
const char *languageCode(Language language) {
  switch (language) {
  case Language::English: return "en";
  case Language::Japanese: return "ja";
  case Language::Chinese: return "zh";
  case Language::Korean: break;
  }
  return "ko";
}
const char *languageName(Language language) {
  switch (language) {
  case Language::English: return "English";
  case Language::Japanese: return "日本語";
  case Language::Chinese: return "中文";
  case Language::Korean: break;
  }
  return "한국어";
}
Language languageFromCode(const std::string &code, Language fallback) {
  for (int i = 0; i < LanguageCount; ++i)
    if (code == languageCode(Language(i)))
      return Language(i);
  return fallback;
}
const Translation *findTranslation(const std::string &korean) { return lookup(korean); }
std::vector<const Translation *> allTranslations() {
  lookup("");
  auto &r = registry();
  std::lock_guard<std::mutex> guard(r.lock);
  std::vector<const Translation *> out;
  for (const auto &[rows, count] : r.tables)
    for (size_t i = 0; i < count; ++i)
      out.push_back(&rows[i]);
  return out;
}
// "업데이트|항목": the part after | only tells keys apart that read the same
// in Korean but not in other languages; Korean shows the part before it.
const char *korean(const char *key) {
  if (!std::strchr(key, '|'))
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
  if (!key)
    return key;
  if (lang == Language::Korean)
    return korean(key);
  const auto *row = lookupKey(key);
  if (!row)
    return korean(key);
  const char *text = lang == Language::English    ? row->en
                     : lang == Language::Japanese ? row->ja
                                                  : row->zh;
  return text && *text ? text : korean(key);
}
std::string trf(const char *korean, std::initializer_list<std::string> args) {
  std::string text = tr(korean);
  size_t n = 0;
  for (const auto &arg : args) {
    const auto mark = "{" + std::to_string(n++) + "}";
    for (auto at = text.find(mark); at != std::string::npos; at = text.find(mark, at + arg.size()))
      text.replace(at, mark.size(), arg);
  }
  return text;
}
} // namespace foil
