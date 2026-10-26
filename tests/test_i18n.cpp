#include "Test.hpp"
#include "core/I18n.hpp"
#include "core/I18nChecks.hpp"
#include "core/Json.hpp"
#include <SDL2/SDL_ttf.h>
#include <cstdint>
#include <filesystem>
#include <set>
#include <sstream>
namespace {
// Korean, Japanese or Chinese: Hangul, kana, CJK ideographs and their
// punctuation and full-width forms. Everything outside the tables is English.
bool cjk(const std::string &text) {
  for (size_t i = 0; i < text.size();) {
    const unsigned char c = text[i];
    const int length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    uint32_t point = length == 1 ? c : c & (0xFF >> (length + 1));
    for (int k = 1; k < length && i + k < text.size(); ++k)
      point = point << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    i += length;
    if ((point >= 0x1100 && point <= 0x11FF) || (point >= 0x3000 && point <= 0x30FF) ||
        (point >= 0x3130 && point <= 0x318F) || (point >= 0x3400 && point <= 0x4DBF) ||
        (point >= 0x4E00 && point <= 0x9FFF) || (point >= 0xAC00 && point <= 0xD7A3) ||
        (point >= 0xFF00 && point <= 0xFFEF))
      return true;
  }
  return false;
}
// String literals on one line, outside // comments, with what precedes each.
struct Literal {
  std::string text, before;
};
std::vector<Literal> literals(const std::string &line) {
  std::vector<Literal> out;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '/' && i + 1 < line.size() && line[i + 1] == '/')
      break;
    if (line[i] == '\'' && i + 2 < line.size()) { // skip '"' char literals
      i += line[i + 1] == '\\' ? 3 : 2;
      continue;
    }
    if (line[i] != '"')
      continue;
    const auto start = i;
    std::string text;
    for (++i; i < line.size() && line[i] != '"'; ++i) {
      if (line[i] == '\\' && i + 1 < line.size()) {
        const char next = line[++i];
        text += next == 'n' ? '\n' : next == 't' ? '\t' : next;
        continue;
      }
      text += line[i];
    }
    // R"(...)" raw strings (SVG icons) carry no screen text.
    if (start > 0 && line[start - 1] == 'R')
      continue;
    out.push_back({text, line.substr(0, start)});
  }
  return out;
}
bool wrapped(std::string before) {
  while (!before.empty() && before.back() == ' ')
    before.pop_back();
  auto ends = [&](const char *tail) {
    const std::string t = tail;
    return before.size() >= t.size() && before.compare(before.size() - t.size(), t.size(), t) == 0;
  };
  return ends("tr(") || ends("trf(");
}
} // namespace
int runI18nTests() {
  int failures = 0;
  namespace fs = std::filesystem;
  // Every language's table has the same keys as every other, once each,
  // with text that keeps the English placeholders.
  std::set<std::string> keys;
  for (const auto language : foil::translatedLanguages())
    for (const auto &row : foil::translations(language))
      keys.insert(row.key);
  for (const auto language : foil::translatedLanguages()) {
    std::set<std::string> seen;
    for (const auto &row : foil::translations(language)) {
      if (!seen.insert(row.key).second)
        std::cerr << foil::languageCode(language) << ": duplicate translation key: " << row.key << '\n';
      if (!row.text || !*row.text)
        std::cerr << foil::languageCode(language) << ": empty translation: " << row.key << '\n';
      CHECK(row.text && *row.text);
      const bool same = foil::i18n::placeholders(row.text ? row.text : "") == foil::i18n::placeholders(foil::english(row.key));
      if (!same)
        std::cerr << foil::languageCode(language) << ": placeholder mismatch: " << row.key << '\n';
      CHECK(same);
    }
    CHECK(seen.size() == foil::translations(language).size());
    for (const auto &key : keys)
      if (!seen.count(key))
        std::cerr << foil::languageCode(language) << ": no translation: " << key << '\n';
    CHECK(seen == keys);
  }
  CHECK(cjk("설정") && cjk("ダウンロード") && cjk("下载") && cjk("「設定」") && cjk("ＡＢ"));
  CHECK(!cjk("Settings · 3 files → v2 “quoted” …"));
  // Keys are the English text: no Korean, Japanese or Chinese in them.
  int foreignKeys = 0;
  for (const auto &key : keys)
    if (cjk(key)) {
      std::cerr << "translation key not in English: " << key << '\n';
      ++foreignKeys;
    }
  CHECK(foreignKeys == 0);
  // Screen text in the code is an English key in tr()/trf() (or a key kept
  // for later, marked "i18n: keep") with a row in the tables; Korean,
  // Japanese and Chinese live only in the tables.
  int unwrapped = 0, missing = 0;
  for (const auto *root : {"source", "include", "engine-shim"}) {
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
      const auto path = entry.path().string();
      if (!entry.is_regular_file() || path.find("source/i18n/") != std::string::npos ||
          path.find("I18n.") != std::string::npos)
        continue;
      std::istringstream lines(foil::readFile(path));
      std::string line;
      int number = 0;
      while (std::getline(lines, line)) {
        ++number;
        // "i18n: as is": text shown or matched the same in every language.
        if (line.find("i18n: as is") != std::string::npos)
          continue;
        const bool kept = line.find("i18n: keep") != std::string::npos;
        for (const auto &literal : literals(line)) {
          if (!kept && !wrapped(literal.before)) {
            if (cjk(literal.text)) {
              std::cerr << path << ':' << number << ": Korean, Japanese or Chinese text outside the tables: " << literal.text << '\n';
              ++unwrapped;
            }
          } else if (!literal.text.empty() && !foil::hasTranslation(literal.text)) {
            std::cerr << path << ':' << number << ": no translation: " << literal.text << '\n';
            ++missing;
          }
        }
      }
    }
  }
  CHECK(unwrapped == 0);
  CHECK(missing == 0);
  // Every character of every language's text, and of the picker's names, is
  // in a bundled font: the console's own fonts lack weights and some scripts.
  {
    std::vector<TTF_Font *> fonts;
    for (const auto *file : {"CookingFoilSans-%s.otf", "CookingFoilSansJP-%s.otf", "CookingFoilSansSC-%s.ttf", "CookingFoilSansTC-%s.ttf"})
      for (const auto *weight : {"Regular", "Bold"}) {
        char name[64];
        std::snprintf(name, sizeof name, file, weight);
        auto *font = TTF_OpenFont((std::string("host/fonts/") + name).c_str(), 18);
        CHECK(font != nullptr);
        if (font) fonts.push_back(font);
      }
    auto covered = [&](uint32_t point) {
      if (point < 0x20 || point == 0xFE0F) return true; // control and variation selectors draw nothing
      for (auto *font : fonts)
        if (TTF_GlyphIsProvided32(font, point)) return true;
      return false;
    };
    std::set<uint32_t> missing;
    auto check = [&](const std::string &text) {
      for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        const int length = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        uint32_t point = length == 1 ? c : c & (0xFF >> (length + 1));
        for (int k = 1; k < length && i + k < text.size(); ++k)
          point = point << 6 | (static_cast<unsigned char>(text[i + k]) & 0x3F);
        i += length;
        if (!covered(point)) missing.insert(point);
      }
    };
    for (int i = 0; i < foil::LanguageCount; ++i) {
      check(foil::languageName(foil::Language(i)));
      for (const auto &row : foil::translations(foil::Language(i))) check(row.text);
    }
    for (const auto &key : keys) check(foil::english(key.c_str()));
    for (const auto point : missing)
      std::cerr << "no bundled glyph for U+" << std::hex << point << std::dec << '\n';
    CHECK(missing.empty());
    for (auto *font : fonts) TTF_CloseFont(font);
  }
  // Lookups follow the chosen language and fill placeholders after it.
  const foil::Translation korean[] = {{"Sample {0}|test", "테스트 {0}개"}};
  const foil::Translation japanese[] = {{"Sample {0}|test", "テスト {0}"}};
  static const foil::TranslationTable koreanTable(foil::Language::Korean, korean, 1);
  static const foil::TranslationTable japaneseTable(foil::Language::Japanese, japanese, 1);
  foil::setLanguage(foil::Language::Japanese);
  CHECK(foil::trf("Sample {0}|test", {"3"}) == "テスト 3");
  CHECK(std::string(foil::tr("Not in any table|test")) == "Not in any table");
  foil::setLanguage(foil::Language::English);
  CHECK(foil::trf("Sample {0}|test", {"3"}) == "Sample 3");
  foil::setLanguage(foil::Language::Korean);
  CHECK(foil::trf("Sample {0}|test", {"3"}) == "테스트 3개");
  foil::setLanguage(foil::Language::Chinese); // a language whose table lacks the key shows the English
  CHECK(foil::trf("Sample {0}|test", {"3"}) == "Sample 3");
  foil::setLanguage(foil::Language::Korean);
  CHECK(foil::languageFromCode("zh", foil::Language::Korean) == foil::Language::Chinese);
  CHECK(foil::languageFromCode("xx", foil::Language::English) == foil::Language::English);
  // Server text in a key's place: the checks it passes before it shows.
  {
    using foil::i18n::Problem;
    using foil::i18n::checkText;
    CHECK(checkText("Delete {0}?|test", "{0} 삭제?") == Problem::None);
    CHECK(checkText("Name|sort", "이름순") == Problem::None);
    CHECK(checkText("Name|sort", "") == Problem::Empty);
    CHECK(checkText("Delete {0}?|test", "삭제?") == Problem::Placeholders);
    CHECK(checkText("Delete {0}?|test", "{0} {1} 삭제?") == Problem::Placeholders);
    CHECK(checkText("Name|sort", "이름\t순") == Problem::Control);
    CHECK(checkText("Name|sort", "\xE2\x80\xAE" "eman") == Problem::Control); // right-to-left override
    CHECK(checkText("Two lines|test", "한 줄\n두 줄") == Problem::None);
    CHECK(checkText("OK|test", std::string(16, 'a')) == Problem::None); // short keys get 16 characters
    CHECK(checkText("OK|test", std::string(17, 'a')) == Problem::TooLong);
    CHECK(foil::i18n::englishOf("Name|sort") == "Name" && foil::i18n::englishOf("Name") == "Name");
    CHECK(foil::i18n::undrawable(0x0627) && foil::i18n::undrawable(0x0E01) && foil::i18n::undrawable(0x1100));
    CHECK(!foil::i18n::undrawable('a') && !foil::i18n::undrawable(0xAC00) && !foil::i18n::undrawable(0x0410));
    CHECK((foil::i18n::codePoints("a한") == std::vector<uint32_t>{'a', 0xD55C}));
  }
  // Every key a server may not reword is one the app draws, once.
  {
    std::set<std::string> lockedSeen;
    for (const auto *key : foil::i18n::lockedKeys()) {
      if (!foil::hasTranslation(key)) std::cerr << "locked key not in the tables: " << key << '\n';
      CHECK(foil::hasTranslation(key));
      CHECK(lockedSeen.insert(key).second);
      CHECK(foil::i18n::locked(key));
    }
    CHECK(!foil::i18n::locked("Name|sort"));
  }
  return failures;
}
