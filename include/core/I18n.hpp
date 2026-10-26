#pragma once
#include <cstddef>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>
namespace foil {
// Interface languages, in the picker's order. The English text in the source
// is the key; tables in source/i18n/ give the other languages.
enum class Language {
  Korean,
  English,
  Japanese,
  Chinese, // simplified
  ChineseTraditional,
  Spanish,
  French,
  German,
  Italian,
  Portuguese, // Brazilian
  Dutch,
  Russian,
};
constexpr int LanguageCount = 12;
// Also leaves an ExtraLanguage in use.
void setLanguage(Language language);
// The language shown; with an ExtraLanguage in use, its base when the app
// ships that, else English (fonts, prices).
Language language();
// "ko", "en", "zh-Hant", ...: what the server's ?lang= and name tables use.
const char *languageCode(Language language);
// The current one's, an ExtraLanguage's own code while it is in use.
const char *languageCode();
// A language the app does not ship, only a server's dictionary has (feature
// "client-strings"): its code, its own name, and the one language whose text
// fills its gaps (empty for English).
struct ExtraLanguage {
  std::string code, name, base;
};
// Shows `extra` from now on, by key: its server text, then its base's (the
// built-in table, or `baseTexts` when the app does not ship the base), then
// English. Locked keys (i18n::locked) show English. Texts are checked as in
// setServerTexts. False, and nothing changes, when `texts` has none to use.
bool useExtraLanguage(const ExtraLanguage &extra, const std::map<std::string, std::string> &texts,
                      const std::map<std::string, std::string> &baseTexts = {});
// The ExtraLanguage in use, or null.
const ExtraLanguage *extraLanguage();
// The current language's own name (한국어, Tiếng Việt, ...).
const char *languageName();
// The codes to try, in order, for text the server gives in several
// languages: the asked one, simplified Chinese for traditional (an
// ExtraLanguage in use: its base), then English.
std::vector<std::string> languageChain(const std::string &code);
// Text the server sends in several languages, only the ones it sent.
struct LanguageText {
  std::string code, text;
};
using LanguageTexts = std::vector<LanguageText>;
// The text in `code`, or null.
const std::string *textIn(const LanguageTexts &texts, const std::string &code);
// The first of languageChain(code) the texts hold, or null.
const std::string *nearestText(const LanguageTexts &texts, const std::string &code);
// What a screen shows for `code`, or null for the server's own name: Korean
// takes only Korean (the server's name is Korean), the others nearestText.
const std::string *displayText(const LanguageTexts &texts, const std::string &code);
// The language's own name, as a picker shows it (한국어, English, 日本語, ...).
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
// Each source/i18n/lang_<code>.cpp registers its language's table with a static
// instance. English has none: its text is the key.
struct TranslationTable {
  TranslationTable(Language language, const Translation *rows, size_t count);
};
// A server's dictionary for `language` (GET /api/client/strings), in place
// of any it gave before: tr() shows its text over the built-in table's. Keys
// the tables do not have, locked keys (i18n::locked) and text failing
// i18n::checkText are left out, so those keep the built-in text. English
// takes none. Returns how many keys took the server's text. Texts given
// earlier stay allocated for the run: a tr() result may still be held.
size_t setServerTexts(Language language, const std::map<std::string, std::string> &texts);
// Whether a key is in any table (screen text in the code must be).
bool hasTranslation(const std::string &key);
// Every row registered for `language`, in table order (the tests check them).
std::vector<Translation> translations(Language language);
} // namespace foil
