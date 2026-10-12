#pragma once
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>
namespace foil {
// Interface languages. The Korean text in the source is the key; tables in
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

// Screen text: Korean in, the current language out. A missing translation
// falls back to the Korean text (tests keep the tables complete).
const char *tr(const char *korean);
// Same, then {0} {1} ... replaced by args in order.
std::string trf(const char *korean, std::initializer_list<std::string> args);

struct Translation {
  const char *ko, *en, *ja, *zh;
};
// Each source/i18n/*.cpp registers one table with a static instance.
struct TranslationTable {
  TranslationTable(const Translation *rows, size_t count);
};
const Translation *findTranslation(const std::string &korean);
std::vector<const Translation *> allTranslations();
} // namespace foil
