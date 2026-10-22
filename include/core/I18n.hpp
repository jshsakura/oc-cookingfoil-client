#pragma once
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>
namespace foil {
// Interface languages. The English text in the source is the key; tables in
// source/i18n/ give the other languages.
enum class Language { Korean, English, Japanese, Chinese };
constexpr int LanguageCount = 4;
void setLanguage(Language language);
Language language();
// "ko", "en", "ja", "zh": what the server's ?lang= and name tables use.
const char *languageCode(Language language);
inline const char *languageCode() { return languageCode(language()); }
// The language's own name, as a picker shows it (한국어, English, 日本語, 中文).
const char *languageName(Language language);
Language languageFromCode(const std::string &code, Language fallback);
// Every language but English, which needs no table.
std::vector<Language> translatedLanguages();
// A content language code ("ko", "en-US", "zh-TW") as the interface names
// it (한국어, 영어, 중국어); one it does not know comes back as it is.
std::string languageLabel(const std::string &code);

// Screen text: the English key in, the current language out. "Name|sort":
// the part after | only tells apart keys that read the same in English;
// English shows the part before it. A missing translation falls back to
// English (tests keep the tables complete).
const char *tr(const char *key);
// Same, then {0} {1} ... replaced by args in order.
std::string trf(const char *key, std::initializer_list<std::string> args);
// The English a key shows ("Name|sort" -> "Name").
const char *english(const char *key);

// One row of a language's table: the English key and its text there.
struct Translation {
  const char *key, *text;
};
// Each source/i18n/<code>.cpp registers its language's table with a static
// instance. English has none: its text is the key.
struct TranslationTable {
  TranslationTable(Language language, const Translation *rows, size_t count);
};
// Whether a key is in any table (screen text in the code must be).
bool hasTranslation(const std::string &key);
// Every row registered for `language`, in table order (the tests check them).
std::vector<Translation> translations(Language language);
} // namespace foil
